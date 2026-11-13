import org.jetbrains.kotlin.gradle.dsl.JvmTarget

// The generic binder protocol between the RetroNest app and its core host: two AIDL interfaces and
// the Bundle keys / constants / RetroPad button mask that go with them. License: MIT (see LICENSE).
// Protocol only: no emulator code, no app code.
plugins {
    id("com.android.library")
}

android {
    namespace = "com.retronest.corehost.api"
    compileSdk = 37

    defaultConfig {
        minSdk = 26
        consumerProguardFiles("consumer-rules.pro")
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        aidl = true
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}
