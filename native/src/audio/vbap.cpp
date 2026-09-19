#include "audio/vbap.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vsa {
namespace {

using Vec = std::array<double, 3>;

Vec sub(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec& a, const Vec& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

constexpr double kPlaneTolerance = 1e-6;

}  // namespace

bool Vbap::position(Speaker speaker, float& azimuth, float& elevation) noexcept {
    // ITU-R BS.775 / Dolby 7.1.4 placements.
    elevation = 0.0f;
    switch (speaker) {
        case Speaker::FrontLeft: azimuth = -30.0f; return true;
        case Speaker::FrontRight: azimuth = 30.0f; return true;
        case Speaker::FrontCentre: azimuth = 0.0f; return true;
        case Speaker::SideLeft: azimuth = -90.0f; return true;
        case Speaker::SideRight: azimuth = 90.0f; return true;
        case Speaker::BackLeft: azimuth = -150.0f; return true;
        case Speaker::BackRight: azimuth = 150.0f; return true;
        case Speaker::TopFrontLeft: azimuth = -45.0f; elevation = 45.0f; return true;
        case Speaker::TopFrontRight: azimuth = 45.0f; elevation = 45.0f; return true;
        case Speaker::TopBackLeft: azimuth = -135.0f; elevation = 45.0f; return true;
        case Speaker::TopBackRight: azimuth = 135.0f; elevation = 45.0f; return true;
        default: azimuth = 0.0f; return false;
    }
}

Vbap::Vbap(uint32_t channels) : channels_(std::min(channels, kMaxOutputChannels)) {
    const auto layout = steam_layout(channels_);
    constexpr double kRadians = std::numbers::pi / 180.0;
    std::vector<int> heights;
    std::vector<int> ear_level;
    for (uint32_t c = 0; c < channels_; ++c) {
        float azimuth = 0.0f;
        float elevation = 0.0f;
        if (!position(layout[c], azimuth, elevation)) {
            continue;
        }
        const double az = static_cast<double>(azimuth) * kRadians;
        const double el = static_cast<double>(elevation) * kRadians;
        points_.push_back({{static_cast<float>(std::sin(az) * std::cos(el)), static_cast<float>(std::sin(el)),
                            static_cast<float>(-std::cos(az) * std::cos(el))},
                           {static_cast<int>(c)}});
        (elevation > 0.0f ? heights : ear_level).push_back(static_cast<int>(c));
    }
    points_.push_back({{0.0f, 1.0f, 0.0f}, heights.empty() ? ear_level : heights});
    points_.push_back({{0.0f, -1.0f, 0.0f}, ear_level});

    // Faces with four or more speakers in a plane: a virtual speaker at their centre (then
    // triangulate again; the new point splits the face into a symmetric fan).
    for (int round = 0; round < 8; ++round) {
        const std::vector<std::vector<int>> flat = triangulate();
        if (flat.empty()) {
            break;
        }
        for (const std::vector<int>& face : flat) {
            Vec centre{};
            std::vector<int> spread;
            for (const int p : face) {
                const Point& point = points_[static_cast<std::size_t>(p)];
                for (std::size_t e = 0; e < 3; ++e) {
                    centre[e] += static_cast<double>(point.direction[e]);
                }
                spread.insert(spread.end(), point.channels.begin(), point.channels.end());
            }
            const double length = std::sqrt(dot(centre, centre));
            points_.push_back({{static_cast<float>(centre[0] / length), static_cast<float>(centre[1] / length),
                                static_cast<float>(centre[2] / length)},
                               spread});
        }
    }
}

std::vector<std::vector<int>> Vbap::triangulate() {
    // Convex hull by brute force (a dozen points): a triangle is a face when every other point is
    // on one side of its plane.
    triangles_.clear();
    std::vector<std::vector<int>> flat;
    const auto n = static_cast<int>(points_.size());
    const auto point = [&](int i) {
        const auto& d = points_[static_cast<std::size_t>(i)].direction;
        return Vec{static_cast<double>(d[0]), static_cast<double>(d[1]), static_cast<double>(d[2])};
    };
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            for (int k = j + 1; k < n; ++k) {
                const Vec a = point(i);
                const Vec b = point(j);
                const Vec c = point(k);
                Vec normal = cross(sub(b, a), sub(c, a));
                const double length = std::sqrt(dot(normal, normal));
                if (length < 1e-9) {
                    continue;
                }
                normal = {normal[0] / length, normal[1] / length, normal[2] / length};
                int above = 0;
                int below = 0;
                std::vector<int> in_plane = {i, j, k};
                for (int m = 0; m < n; ++m) {
                    if (m == i || m == j || m == k) {
                        continue;
                    }
                    const double side = dot(normal, sub(point(m), a));
                    above += side > kPlaneTolerance ? 1 : 0;
                    below += side < -kPlaneTolerance ? 1 : 0;
                    if (std::abs(side) <= kPlaneTolerance) {
                        in_plane.push_back(m);
                    }
                }
                if (above != 0 && below != 0) {
                    continue;
                }
                if (in_plane.size() > 3) {
                    std::sort(in_plane.begin(), in_plane.end());
                    if (std::find(flat.begin(), flat.end(), in_plane) == flat.end()) {
                        flat.push_back(in_plane);
                    }
                    continue;
                }
                // Inverse of [a b c] (columns): rows are the cross products over the determinant.
                const double det = dot(a, cross(b, c));
                if (std::abs(det) < 1e-9) {
                    continue;
                }
                const Vec r0 = cross(b, c);
                const Vec r1 = cross(c, a);
                const Vec r2 = cross(a, b);
                Triangle t{};
                t.points = {i, j, k};
                for (std::size_t e = 0; e < 3; ++e) {
                    t.inverse[e] = static_cast<float>(r0[e] / det);
                    t.inverse[3 + e] = static_cast<float>(r1[e] / det);
                    t.inverse[6 + e] = static_cast<float>(r2[e] / det);
                }
                triangles_.push_back(t);
            }
        }
    }
    return flat;
}

