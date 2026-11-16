import org.jetbrains.kotlin.gradle.dsl.JvmTarget

// Minimal self-check host for the AAR: it exercises the Kotlin/JNI layer
// end-to-end on a device, mirroring platforms/ohos SelfCheck.ets. No AndroidX
// dependencies on purpose — a plain framework Activity keeps the build thin.
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "io.captrim.selfcheck"
    compileSdk = 35

    defaultConfig {
        applicationId = "io.captrim.selfcheck"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
        ndk { abiFilters += listOf("arm64-v8a") }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        release { isMinifyEnabled = false }
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(project(":captrim"))
}
