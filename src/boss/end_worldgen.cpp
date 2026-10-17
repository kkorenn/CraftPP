// minecraft:end_spike (EndSpikeFeature) for the End's features step: obsidian pillars, iron bar cages, the bedrock
// + fire crystal seat, and the crystals themselves (queued for the boss tick: worldgen runs on worker threads).
#include "boss/end_features.hpp"
#include "worldgen/features.hpp"

namespace {

using namespace worldgen;

const endgen::States& worldgen_states() {
    static const endgen::States st = [] {
        auto& R = bs::R();
        endgen::States s{};
        s.air = R.parse("minecraft:air"), s.obsidian = R.parse("minecraft:obsidian"), s.bedrock = R.parse("minecraft:bedrock");
        s.end_stone = R.parse("minecraft:end_stone"), s.end_portal = R.parse("minecraft:end_portal");
        s.end_gateway = R.parse("minecraft:end_gateway"), s.fire = R.parse("minecraft:fire");
        s.wall_torch_n = R.parse("minecraft:wall_torch[facing=north]"), s.wall_torch_e = R.parse("minecraft:wall_torch[facing=east]");
        s.wall_torch_s = R.parse("minecraft:wall_torch[facing=south]"), s.wall_torch_w = R.parse("minecraft:wall_torch[facing=west]");
        for (int i = 0; i < 16; i++) {
            auto b = [&](int bit) { return (i >> bit & 1) ? "true" : "false"; };
            s.iron_bars[i] = R.parse(std::string("minecraft:iron_bars[north=") + b(0) + ",south=" + b(1) + ",west=" + b(2) + ",east=" + b(3) + "]");
        }
        return s;
    }();
    return st;
}

struct EndSpike : feat::Feature {
    std::vector<endgen::EndSpike> spikes;  // empty: getSpikesForLevel(seed)
    bool invulnerable = false;
    bool place(feat::Level& l, RandomSource& r, const feat::P& origin) const override {
        auto list = spikes;
        if (list.empty()) {
            auto s = endgen::spikes_for_seed(l.seed());
            list.assign(s.begin(), s.end());
        }
        for (auto& s : list) {
            if ((origin.x >> 4) != (s.x >> 4) || (origin.z >> 4) != (s.z >> 4)) continue;  // isCenterWithinChunk
            endgen::place_spike(s, worldgen_states(), l.min_y(), [&](int x, int y, int z, int st) { l.set({x, y, z}, st); });
            float yaw = r.nextFloat() * 360.0F;  // endCrystal.snapTo(..., random.nextFloat() * 360, 0)
            std::lock_guard lock(endgen::pending_mtx);
            endgen::pending_crystals.push_back({s.x + 0.5, double(s.height + 1), s.z + 0.5, yaw});
        }
        return true;
    }
};

const bool registered = [] {
    feat::extra_feature_parsers()["end_spike"] = [](feat::Loader& L, const Json& j) {
        auto* f = L.make<EndSpike>();
        for (auto& s : j["spikes"].arr)
            f->spikes.push_back({s.getInt("centerX", 0), s.getInt("centerZ", 0), s.getInt("radius", 0), s.getInt("height", 0),
                                 s.find("guarded") && s["guarded"].asBool()});
        f->invulnerable = j.find("crystal_invulnerable") && j["crystal_invulnerable"].asBool();
        return f;
    };
    return true;
}();

}  // namespace
