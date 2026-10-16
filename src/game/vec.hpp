#pragma once
// Vec3 / AABB / Mth helpers shared by commands and the entity system (net.minecraft.world.phys, util.Mth).
#include <algorithm>
#include <cmath>
#include <cstdint>

struct Vec3 {  // aggregate: Vec3{x, y, z}
    double x, y, z;
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
    double length_sqr() const { return x * x + y * y + z * z; }
    double length() const { return std::sqrt(length_sqr()); }
    double horizontal_sqr() const { return x * x + z * z; }
    double horizontal() const { return std::sqrt(horizontal_sqr()); }
    double dist_sqr(const Vec3& o) const { return (*this - o).length_sqr(); }
    Vec3 normalize() const {  // Vec3.normalize: zero below 1e-5
        double l = length();
        return l < 1.0E-5 ? Vec3{0, 0, 0} : Vec3{x / l, y / l, z / l};
    }
    Vec3 multiply(double a, double b, double c) const { return {x * a, y * b, z * c}; }
    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
};

struct AABB {
    double x0, y0, z0, x1, y1, z1;
    static AABB at_feet(double x, double y, double z, double w, double h) {  // EntityDimensions.makeBoundingBox
        double r = w / 2.0;
        return {x - r, y, z - r, x + r, y + h, z + r};
    }
    AABB move(double dx, double dy, double dz) const { return {x0 + dx, y0 + dy, z0 + dz, x1 + dx, y1 + dy, z1 + dz}; }
    AABB move(const Vec3& v) const { return move(v.x, v.y, v.z); }
    AABB inflate(double dx, double dy, double dz) const { return {x0 - dx, y0 - dy, z0 - dz, x1 + dx, y1 + dy, z1 + dz}; }
    AABB inflate(double d) const { return inflate(d, d, d); }
    AABB expand_towards(double dx, double dy, double dz) const {
        AABB r = *this;
        (dx < 0 ? r.x0 : r.x1) += dx;
        (dy < 0 ? r.y0 : r.y1) += dy;
        (dz < 0 ? r.z0 : r.z1) += dz;
        return r;
    }
    AABB expand_towards(const Vec3& v) const { return expand_towards(v.x, v.y, v.z); }
    bool intersects(const AABB& o) const {
        return x0 < o.x1 && x1 > o.x0 && y0 < o.y1 && y1 > o.y0 && z0 < o.z1 && z1 > o.z0;
    }
    bool contains(const Vec3& p) const { return p.x >= x0 && p.x < x1 && p.y >= y0 && p.y < y1 && p.z >= z0 && p.z < z1; }
    double min(int axis) const { return axis == 0 ? x0 : axis == 1 ? y0 : z0; }
    double max(int axis) const { return axis == 0 ? x1 : axis == 1 ? y1 : z1; }
    Vec3 center() const { return {(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2}; }
    double xsize() const { return x1 - x0; }
    double ysize() const { return y1 - y0; }
    double zsize() const { return z1 - z0; }
    double distance_sqr(const Vec3& p) const {  // AABB.distanceToSqr
        double dx = std::max({x0 - p.x, p.x - x1, 0.0}), dy = std::max({y0 - p.y, p.y - y1, 0.0}),
               dz = std::max({z0 - p.z, p.z - z1, 0.0});
        return dx * dx + dy * dy + dz * dz;
    }
};

namespace mth {
inline constexpr float DEG = float(M_PI / 180.0);
inline int floor(double v) { return int(std::floor(v)); }
inline float wrap_degrees(float d) {  // Mth.wrapDegrees(float)
    float r = std::fmod(d, 360.0f);
    if (r >= 180.0f) r -= 360.0f;
    if (r < -180.0f) r += 360.0f;
    return r;
}
inline double wrap_degrees(double d) {
    double r = std::fmod(d, 360.0);
    if (r >= 180.0) r -= 360.0;
    if (r < -180.0) r += 360.0;
    return r;
}
inline float approach_degrees(float cur, float target, float max) {  // Mth.approachDegrees
    float diff = wrap_degrees(target - cur);
    return cur + std::clamp(diff, -max, max);
}
inline float rotlerp(float a, float b, float max) {  // Mob.rotlerp / MoveControl.rotlerp
    float diff = wrap_degrees(b - a);
    diff = std::clamp(diff, -max, max);
    return a + diff;
}
inline uint8_t pack_degrees(float deg) { return uint8_t(int(std::floor(deg * 256.0f / 360.0f))); }  // Mth.packDegrees
inline bool equal(double a, double b) { return std::abs(b - a) < 1.0E-5F; }  // Mth.equal(double, double)
inline float sin(float v) { return std::sin(v); }  // ponytail: vanilla Mth.sin is a 65536-entry table; error < 1e-4
inline float cos(float v) { return std::cos(v); }
inline double lerp(double t, double a, double b) { return a + t * (b - a); }
inline float lerp(float t, float a, float b) { return a + t * (b - a); }
inline int64_t block_key(int x, int y, int z) {  // BlockPos.asLong
    return (int64_t(x & 0x3FFFFFF) << 38) | (int64_t(z & 0x3FFFFFF) << 12) | int64_t(y & 0xFFF);
}
inline int64_t chunk_key(int cx, int cz) { return int64_t(uint32_t(cx)) | (int64_t(uint32_t(cz)) << 32); }  // ChunkPos.pack
}  // namespace mth