void Vbap::gains(const float direction[3], float* out) const noexcept {
    std::fill(out, out + channels_, 0.0f);
    const float x = direction[0];
    const float y = direction[1];
    const float z = direction[2];

    // The containing triangle has no negative weight; keep the least negative as a fallback
    // against rounding on an edge.
    const Triangle* found = nullptr;
    std::array<float, 3> g{};
    float best = -1e30f;
    for (const Triangle& t : triangles_) {
        const std::array<float, 3> w = {t.inverse[0] * x + t.inverse[1] * y + t.inverse[2] * z,
                                        t.inverse[3] * x + t.inverse[4] * y + t.inverse[5] * z,
                                        t.inverse[6] * x + t.inverse[7] * y + t.inverse[8] * z};
        const float lowest = std::min({w[0], w[1], w[2]});
        if (lowest > best) {
            best = lowest;
            found = &t;
            g = w;
            if (lowest >= -1e-5f) {
                break;
            }
        }
    }
    if (found == nullptr) {
        return;
    }

    for (std::size_t e = 0; e < 3; ++e) {
        const float w = std::max(g[e], 0.0f);
        const std::vector<int>& spread = points_[static_cast<std::size_t>(found->points[e])].channels;
        const float share = spread.size() == 1 ? w : w / std::sqrt(static_cast<float>(spread.size()));
        for (const int c : spread) {
            out[c] += share;
        }
    }
    float power = 0.0f;
    for (uint32_t c = 0; c < channels_; ++c) {
        power += out[c] * out[c];
    }
    if (power > 0.0f) {
        const float scale = 1.0f / std::sqrt(power);
        for (uint32_t c = 0; c < channels_; ++c) {
            out[c] *= scale;
        }
    }
}

}  // namespace vsa
