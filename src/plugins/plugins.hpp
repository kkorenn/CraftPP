#pragma once
// Bukkit/Paper plugin support: an embedded JVM (dlopen'd libjvm, JNI) running the
// bridge in java/ (paper-api + CraftPP's server implementation). See docs/plugins.md.
// Every hook is a cheap no-op when no plugin is loaded (no plugins/*.jar or no Java 25+).
#include <string>

struct Player;

namespace plugins {

// Startup: if plugins/*.jar exist and a JVM is found, boot it, load + enable plugins,
// wrap the packet handlers that fire events, and add plugin roots to the command tree.
// Call after register_core_handlers() and build_commands_packet(), before accepting players.
void start();
// Shutdown: onDisable for every plugin (before the world is saved).
void stop();
bool active();

// Join/quit announcements (PlayerJoinEvent / PlayerQuitEvent may rewrite or suppress them).
void announce_join(Player& p);
void announce_quit(Player& p);

// Console line (without '/'): true when a plugin (or /plugins, /version) handled it.
bool console_command(const std::string& line);

// Game tick hook: the game loop calls this once per tick so Bukkit scheduler ticks are
// world ticks. Until it is called the bridge self-ticks at 20 TPS on its server thread.
void tick();

}  // namespace plugins
