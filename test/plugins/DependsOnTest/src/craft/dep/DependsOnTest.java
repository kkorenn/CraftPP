package craft.dep;

import org.bukkit.plugin.java.JavaPlugin;

public final class DependsOnTest extends JavaPlugin {
    @Override
    public void onEnable() {
        getLogger().info("enabled after CraftTestPlugin=" + getServer().getPluginManager().getPlugin("CraftTestPlugin").isEnabled());
    }
}
