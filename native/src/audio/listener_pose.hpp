#pragma once

namespace vsa {

/// Listener pose as the render and simulation threads use it: an orthonormal basis, position in
/// scene coordinates.
struct ListenerPose {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float right[3] = {1.0f, 0.0f, 0.0f};
    float up[3] = {0.0f, 1.0f, 0.0f};
    float forward[3] = {0.0f, 0.0f, -1.0f};
};

}  // namespace vsa
