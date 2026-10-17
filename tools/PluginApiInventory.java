import com.google.gson.GsonBuilder;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Reflection inventory of core proxy surfaces; method presence is never treated as compatibility. */
class PluginApiInventory {
    public static void main(String[] args) throws Exception {
        String[][] surfaces = {
            {"org.bukkit.Server", "prism.bridge.ServerImpl"},
            {"org.bukkit.World", "prism.bridge.WorldImpl"},
            {"org.bukkit.entity.Entity", "prism.bridge.EntityImpl"},
            {"org.bukkit.entity.LivingEntity", "prism.bridge.EntityImpl"},
            {"org.bukkit.entity.Player", "prism.bridge.PlayerImpl"},
            {"org.bukkit.OfflinePlayer", "prism.bridge.PlayerImpl"},
            {"org.bukkit.block.Block", "prism.bridge.BlockImpl"},
            {"org.bukkit.block.BlockState", "prism.bridge.BlockImpl$StateImpl"},
            {"org.bukkit.Chunk", "prism.bridge.ChunkImpl"},
            {"org.bukkit.inventory.meta.ItemMeta", "prism.bridge.ItemMetaImpl"},
            {"org.bukkit.inventory.ItemFactory", "prism.bridge.ServerImpl$ItemFactoryImpl"}
        };
        ClassLoader loader = PluginApiInventory.class.getClassLoader();
        List<Object> inventory = new ArrayList<>();
        int total = 0, missing = 0;
        for (String[] surface : surfaces) {
            Class<?> api = Class.forName(surface[0], false, loader);
            Class<?> backing = Class.forName(surface[1], false, loader);
            List<Map<String, String>> methods = new ArrayList<>();
            int backed = 0, defaults = 0, absent = 0;
            Method[] sorted = api.getMethods();
            Arrays.sort(sorted, java.util.Comparator.comparing(Method::toGenericString));
            for (Method method : sorted) {
                if (Modifier.isStatic(method.getModifiers()) || method.isSynthetic()) continue;
                String status;
                try {
                    Method implementation = backing.getMethod(method.getName(), method.getParameterTypes());
                    if (!method.getReturnType().isAssignableFrom(implementation.getReturnType())) {
                        status = "missing";
                        absent++;
                    } else {
                        status = "backing_unverified";
                        backed++;
                    }
                } catch (NoSuchMethodException e) {
                    if (method.isDefault()) {
                        status = "default_unverified";
                        defaults++;
                    } else {
                        status = "missing";
                        absent++;
                    }
                }
                methods.add(Map.of("signature", method.toGenericString(), "status", status));
            }
            total += methods.size();
            missing += absent;
            Map<String, Object> row = new LinkedHashMap<>();
            row.put("interface", surface[0]);
            row.put("backing", surface[1]);
            row.put("total", methods.size());
            row.put("backing_unverified", backed);
            row.put("default_unverified", defaults);
            row.put("missing", absent);
            row.put("methods", methods);
            inventory.add(row);
            if (args.length == 0 || !args[0].equals("--json")) {
                System.out.printf("%s: %d methods, %d backing/unverified, %d default/unverified, %d missing%n",
                        api.getSimpleName(), methods.size(), backed, defaults, absent);
            }
        }
        if (args.length > 0 && args[0].equals("--json")) {
            Map<String, Object> report = new LinkedHashMap<>();
            report.put("scope", "11 core proxy interfaces, inherited methods counted per receiver interface; not the entire Paper API");
            report.put("warning", "Method presence does not prove behavior; default methods can call missing methods. No compatibility percentage is computed.");
            report.put("total", total);
            report.put("missing", missing);
            report.put("surfaces", inventory);
            System.out.println(new GsonBuilder().setPrettyPrinting().create().toJson(report));
        } else {
            System.out.printf("Core proxy method inventory: %d total, %d missing. No compatibility percentage: existing methods require behavior audit.%n", total, missing);
        }
    }
}
