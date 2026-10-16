#pragma once
// Mob roster entry point (vanilla mob behaviours on top of the mob framework). Included from game/game.hpp.
#include "breeding.hpp"
#include "merchant_menu.hpp"
#include "trading.hpp"

#include <unistd.h>

inline void roster_init() {
    if (!load_roster_data()) _exit(1);
    merchant_menu_init();
}
