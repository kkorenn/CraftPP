// Plugin inventories (Player.openInventory with a Bukkit Inventory): a menu whose slots CraftPP owns while it is
// open; the bridge's InventoryImpl stays the plugin-facing copy and is updated after every click ("invchange").
// Also fires InventoryClickEvent / InventoryCloseEvent for every menu.
#include "internal.hpp"
#include "../containers/containers.hpp"

namespace plugins::detail {
namespace {

struct PluginMenu : cont::SlotsMenu {
    std::vector<ItemStack> items;
    explicit PluginMenu(size_t n) : items(n) {
        for (auto& s : items) slots.push_back(&s);
    }
    bool still_valid(Player&) override { return true; }
};

PluginMenu* plugin_menu(Player& p) { return dynamic_cast<PluginMenu*>(p.g.menu.get()); }

void fill(PluginMenu& m, const Json* items) {
    for (size_t i = 0; i < m.items.size(); i++)
        m.items[i] = items && items->isArray() && i < items->size() ? item_from_json((*items)[i]) : ItemStack{};
}

// The bridge's copy of the top inventory follows CraftPP's after each click.
void send_contents(Player& p) {
    PluginMenu* m = plugin_menu(p);
    if (!m) return;
    std::string items = "[";
    for (size_t i = 0; i < m->items.size(); i++) items += (i ? "," : "") + item_to_json(m->items[i]);
    try {
        request("invchange", "{\"uuid\":" + q(uuid_str(p.uuid_hi, p.uuid_lo)) + ",\"windowId\":" + std::to_string(p.g.container) +
                                 ",\"items\":" + items + "]}");
    } catch (const std::exception& e) {
        log_msg(LogLevel::WARN, std::string("Plugin inventory sync failed: ") + e.what());
    }
}

}  // namespace

std::optional<std::string> api_inventory(const std::string& m, const Json& a) {
    if (m == "player.openInventory") {
        Player& p = require_player(a);
        std::string type = text_arg(a, "type", "generic_9x3");
        auto id = menu_ids.find(type);
        if (id == menu_ids.end()) throw std::runtime_error("Unknown menu type " + type);
        const Json* items = a.find("items");
        auto menu = std::make_shared<PluginMenu>(items && items->isArray() ? items->size() : 27);
        menu->type = id->second;
        fill(*menu, items);
        Writer title;
        legacy_component(title, text_arg(a, "title"));
        open_menu(p, menu, title.b);
        return std::to_string(p.g.container);
    }
    if (m == "player.updateInventory") {
        Player& p = require_player(a);
        PluginMenu* menu = plugin_menu(p);
        if (menu && p.g.container == int(num_arg(a, "windowId"))) fill(*menu, a.find("items"));  // sent at the end of the tick
        return "null";
    }
    if (m == "player.closeInventory") {
        Player& p = require_player(a);
        if (p.g.container) close_container(p, true);
        return "null";
    }
    return std::nullopt;
}

void install_inventory_handlers() {
    on(play_sb::container_click, [orig = original(play_sb::container_click)](Player& p, Reader& in, int id) {
        bool plugin = plugin_menu(p) != nullptr;
        if (has("invclick")) {
            Reader peek = in;
            int container = peek.varint();
            peek.varint();
            int slot = int16_t(peek.u16());
            int button = int8_t(peek.u8());
            int mode = peek.varint();
            if (container == p.g.container) {
                std::string current = slot >= 0 && slot < menu_size(p) ? item_to_json(*menu_slot(p, slot).s) : "null";
                Json r = event("invclick", "{\"uuid\":" + q(uuid_str(p.uuid_hi, p.uuid_lo)) + ",\"name\":" + q(p.name) +
                                               ",\"windowId\":" + std::to_string(container) + ",\"slot\":" + std::to_string(slot) +
                                               ",\"button\":" + std::to_string(button) + ",\"mode\":" + std::to_string(mode) +
                                               ",\"current\":" + current + "}");
                if (cancelled(r)) {  // the client predicted the click: resend the menu and the cursor
                    p.g.remote_valid = false;
                    if (plugin) send_contents(p);
                    return;
                }
            }
        }
        if (orig) orig(p, in, id);
        if (plugin) send_contents(p);
    });
    on(play_sb::container_close, [orig = original(play_sb::container_close)](Player& p, Reader& in, int id) {
        int container = p.g.container;
        bool notify = plugin_menu(p) != nullptr || has("invclose");
        if (orig) orig(p, in, id);
        if (notify && container)
            event("invclose", "{\"uuid\":" + q(uuid_str(p.uuid_hi, p.uuid_lo)) + ",\"name\":" + q(p.name) +
                                  ",\"windowId\":" + std::to_string(container) + "}");
    });
}

}  // namespace plugins::detail
