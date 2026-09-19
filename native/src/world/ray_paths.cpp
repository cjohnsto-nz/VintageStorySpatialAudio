#include "world/ray_paths.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vsa::world {
namespace {

/// A small deterministic generator (the paths must be the same every call).
struct Random {
    uint64_t state;
    double next() noexcept {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(state >> 11) * (1.0 / 9007199254740992.0);
    }
};

/// A cosine-weighted direction around the unit normal `n`.
void diffuse(const double n[3], Random& random, double out[3]) {
    const double u = random.next();
    const double v = random.next();
    const double r = std::sqrt(u);
    const double phi = 2.0 * std::numbers::pi * v;
    const double x = r * std::cos(phi);
    const double y = r * std::sin(phi);
    const double z = std::sqrt(std::max(0.0, 1.0 - u));
    // An orthonormal basis around n.
    const double a[3] = {std::abs(n[0]) < 0.9 ? 1.0 : 0.0, std::abs(n[0]) < 0.9 ? 0.0 : 1.0, 0.0};
    double t[3] = {a[1] * n[2] - a[2] * n[1], a[2] * n[0] - a[0] * n[2], a[0] * n[1] - a[1] * n[0]};
    const double tl = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    for (double& c : t) {
        c /= tl;
    }
    const double b[3] = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]};
    for (int k = 0; k < 3; ++k) {
        out[k] = x * t[k] + y * b[k] + z * n[k];
    }
}

}  // namespace

std::vector<RaySegment> trace_paths(const VoxelView& view, const float origin[3], uint32_t rays, uint32_t bounces,
                                    float max_distance, std::size_t max_segments) {
    std::vector<RaySegment> segments;
    segments.reserve(std::min<std::size_t>(max_segments, static_cast<std::size_t>(rays) * (bounces + 1)));
    const int32_t* o = view.origin();
    const auto& materials = view.materials();
    const double golden = std::numbers::pi * (3.0 - std::sqrt(5.0));
    for (uint32_t r = 0; r < rays && segments.size() < max_segments; ++r) {
        Random random{0x9E3779B97F4A7C15ull * (r + 1)};
        double position[3] = {static_cast<double>(origin[0]) + o[0], static_cast<double>(origin[1]) + o[1],
                              static_cast<double>(origin[2]) + o[2]};
        const double y = 1.0 - (2.0 * r + 1.0) / rays;
        const double radius = std::sqrt(std::max(0.0, 1.0 - y * y));
        double direction[3] = {radius * std::cos(golden * r), y, radius * std::sin(golden * r)};
        float energy = 1.0f;
        for (uint32_t bounce = 0; bounce <= bounces && segments.size() < max_segments; ++bounce) {
            VoxelHit hit;
            const bool found = view.first_hit(position, direction, static_cast<double>(max_distance), hit);
            RaySegment s;
            s.bounce = bounce;
            for (int k = 0; k < 3; ++k) {
                s.from[k] = static_cast<float>(position[k] - o[k]);
                const double end = found ? hit.point[k] : position[k] + direction[k] * static_cast<double>(max_distance);
                s.to[k] = static_cast<float>(end - o[k]);
            }
            if (!found) {
                s.energy = energy;
                segments.push_back(s);
                break;  // off into the open
            }
            const TransmissionMaterial* m = hit.material < materials.size() ? &materials[hit.material] : nullptr;
            energy *= 1.0f - (m != nullptr ? m->absorption[1] : 0.1f);
            s.energy = energy;
            s.material = hit.material;
            segments.push_back(s);
            if (energy < 1e-3f) {
                break;
            }
            // Leave the surface: mirror-like, or scattered.
            const double scattering = m != nullptr ? static_cast<double>(m->scattering) : 0.05;
            if (random.next() < scattering) {
                diffuse(hit.normal, random, direction);
            } else {
                const double dn = direction[0] * hit.normal[0] + direction[1] * hit.normal[1] + direction[2] * hit.normal[2];
                for (int k = 0; k < 3; ++k) {
                    direction[k] -= 2.0 * dn * hit.normal[k];
                }
            }
            for (int k = 0; k < 3; ++k) {
                position[k] = hit.point[k] + hit.normal[k] * 1e-4;
            }
        }
    }
    return segments;
}

}  // namespace vsa::world
