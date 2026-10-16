#pragma once
// Command system entry point (implementation in src/commands/*): vanilla command tree, dispatcher, commands.
#include "blocks.hpp"
#include "commands/system.hpp"

// Loads ops/bans/whitelist + world state and caches the level-0 tree (call once at startup).
inline void build_commands_packet() { init_commands(); }

// Runs a command line (without '/'). `p` is null for the console.
inline void run_command(Player* p, const std::string& line) {
    if (!p) { perform(console_source(), line); return; }
    if (auto pp = find_player(p->name)) perform(source_for(pp), line);
}
