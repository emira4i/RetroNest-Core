import org.jetbrains.kotlin.gradle.dsl.JvmTarget

// The core host: native libretro bridge, the emulation engine, the prebuilt/self-built cores and
// the service that runs them in the ":core" process. License: GPL-2.0-or-later (see LICENSE),
// because it loads and ships GPL cores. It talks to the closed app ONLY over the binder protocol
// in :corehost-api and must never depend on :app.
plugins {
    id("com.android.library")
}

android {
    namespace = "com.retronest.corehost.engine"
    compileSdk = 37

    // Pinned to the NDK installed on the dev machine (see docs/android-studio-setup.md).
    ndkVersion = "30.0.15729638"

    defaultConfig {
        minSdk = 26
        consumerProguardFiles("consumer-rules.pro")
        // On-device core smoke test (src/androidTest, :corehost:connectedDebugAndroidTest). The test APK is
        // self-instrumenting (package com.retronest.corehost.engine.test): it never targets the app.
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            // arm64-v8a only: no 32-bit cores are bundled, so an armeabi-v7a lib would make Play offer the app to devices where nothing runs.
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                cFlags += listOf("-std=c11", "-Wall", "-Wextra")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            // Pinned to the CMake installed via SDK Manager.
            version = "4.1.2"
        }
    }

    // Pre-compiled / self-built Libretro cores live in corehost/libs/<abi>/ and are packaged as jniLibs.
    // (The app module sets packaging.jniLibs.useLegacyPackaging so they are extracted to nativeLibraryDir.)
    sourceSets {
        getByName("main") {
            jniLibs.directories.add("libs")
        }
    }

    testOptions {
        targetSdk = 36
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(project(":corehost-api"))
    implementation("androidx.annotation:annotation:1.9.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")

    testImplementation("junit:junit:4.13.2")

    androidTestImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test:runner:1.6.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
}

// Test APK only (the AAR and the app are untouched): extract the cores to nativeLibraryDir so the bridge can
// dlopen() them by path, exactly like the app does, and leave out the cores the smoke test does not run
// (MAME alone is ~180 MB).
val smokeTestUnusedCores = listOf(
    "mame", "mame2003_plus", "mame2010", "mupen64plus_next_gles3", "ppsspp", "snes9x", "genesis_plus_gx",
)
androidComponents {
    onVariants { variant ->
        variant.deviceTests.values.forEach { deviceTest ->
            deviceTest.packaging.jniLibs.useLegacyPackaging.set(true)
            smokeTestUnusedCores.forEach { deviceTest.packaging.jniLibs.excludes.add("**/lib${it}_libretro_android.so") }
        }
    }
}

// Test-only core for ShutdownRequestTest (src/androidTest/cpp/shutdown_test_core.c): compiled with the NDK's
// clang into build/testCores and packaged as a jniLib of the androidTest APK only. It never reaches the AAR
// or the app.
val testCoresDir = layout.buildDirectory.dir("testCores/jniLibs")
val buildShutdownTestCore = tasks.register<Exec>("buildShutdownTestCore") {
    val source = layout.projectDirectory.file("src/androidTest/cpp/shutdown_test_core.c")
    val header = layout.projectDirectory.dir("src/main/cpp")
    val output = testCoresDir.map { it.file("arm64-v8a/libshutdowntest_libretro_android.so") }
    val ndk = androidComponents.sdkComponents.ndkDirectory
    inputs.file(source)
    inputs.file(header.file("libretro.h"))
    outputs.file(output)
    doFirst {
        val prebuilt = ndk.get().dir("toolchains/llvm/prebuilt").asFile
        val host = prebuilt.listFiles()?.firstOrNull { it.isDirectory } ?: error("No NDK toolchain in $prebuilt")
        val clang = host.resolve("bin").listFiles()?.firstOrNull { it.name == "clang" || it.name == "clang.exe" }
            ?: error("No clang in $host")
        output.get().asFile.parentFile.mkdirs()
        commandLine(
            clang.path, "--target=aarch64-linux-android26", "-shared", "-fPIC", "-O1", "-std=c11", "-Wall", "-Wextra",
            "-Wl,-z,max-page-size=16384", "-I", header.asFile.path, "-o", output.get().asFile.path, source.asFile.path,
        )
    }
}
android.sourceSets.getByName("androidTest").jniLibs.directories.add(testCoresDir.get().asFile.path)
tasks.matching { it.name.startsWith("merge") && it.name.endsWith("AndroidTestJniLibFolders") }.configureEach {
    dependsOn(buildShutdownTestCore)
}
