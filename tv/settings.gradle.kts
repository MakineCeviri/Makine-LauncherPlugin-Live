// Makine TV — Android TV altyazı katmanı (konsol / TV modu). Ayrıntı: docs/MIMARI.md §5, docs/KONSOL_TV.md
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}
rootProject.name = "makine-tv"
include(":app")
