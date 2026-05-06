import java.io.ByteArrayOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}


fun gitShortSha(): String {
    return try {
        val stdout = ByteArrayOutputStream()
        exec {
            commandLine("git", "rev-parse", "--short", "HEAD")
            standardOutput = stdout
        }
        stdout.toString().trim().ifEmpty { "nogit" }
    } catch (_: Exception) {
        "nogit"
    }
}

fun buildStamp(): String = SimpleDateFormat("yyyyMMdd-HHmm", Locale.US).format(Date())

android {
    namespace = "com.namnh.awesomecam"
    compileSdk = 34
    buildToolsVersion = "36.1.0"

    defaultConfig {
        applicationId = "com.namnh.awesomecam"
        minSdk = 28
        targetSdk = 34
        versionCode = 18
        versionName = "2.7"
        buildConfigField("String", "GIT_SHA", "\"${gitShortSha()}\"")
        buildConfigField("String", "BUILD_STAMP", "\"${buildStamp()}\"")
        buildConfigField("String", "ARCH_LABEL", "\"ffmpeg-native-player\"")

        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                cppFlags += ""
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    buildFeatures {
        prefab = true
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }

    kotlinOptions {
        jvmTarget = "1.8"
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/jni/CMakeLists.txt")
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.12.0")
    implementation("androidx.appcompat:appcompat:1.6.1")
    implementation("com.google.android.material:material:1.11.0")
    implementation("com.bytedance.android:shadowhook:1.0.10")
    implementation("io.github.yearsyan:ffmpeg-standard:7.1-beta.16")
}

tasks.register("buildDebug16kApk") {
    group = "build"
    description = "Create 16KB-page compatible debug APK (ELF + APK alignment)."
    mustRunAfter("assembleDebug")

    doLast {
        val buildToolsDir = android.sdkDirectory.resolve("build-tools/${android.buildToolsVersion}")
        val zipalign = buildToolsDir.resolve("zipalign")
        val apksigner = buildToolsDir.resolve("apksigner")
        val debugKeystore = File(System.getProperty("user.home"), ".android/debug.keystore")
        val apkDir = layout.buildDirectory.dir("outputs/apk/debug").get().asFile
        val inputApk = apkDir.resolve("app-debug.apk")
        val alignedApk = apkDir.resolve("app-debug-16k-aligned-unsigned.apk")
        val outputApk = apkDir.resolve("app-debug-16k.apk")

        delete(alignedApk, outputApk)
        exec {
            commandLine(
                zipalign.absolutePath,
                "-f",
                "-P",
                "16",
                "4",
                inputApk.absolutePath,
                alignedApk.absolutePath,
            )
        }
        exec {
            commandLine(
                apksigner.absolutePath,
                "sign",
                "--ks",
                debugKeystore.absolutePath,
                "--ks-pass",
                "pass:android",
                "--key-pass",
                "pass:android",
                "--out",
                outputApk.absolutePath,
                alignedApk.absolutePath,
            )
        }
        exec {
            commandLine(apksigner.absolutePath, "verify", "--verbose", outputApk.absolutePath)
        }
        exec {
            commandLine(zipalign.absolutePath, "-c", "-P", "16", "4", outputApk.absolutePath)
        }
        copy {
            from(outputApk)
            into(apkDir)
            rename { "app-debug.apk" }
        }
        println("16KB-compatible APK: ${outputApk.absolutePath}")
        println("Default debug APK overwritten with 16KB-compatible build: ${inputApk.absolutePath}")
    }
}

afterEvaluate {
    tasks.named("assembleDebug") {
        finalizedBy("buildDebug16kApk")
    }
}
