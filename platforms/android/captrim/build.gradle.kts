import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "io.captrim"
    compileSdk = 35

    defaultConfig {
        minSdk = 26 // AAudio / AHardwareBuffer baseline
        consumerProguardFiles("consumer-rules.pro")

        // Only ship ABIs that were actually built by scripts/build-rust.sh.
        ndk { abiFilters += listOf("arm64-v8a") }

        externalNativeBuild {
            cmake {
                // c++_static keeps libc++ out of the AAR, so integrating the
                // SDK never forces the host app to ship a matching
                // libc++_shared.so.
                arguments("-DANDROID_STL=c++_static", "-DCMAKE_CXX_STANDARD=17")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    sourceSets {
        getByName("main") {
            jniLibs.srcDirs("src/main/jniLibs")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

// Kotlin 2.x DSL. The old top-level `kotlinOptions { }` block was removed, so
// this is the form that works with the plugin version pinned in build.gradle.kts.
kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
}
