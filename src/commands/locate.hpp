#pragma once
// /locate structure (LocateCommand.locateStructure + showLocateResult) over the structure placement code.
#include <chrono>

#include "dispatcher.hpp"
#include "../worldgen/structure/hooks.hpp"

inline int locate_structure(Cmd& c) {
    std::string arg = c.str("structure");
    bool tag = !arg.empty() && arg[0] == '#';
    std::string id = structure::fullId(tag ? arg.substr(1) : arg);
    std::string printable = tag ? "#" + id : id;
    if (!structure_world) throw err("commands.locate.structure.not_found", {printable});  // flat / generate-structures=false
    auto& mgr = structure_world->manager();
    std::vector<const structure::Structure*> wanted;
    if (tag) {
        for (auto& n : structure::resolveTag("worldgen/structure", id))
            if (auto* s = mgr.structure(n)) wanted.push_back(s);
        if (wanted.empty() && structure::resolveTag("worldgen/structure", id).empty())
            throw err("commands.locate.structure.invalid", {printable});
    } else if (auto* s = mgr.structure(id)) {
        wanted.push_back(s);
    } else {
        throw err("commands.locate.structure.invalid", {printable});
    }
    structure::BlockPos from{int(std::floor(c.src.x)), int(std::floor(c.src.y)), int(std::floor(c.src.z))};
    auto t0 = std::chrono::steady_clock::now();
    auto found = mgr.findNearest(wanted, from, 100);
    if (!found) throw err("commands.locate.structure.not_found", {printable});
    int dx = found->pos.x - from.x, dz = found->pos.z - from.z;
    int distance = int(std::floor(std::sqrt(float(dx * dx + dz * dz))));  // Mth.floor(Mth.sqrt(int))
    std::string name = tag ? printable + " (" + found->structure->id + ")" : printable;
    Text coords = tr("chat.square_brackets", {tr("chat.coordinates", {found->pos.x, "~", found->pos.z})});
    coords = styled(coords, "green");
    Nbt click = Nbt::compound();
    click.set("action", Nbt::str("suggest_command"));
    click.set("command", Nbt::str("/tp @s " + std::to_string(found->pos.x) + " ~ " + std::to_string(found->pos.z)));
    coords.set("click_event", click);
    Nbt hover = Nbt::compound();
    hover.set("action", Nbt::str("show_text"));
    hover.set("value", tr("chat.coordinates.tooltip"));
    coords.set("hover_event", hover);
    c.success(tr("commands.locate.structure.success", {name, coords, distance}), false);
    log_info("Locating element %s took %lld ms", name.c_str(),
           (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    return distance;
}

inline void register_locate() {
    command_handlers()["locate"] = [](Cmd& c) -> int {
        if (c.is("structure")) return locate_structure(c);
        throw fail(not_supported(c.is("biome") ? "locate biome" : "locate poi"));
    };
}
