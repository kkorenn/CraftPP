#pragma once
// Passengers and leads: set_passengers / set_entity_link packets (ClientboundSetPassengersPacket,
// ClientboundSetEntityLinkPacket). The mob framework owns vehicle/passenger state; these encode it.
#include "../game/entity.hpp"

inline Writer set_passengers_packet(int vehicle, const std::vector<int>& passengers) {
    Writer w;
    w.varint(play_cb::set_passengers).varint(vehicle).varint(int32_t(passengers.size()));
    for (int id : passengers) w.varint(id);
    return w;
}
// holder 0 = unleashed.
inline Writer set_entity_link_packet(int leashed, int holder) {
    return std::move(Writer().varint(play_cb::set_entity_link).i32(leashed).i32(holder));
}
