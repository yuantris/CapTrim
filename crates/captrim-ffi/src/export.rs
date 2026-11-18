//! Export-plan ABI.
//!
//! The platform does not need to know how a timeline becomes segments; it only
//! needs a stable JSON snapshot built from the same Rust core that drives the
//! preview. Returning JSON keeps the C ABI small while still letting a host
//! app cache the plan and walk frames without a cross-boundary call each time.

use crate::{captrim_engine_t, cstr, engine_from, ffi_guard, lock, owned_cstring};
use captrim_core::{effects_to_filter_desc, plan_export, Error};
use std::os::raw::c_char;
use std::ptr;

/// Full export breakdown as an `ExportPlan` JSON object.
#[no_mangle]
pub extern "C" fn captrim_engine_export_plan(
    engine: *mut captrim_engine_t,
    out_json: *mut *mut c_char,
) -> i32 {
    ffi_guard(|| {
        if out_json.is_null() {
            return Err(Error::invalid("out_json is null"));
        }
        unsafe { *out_json = ptr::null_mut() };
        let e = engine_from(engine)?;
        let json = {
            let editor = lock(&e.editor)?;
            plan_export(editor.timeline()).to_json()?
        };
        unsafe { *out_json = owned_cstring(json)? };
        Ok(0)
    })
}

/// Translate a clip's `effects` list (a JSON array of names, exactly as it
/// appears in `ExportVideoItem.effects`) into the FFmpeg `filter_desc` chain
/// for `captrim_engine_export_transcode`.
///
/// Keeping the translation in the core means the platform never duplicates the
/// effect vocabulary or its constants: it reads `effects` from its cached
/// export plan, hands the array here, and passes the returned string to the
/// transcode call. An empty array yields an empty string, which transcode
/// treats as a passthrough. Release `out_desc` with `captrim_string_free()`.
#[no_mangle]
pub extern "C" fn captrim_effects_to_filter_desc(
    effects_json: *const c_char,
    out_desc: *mut *mut c_char,
) -> i32 {
    ffi_guard(|| {
        if out_desc.is_null() {
            return Err(Error::invalid("out_desc is null"));
        }
        unsafe { *out_desc = ptr::null_mut() };
        let json = unsafe { cstr(effects_json)? };
        let effects: Vec<String> = serde_json::from_str(json)
            .map_err(|e| Error::invalid(format!("effects must be a JSON string array: {e}")))?;
        let desc = effects_to_filter_desc(&effects)?;
        unsafe { *out_desc = owned_cstring(desc)? };
        Ok(0)
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::captrim_string_free;
    use std::ffi::CString;

    fn translate(json: &str) -> Result<String, i32> {
        let input = CString::new(json).unwrap();
        let mut out: *mut c_char = ptr::null_mut();
        let rc = captrim_effects_to_filter_desc(input.as_ptr(), &mut out);
        if rc != 0 {
            captrim_string_free(out);
            return Err(rc);
        }
        let s = unsafe { cstr(out) }.unwrap().to_string();
        captrim_string_free(out);
        Ok(s)
    }

    #[test]
    fn effects_json_translates_in_canonical_order() {
        assert_eq!(
            translate(r#"["blur","invert","hflip"]"#).unwrap(),
            "hflip,negate,boxblur=2:1"
        );
        assert_eq!(translate("[]").unwrap(), "");
    }

    #[test]
    fn effects_json_rejects_junk_and_unknown_names() {
        assert!(translate(r#"["sepia"]"#).is_err());
        assert!(translate(r#"{"effect":"hflip"}"#).is_err());
        assert!(translate("not json").is_err());
    }
}
