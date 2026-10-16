package prism.bridge.nms;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * One-shot NMS preparation, run by CraftPP in a short-lived child JVM before it
 * embeds the real one (the embedded JVM's class path is fixed at creation).
 * Scans the plugin jars for references to net/minecraft or org/bukkit/craftbukkit;
 * if any, downloads Mojang's 26.3 server jar into cache/ (SHA-1 verified, as
 * paperclip does), unpacks it and its libraries Paper-style into versions/ and
 * libraries/, and access-widens the server jar (see {@link Widen}).
 *
 * <p>Usage: {@code Prepare <serverDir> <outFile> <plugin jars...>}. Writes
 * {@code outFile}: the cache key (read from {@code outFile.key}) on the first line,
 * then one class path entry per line (none when no plugin needs NMS).
 */
public final class Prepare {
    static final String VERSION = "26.3";
    static final String BUNDLER_URL = "https://piston-data.mojang.com/v1/objects/33680f5f2ac32864d6d7cf5e56a705fdb3e05f4c/server.jar";
    static final String BUNDLER_SHA1 = "33680f5f2ac32864d6d7cf5e56a705fdb3e05f4c";
    private static final byte[][] MARKERS = {"net/minecraft/".getBytes(StandardCharsets.US_ASCII),
            "org/bukkit/craftbukkit/".getBytes(StandardCharsets.US_ASCII)};

    public static void main(String[] args) throws Exception {
        Path server = Path.of(args[0]);
        Path out = Path.of(args[1]);
        List<String> jars = List.of(args).subList(2, args.length);
        List<String> classpath = new ArrayList<>();
        if (needsNms(jars)) {
            System.out.println("Plugins reference server internals (NMS); preparing Minecraft " + VERSION + " server classes");
            classpath = prepare(server);
        }
        String key = Files.readString(Path.of(out + ".key")).strip();
        StringBuilder text = new StringBuilder(key).append('\n');
        for (String entry : classpath) {
            text.append(entry).append('\n');
        }
        Files.writeString(out, text);
    }

    static boolean needsNms(List<String> jars) {
        for (String jar : jars) {
            try (ZipFile zip = new ZipFile(jar)) {
                for (var entries = zip.entries(); entries.hasMoreElements(); ) {
                    ZipEntry entry = entries.nextElement();
                    if (!entry.getName().endsWith(".class")) {
                        continue;
                    }
                    try (InputStream in = zip.getInputStream(entry)) {
                        byte[] bytes = in.readAllBytes();
                        for (byte[] marker : MARKERS) {
                            if (indexOf(bytes, marker) >= 0) {
                                return true;
                            }
                        }
                    }
                }
            } catch (IOException e) {
                // not a readable jar; the plugin loader reports it properly later
            }
        }
        return false;
    }

    private static int indexOf(byte[] data, byte[] needle) {
        outer:
        for (int i = 0; i + needle.length <= data.length; i++) {
            for (int j = 0; j < needle.length; j++) {
                if (data[i + j] != needle[j]) {
                    continue outer;
                }
            }
            return i;
        }
        return -1;
    }

    static List<String> prepare(Path server) throws Exception {
        Path bundler = server.resolve("cache/mojang_" + VERSION + ".jar");
        if (!Files.exists(bundler)) {
            Files.createDirectories(bundler.getParent());
            Path local = server.resolve("vanilla/server.jar"); // CraftPP dev checkouts already have it
            Path tmp = Path.of(bundler + ".tmp");
            if (Files.exists(local)) {
                Files.copy(local, tmp, StandardCopyOption.REPLACE_EXISTING);
            } else {
                System.out.println("Downloading mojang_" + VERSION + ".jar");
                try (InputStream in = URI.create(BUNDLER_URL).toURL().openStream()) {
                    Files.copy(in, tmp, StandardCopyOption.REPLACE_EXISTING);
                }
            }
            String sha1 = HexFormat.of().formatHex(MessageDigest.getInstance("SHA-1").digest(Files.readAllBytes(tmp)));
            if (!sha1.equals(BUNDLER_SHA1)) {
                Files.delete(tmp);
                throw new IOException("mojang_" + VERSION + ".jar failed verification (sha1 " + sha1 + ")");
            }
            Files.move(tmp, bundler, StandardCopyOption.REPLACE_EXISTING);
        }
        List<String> classpath = new ArrayList<>();
        try (ZipFile zip = new ZipFile(bundler.toFile())) {
            for (String line : list(zip, "META-INF/versions.list")) {
                Path jar = extract(zip, "META-INF/versions/" + line, server.resolve("versions/" + line));
                Path widened = Path.of(jar.toString().replace(".jar", "-craftpp.jar"));
                if (!Files.exists(widened)) {
                    System.out.println("Applying access patches to " + jar.getFileName());
                    Widen.main(new String[] {jar.toString(), widened.toString()});
                }
                classpath.add(widened.toAbsolutePath().toString());
            }
            for (String line : list(zip, "META-INF/libraries.list")) {
                classpath.add(extract(zip, "META-INF/libraries/" + line, server.resolve("libraries/" + line)).toAbsolutePath().toString());
            }
        }
        return classpath;
    }

    private static List<String> list(ZipFile zip, String name) throws IOException {
        List<String> paths = new ArrayList<>();
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(zip.getInputStream(zip.getEntry(name)), StandardCharsets.UTF_8))) {
            for (String line; (line = reader.readLine()) != null; ) {
                String[] parts = line.split("\t"); // sha256, maven coordinate, path
                if (parts.length == 3) {
                    paths.add(parts[2]);
                }
            }
        }
        return paths;
    }

    private static Path extract(ZipFile zip, String entry, Path target) throws IOException {
        if (!Files.exists(target)) {
            Files.createDirectories(target.getParent());
            Path tmp = Path.of(target + ".tmp");
            try (InputStream in = zip.getInputStream(zip.getEntry(entry))) {
                Files.copy(in, tmp, StandardCopyOption.REPLACE_EXISTING);
            }
            Files.move(tmp, target, StandardCopyOption.REPLACE_EXISTING);
        }
        return target;
    }
}
