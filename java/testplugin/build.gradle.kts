plugins { java }
layout.buildDirectory.set(file("../../build/java/testplugin"))

repositories {
    mavenCentral()
    maven("https://repo.papermc.io/repository/maven-public/")
}

val vanillaDir = rootProject.layout.buildDirectory.dir("vanilla")
dependencies {
    compileOnly("io.papermc.paper:paper-api:26.3.build.195-stable")
    compileOnly(files(vanillaDir.map { it.file("server-26.3-widened.jar") })) // Paper-style public NMS members.
    compileOnly(fileTree(vanillaDir.get().asFile.resolve("libraries")) { include("**/*.jar") })
}
tasks.compileJava { dependsOn(":widenVanilla") }

java { toolchain { languageVersion.set(JavaLanguageVersion.of(25)) } }
tasks.withType<JavaCompile> { options.encoding = "UTF-8"; options.release.set(25) }
tasks.jar { archiveFileName.set("PrismTestPlugin.jar") }
