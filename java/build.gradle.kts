import java.net.URI

plugins { java }

// Build output lives in the CMake build dir (../build/java) so the repo stays clean.
layout.buildDirectory.set(file("../build/java"))

repositories {
    mavenCentral()
    maven("https://repo.papermc.io/repository/maven-public/")
}

val paperApiVersion = "26.3.build.195-stable"

dependencies {
    implementation("io.papermc.paper:paper-api:$paperApiVersion")
    // API type-use annotations must be present when plugins compile against the bundled API.
    implementation("org.jetbrains:annotations:26.0.2")
    // plugin.yml `libraries:` and paper-plugin loaders resolve from Maven Central at runtime.
    runtimeOnly("org.apache.maven.resolver:maven-resolver-connector-basic:1.9.18")
    runtimeOnly("org.apache.maven.resolver:maven-resolver-transport-http:1.9.18")
    implementation("org.slf4j:slf4j-jdk14:2.0.17")
    // Pulled in transitively by the excluded Maven resolver, but used directly by the API.
    implementation("org.apache.commons:commons-lang3:3.17.0")
}

// The official (unobfuscated since 26.x) Mojang server jar: compile-time only, for the
// NMS layer (prism.bridge.nms) and the CraftBukkit-shaped classes. Never bundled;
// CraftPP downloads it into the server's cache/ at runtime like paperclip does.
val vanillaDir = layout.buildDirectory.dir("vanilla")
val downloadVanilla by tasks.registering {
    val dir = vanillaDir.get().asFile
    outputs.dir(dir)
    doLast {
        val bundler = dir.resolve("server.jar")
        val local = rootDir.resolve("../vanilla/server.jar") // README setup step already fetched it
        if (!bundler.exists() && local.exists()) { dir.mkdirs(); local.copyTo(bundler) }
        if (!bundler.exists()) {
            dir.mkdirs()
            println("Downloading Minecraft 26.3 server jar")
            URI("https://piston-data.mojang.com/v1/objects/33680f5f2ac32864d6d7cf5e56a705fdb3e05f4c/server.jar")
                .toURL().openStream().use { input -> bundler.outputStream().use { output -> input.copyTo(output) } }
        }
        if (!dir.resolve("server-26.3.jar").exists()) {
            copy {
                from(zipTree(bundler)) { include("META-INF/versions/26.3/server-26.3.jar"); eachFile { path = name } }
                into(dir); includeEmptyDirs = false
            }
            copy {
                from(zipTree(bundler)) { include("META-INF/libraries/**"); eachFile { path = path.removePrefix("META-INF/") } }
                into(dir); includeEmptyDirs = false
            }
        }
    }
}
dependencies {
    compileOnly(files(vanillaDir.map { it.file("server-26.3.jar") }))
    compileOnly(fileTree(vanillaDir.get().asFile.resolve("libraries")) { include("**/*.jar") })
}
tasks.compileJava { dependsOn(downloadVanilla) }

// Paper-style access-widened jar (what NMS plugins compile against); used by testplugin.
val widenVanilla by tasks.registering(JavaExec::class) {
    dependsOn(downloadVanilla, tasks.compileJava)
    val input = vanillaDir.get().asFile.resolve("server-26.3.jar")
    val output = vanillaDir.get().asFile.resolve("server-26.3-widened.jar")
    inputs.file(input); outputs.file(output)
    classpath = sourceSets["main"].output + sourceSets["main"].runtimeClasspath
    mainClass.set("prism.bridge.nms.Widen")
    args(input.absolutePath, output.absolutePath)
}

java { toolchain { languageVersion.set(JavaLanguageVersion.of(25)) } }

tasks.withType<JavaCompile> { options.encoding = "UTF-8"; options.release.set(25) }

// One self-contained jar: bridge classes + paper-api + every runtime dependency.
tasks.jar {
    archiveFileName.set("craftpp-bridge.jar")
    manifest { attributes("Main-Class" to "prism.bridge.Main", "Paper-API-Version" to paperApiVersion) }
    duplicatesStrategy = DuplicatesStrategy.EXCLUDE
    from(configurations.runtimeClasspath.get().map { if (it.isDirectory) it else zipTree(it) }) {
        exclude("META-INF/*.SF", "META-INF/*.DSA", "META-INF/*.RSA", "META-INF/versions/**/module-info.class", "module-info.class")
    }
}
