# Native methods are registered from C++ with RegisterNatives. Keep their
# names and the exception type when the consuming app enables minification.
-keepclasseswithmembernames class * {
    native <methods>;
}
-keep class io.captrim.CapTrimException { *; }
-keep class io.captrim.ProgressCallback { *; }
-keep class io.captrim.CapTrimNative { *; }
