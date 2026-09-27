plugins { id("com.android.application"); id("org.jetbrains.kotlin.android") }

android { namespace = "org.phonecam.connect"; compileSdk = 35
    defaultConfig { applicationId = "org.phonecam.connect"; minSdk = 26; targetSdk = 35; versionCode = 1; versionName = "0.1.0" }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.activity:activity-ktx:1.10.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
    implementation("androidx.security:security-crypto:1.1.0-alpha06")

    // WebRTC (Google's prebuilt M125)
    implementation("io.github.webrtc-sdk:android:125.6422.07")

    // Unit tests
    testImplementation("junit:junit:4.13.2")
}
