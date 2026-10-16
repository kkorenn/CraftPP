#include "worldgen/features.hpp"

#include "worldgen/overworld.hpp"

namespace worldgen {

Features::Features(Overworld& ow, const std::string&) : ow(ow) {}
Features::~Features() = default;
void Features::decorate(Region&) {}

}  // namespace worldgen
