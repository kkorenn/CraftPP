package prism.bridge;

import org.bukkit.plugin.InvalidDescriptionException;
import org.bukkit.plugin.PluginDescriptionFile;
import org.yaml.snakeyaml.Yaml;

import java.io.InputStream;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Converts a paper-plugin.yml into the plugin.yml shape the rest of the loader
 * understands. Bootstrappers and custom loaders are not run (logged instead).
 */
final class PaperPluginYaml {
    static final Map<String, String> BOOTSTRAPPERS = new java.util.concurrent.ConcurrentHashMap<>();
    static final Map<String, String> LOADERS = new java.util.concurrent.ConcurrentHashMap<>();

    private PaperPluginYaml() {
    }

    @SuppressWarnings("unchecked")
    static PluginDescriptionFile toDescription(InputStream stream) throws InvalidDescriptionException {
        Map<String, Object> root = new Yaml().load(stream);
        if (root == null || !root.containsKey("name") || !root.containsKey("main")) {
            throw new InvalidDescriptionException("paper-plugin.yml is missing name or main");
        }
        Map<String, Object> converted = new LinkedHashMap<>();
        for (String key : List.of("name", "main", "version", "description", "author", "authors", "contributors",
                "website", "prefix", "load", "api-version", "provides", "permissions", "default-permission", "commands")) {
            if (root.containsKey(key)) {
                converted.put(key, root.get(key));
            }
        }
        if (root.containsKey("bootstrapper")) {
            BOOTSTRAPPERS.put(String.valueOf(root.get("name")), String.valueOf(root.get("bootstrapper")));
        }
        if (root.containsKey("loader")) {
            LOADERS.put(String.valueOf(root.get("name")), String.valueOf(root.get("loader")));
        }
        List<String> depend = new ArrayList<>();
        List<String> softDepend = new ArrayList<>();
        List<String> loadBefore = new ArrayList<>();
        Object dependencies = root.get("dependencies");
        if (dependencies instanceof Map<?, ?> byTarget) {
            for (Object section : byTarget.values()) {
                if (!(section instanceof Map<?, ?> entries)) {
                    continue;
                }
                for (Map.Entry<?, ?> entry : entries.entrySet()) {
                    String name = String.valueOf(entry.getKey());
                    boolean required = true;
                    String load = "OMIT";
                    if (entry.getValue() instanceof Map<?, ?> options) {
                        if (options.get("required") instanceof Boolean flag) {
                            required = flag;
                        }
                        if (options.get("load") != null) {
                            load = String.valueOf(options.get("load")).toUpperCase(java.util.Locale.ROOT);
                        }
                    }
                    if (load.equals("AFTER")) {
                        loadBefore.add(name);
                    } else if (required) {
                        depend.add(name);
                    } else {
                        softDepend.add(name);
                    }
                }
            }
        }
        if (!depend.isEmpty()) {
            converted.put("depend", depend);
        }
        if (!softDepend.isEmpty()) {
            converted.put("softdepend", softDepend);
        }
        if (!loadBefore.isEmpty()) {
            converted.put("loadbefore", loadBefore);
        }
        String yaml = new Yaml().dump(converted);
        return new PluginDescriptionFile(new java.io.StringReader(yaml));
    }
}
