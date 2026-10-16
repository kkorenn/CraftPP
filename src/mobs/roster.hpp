#pragma once
// Mob roster entry point: villager trading data + merchant menu (game side). Mob classes: src/mobs/roster/*.cpp.
#include "merchant_menu.hpp"
#include "riding.hpp"

#include <unistd.h>

inline void roster_init() {
    if (!load_trading()) _exit(1);
    merchant_menu_init();
}
