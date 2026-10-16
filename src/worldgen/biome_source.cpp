#include "worldgen/biome_source.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_map>

#include "gen/ids.hpp"

namespace worldgen {
namespace {
// Java long arithmetic wraps.
int64_t sub(int64_t a, int64_t b) { return (int64_t)((uint64_t)a - (uint64_t)b); }
int64_t add(int64_t a, int64_t b) { return (int64_t)((uint64_t)a + (uint64_t)b); }
int64_t square(int64_t a) { return (int64_t)((uint64_t)a * (uint64_t)a); }
int64_t jabs(int64_t a) { return a < 0 ? (int64_t)(0 - (uint64_t)a) : a; }

// Parameter.distance
int64_t distance(const Parameter& p, int64_t target) {
    int64_t above = sub(target, p.max), below = sub(p.min, target);
    return above > 0 ? above : std::max<int64_t>(below, 0);
}
int64_t distance(const Parameter* space, const int64_t* target) {
    int64_t d = 0;
    for (int i = 0; i < 7; i++) d = add(d, square(distance(space[i], target[i])));
    return d;
}
int64_t center(const Parameter& p) { return add(p.min, p.max) / 2; }

struct Raw { int biome; int64_t v[13]; };
const Raw OVERWORLD[] = {
#include "worldgen/biome_params.inc"
};

std::atomic<uint64_t> next_id{0};
}  // namespace

BiomeSource::BiomeSource() : BiomeSource([] {
    std::vector<std::pair<ParameterPoint, int>> v;
    v.reserve(std::size(OVERWORLD));
    for (const Raw& r : OVERWORLD) {
        ParameterPoint p;
        for (int d = 0; d < 6; d++) p.space[d] = {r.v[2 * d], r.v[2 * d + 1]};
        p.space[6] = {r.v[12], r.v[12]};
        v.push_back({p, r.biome});
    }
    return v;
}()) {}

BiomeSource::BiomeSource(std::vector<std::pair<ParameterPoint, int>> values, int childrenPerNode)
    : values_(std::move(values)), childrenPerNode_(childrenPerNode), id_(next_id++) {
    std::vector<int> leaves;
    for (auto& [p, biome] : values_) {
        Node n;
        std::copy(p.space, p.space + 7, n.space);
        n.value = biome;
        leaves.push_back((int)nodes_.size());
        nodes_.push_back(std::move(n));
    }
    root_ = build(std::move(leaves));
}

// RTree.build. Java List.sort is a stable TimSort; std::stable_sort gives the identical order.
int BiomeSource::build(std::vector<int> children) {
    auto space_of = [this](const std::vector<int>& kids, Parameter* out) {  // buildParameterSpace
        for (int d = 0; d < 7; d++) out[d] = nodes_[kids[0]].space[d];
        for (int k : kids)
            for (int d = 0; d < 7; d++)
                out[d] = {std::min(out[d].min, nodes_[k].space[d].min), std::max(out[d].max, nodes_[k].space[d].max)};
    };
    auto make_subtree = [&](std::vector<int> kids) {
        Node n;
        space_of(kids, n.space);
        n.children = std::move(kids);
        nodes_.push_back(std::move(n));
        return (int)nodes_.size() - 1;
    };
    // sort(): lexicographic over dimensions dim, dim+1, ... (mod 7), key = center (or |center|)
    auto less = [](const Parameter* a, const Parameter* b, int dim, bool absolute) {
        for (int i = 0; i < 7; i++) {
            int d = (dim + i) % 7;
            int64_t ka = center(a[d]), kb = center(b[d]);
            if (absolute) ka = jabs(ka), kb = jabs(kb);
            if (ka != kb) return ka < kb;
        }
        return false;
    };

    if (children.size() == 1) return children[0];
    if ((int)children.size() <= childrenPerNode_) {
        auto mag = [this](int n) {
            int64_t m = 0;
            for (int d = 0; d < 7; d++) m = add(m, jabs(center(nodes_[n].space[d])));
            return m;
        };
        std::stable_sort(children.begin(), children.end(), [&](int a, int b) { return mag(a) < mag(b); });
        return make_subtree(std::move(children));
    }

    struct Bucket { std::vector<int> kids; Parameter space[7]; };
    int64_t minCost = INT64_MAX;
    int minDimension = -1;
    std::vector<Bucket> minBuckets;
    int expected = (int)std::pow((double)childrenPerNode_,
                                 std::floor(std::log((double)children.size() - 0.01) / std::log((double)childrenPerNode_)));
    for (int d = 0; d < 7; d++) {
        std::stable_sort(children.begin(), children.end(),
                         [&](int a, int b) { return less(nodes_[a].space, nodes_[b].space, d, false); });
        std::vector<Bucket> buckets;  // bucketize
        for (size_t i = 0; i < children.size(); i += expected) {
            Bucket b;
            b.kids.assign(children.begin() + i, children.begin() + std::min(children.size(), i + expected));
            space_of(b.kids, b.space);
            buckets.push_back(std::move(b));
        }
        int64_t totalCost = 0;
        for (auto& b : buckets)
            for (auto& p : b.space) totalCost = add(totalCost, jabs(sub(p.max, p.min)));
        if (minCost > totalCost) minCost = totalCost, minDimension = d, minBuckets = std::move(buckets);
    }
    std::stable_sort(minBuckets.begin(), minBuckets.end(),
                     [&](const Bucket& a, const Bucket& b) { return less(a.space, b.space, minDimension, true); });
    std::vector<int> built;
    for (auto& b : minBuckets) built.push_back(build(std::move(b.kids)));
    return make_subtree(std::move(built));
}

// SubTree.search / Leaf.search with the default Node::distance metric.
int BiomeSource::search(int node, const int64_t* target, int candidate) const {
    const Node& n = nodes_[node];
    if (n.children.empty()) return node;
    int64_t minDistance = candidate < 0 ? INT64_MAX : distance(nodes_[candidate].space, target);
    int closestLeaf = candidate;
    for (int child : n.children) {
        int64_t childDistance = distance(nodes_[child].space, target);
        if (minDistance > childDistance) {
            int leaf = search(child, target, closestLeaf);
            int64_t leafDistance = child == leaf ? childDistance : distance(nodes_[leaf].space, target);
            if (minDistance > leafDistance) minDistance = leafDistance, closestLeaf = leaf;
        }
    }
    return closestLeaf;
}

int BiomeSource::find(const TargetPoint& t) const {
    // RTree.lastResult is a ThreadLocal per tree instance; key by a unique id (not address) to survive reuse.
    thread_local std::unordered_map<uint64_t, int> lastResult;
    int64_t target[7] = {t.temperature, t.humidity, t.continentalness, t.erosion, t.depth, t.weirdness, 0};
    auto [it, inserted] = lastResult.try_emplace(id_, -1);
    it->second = search(root_, target, it->second);
    return nodes_[it->second].value;
}

}  // namespace worldgen
