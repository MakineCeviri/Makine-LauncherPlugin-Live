plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Sürüm, PC uygulamasıyla aynı: ../../manifest.json
val appVersion: String = Regex("\"version\"\\s*:\\s*\"([^\"]+)\"")
    .find(rootProject.file("../manifest.json").readText())?.groupValues?.get(1) ?: "0.0.0"
val versionParts = appVersion.split(".").map { it.toIntOrNull() ?: 0 } + listOf(0, 0, 0)

// İmza anahtarı depoda DEĞİL: yol ve şifre ortam değişkeninden. Yoksa hata ayıklama anahtarıyla imzalanır.
val keystorePath: String? = System.getenv("MAKINE_TV_KEYSTORE")
val keystorePass: String? = System.getenv("MAKINE_TV_KEYSTORE_PASS")

android {
    namespace = "org.makineceviri.tv"
    compileSdk = 35

    defaultConfig {
        applicationId = "org.makineceviri.tv"
        minSdk = 26
        targetSdk = 35
        versionCode = versionParts[0] * 10000 + versionParts[1] * 100 + versionParts[2]
        versionName = appVersion
    }

    signingConfigs {
        if (keystorePath != null && keystorePass != null) {
            create("release") {
                storeFile = file(keystorePath)
                storePassword = keystorePass
                keyAlias = System.getenv("MAKINE_TV_KEY_ALIAS") ?: "makine-tv"
                keyPassword = keystorePass
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
            signingConfig = signingConfigs.findByName("release") ?: signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    sourceSets["main"].assets.srcDir(layout.buildDirectory.dir("generated/makine-assets"))
}

// Örnek altyazılar PC uygulaması ve portalla aynı kaynaktan: spec/sample_quotes.json
val copySamples by tasks.registering(Copy::class) {
    from(rootProject.file("../spec/sample_quotes.json"))
    into(layout.buildDirectory.dir("generated/makine-assets"))
}
tasks.named("preBuild") { dependsOn(copySamples) }
