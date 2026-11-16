//! Export-plan ABI.
//!
//! The platform does not need to know how a timeline becomes segments; it only
//! needs a stable JSON snapshot built from the same Rust core that drives the
//! preview. Returning JSON keeps the C ABI small while still letting a host
//! app cache the plan and walk frames without a cross-boundary call each time.

use crate::{captrim_engine_t, engine_from, ffi_guard, lock, owned_cstring};
use captrim_core::{plan_export, Error};
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
