#pragma once

#include "audio/channel_layout.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace vsa {

/// 3D vector-base amplitude panning (Pulkki) over a speaker layout with heights.
///
/// The speakers are triangulated (their convex hull) once, with virtual speakers added where a
/// plain triangulation would be lopsided: one at the zenith spread evenly over the height
/// speakers, one at the nadir spread over the ear-level ones, and one at the centre of any face
/// with four or more speakers in a plane (7.1.4's back quad), spread over its corners. So a
/// source straight overhead plays equally from every height speaker, one below from the ear-level
/// ring, and one behind symmetrically from the back four. A direction's gains come from the
/// triangle that contains it and are power-normalised. The LFE gets none.
class Vbap {
public:
    /// Builds the triangulation for the engine's layout of `channels` (12 = 7.1.4). Allocates.
    explicit Vbap(uint32_t channels);

    [[nodiscard]] uint32_t channels() const noexcept { return channels_; }

    /// Gains for a unit direction in listener space (+x right, +y up, -z ahead), one per channel
    /// in the engine's order. Never allocates.
    void gains(const float direction[3], float* out) const noexcept;

    /// Where the engine's layouts put each speaker: azimuth (degrees, clockwise from ahead) and
    /// elevation. False for speakers without a direction.
    static bool position(Speaker speaker, float& azimuth, float& elevation) noexcept;

private:
    struct Point {
        std::array<float, 3> direction;
        std::vector<int> channels;  // one for a real speaker; a virtual one's share goes to several
    };
    struct Triangle {
        std::array<int, 3> points;
        std::array<float, 9> inverse;  // row-major inverse of the matrix whose columns are the points
    };

    /// The convex hull of points_. Returns the point sets of faces with four or more coplanar
    /// points that do not have a virtual centre yet.
    std::vector<std::vector<int>> triangulate();

    uint32_t channels_;
    std::vector<Point> points_;
    std::vector<Triangle> triangles_;
};

}  // namespace vsa
