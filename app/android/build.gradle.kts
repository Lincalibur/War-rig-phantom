allprojects {
    repositories {
        google()
        mavenCentral()
    }
}

val newBuildDir: Directory =
    rootProject.layout.buildDirectory
        .dir("../../build")
        .get()
rootProject.layout.buildDirectory.value(newBuildDir)

subprojects {
    val newSubprojectBuildDir: Directory = newBuildDir.dir(project.name)
    project.layout.buildDirectory.value(newSubprojectBuildDir)
}
subprojects {
    project.evaluationDependsOn(":app")
}

// This Flutter's default compileSdk is 33, but some plugins (flutter_blue_plus's
// transitive androidx deps) require 34+. Force plugin modules to compileSdk 36.
// (:app is pre-evaluated by the block above and already sets compileSdk 36 itself.)
subprojects {
    if (project.name != "app" && !state.executed) {
        afterEvaluate {
            val androidExtension = project.extensions.findByName("android")
            if (androidExtension != null) {
                try {
                    androidExtension.javaClass
                        .getMethod("setCompileSdkVersion", Int::class.javaPrimitiveType)
                        .invoke(androidExtension, 36)
                } catch (_: Throwable) { }
            }
        }
    }
}

tasks.register<Delete>("clean") {
    delete(rootProject.layout.buildDirectory)
}
