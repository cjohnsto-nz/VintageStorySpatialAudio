#include "world/air_paths.hpp"

#include <algorithm>
#include <cmath>

namespace vsa::world {

void AirField::compute(const VoxelView& view, const double from[3]) {
    constexpr std::size_t kCells = static_cast<std::size_t>(kSize) * kSize * kSize;
    if (units_.size() != kCells) {
        units_.resize(kCells);
        passage_.resize(kCells);
        buckets_.resize(kCorner + kPartialPenalty + 1);
    }
    for (int k = 0; k < 3; ++k) {
        from_[k] = from[k];
        centre_[k] = static_cast<int64_t>(std::floor(from[k]));
    }
    for (int y = 0; y < kSize; ++y) {
        for (int z = 0; z < kSize; ++z) {
            for (int x = 0; x < kSize; ++x) {
                passage_[index(x, y, z)] = view.passage(centre_[0] + x - kRadius, centre_[1] + y - kRadius, centre_[2] + z - kRadius);
            }
        }
    }
    std::fill(units_.begin(), units_.end(), kUnreached);
    for (auto& bucket : buckets_) {
        bucket.clear();
    }

    // Dial's algorithm: the step costs are small integers, so a ring of buckets is the queue.
    const std::size_t start = index(kRadius, kRadius, kRadius);
    units_[start] = 0;
    buckets_[0].push_back(static_cast<uint32_t>(start));
    std::size_t pending = 1;
    const auto ring = static_cast<uint32_t>(buckets_.size());
    for (uint32_t cost = 0; pending > 0; ++cost) {
        std::vector<uint32_t>& bucket = buckets_[cost % ring];
        while (!bucket.empty()) {
            const uint32_t cell = bucket.back();
            bucket.pop_back();
            --pending;
            if (units_[cell] != cost) {
                continue;  // reached more cheaply since
            }
            const int x = static_cast<int>(cell % kSize);
            const int z = static_cast<int>((cell / kSize) % kSize);
            const int y = static_cast<int>(cell / (static_cast<std::size_t>(kSize) * kSize));
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int axes = (dx != 0 ? 1 : 0) + (dy != 0 ? 1 : 0) + (dz != 0 ? 1 : 0);
                        const int nx = x + dx;
                        const int ny = y + dy;
                        const int nz = z + dz;
                        if (axes == 0 || nx < 0 || ny < 0 || nz < 0 || nx >= kSize || ny >= kSize || nz >= kSize) {
                            continue;
                        }
                        const std::size_t next = index(nx, ny, nz);
                        if (passage_[next] == VoxelView::Passage::Closed) {
                            continue;
                        }
                        // No squeezing between solids: every face-neighbour a diagonal step
                        // passes must be passable too.
                        if (axes > 1 &&
                            ((dx != 0 && passage_[index(nx, y, z)] == VoxelView::Passage::Closed) ||
                             (dy != 0 && passage_[index(x, ny, z)] == VoxelView::Passage::Closed) ||
                             (dz != 0 && passage_[index(x, y, nz)] == VoxelView::Passage::Closed))) {
                            continue;
                        }
                        uint32_t step = axes == 1 ? kFace : axes == 2 ? kEdge : kCorner;
                        if (passage_[next] == VoxelView::Passage::Partial) {
                            step += kPartialPenalty;
                        }
                        const uint32_t reached = cost + step;
                        if (reached < units_[next]) {
                            units_[next] = static_cast<uint16_t>(std::min<uint32_t>(reached, kUnreached - 1));
                            buckets_[reached % ring].push_back(static_cast<uint32_t>(next));
                            ++pending;
                        }
                    }
                }
            }
        }
    }
    computed_ = true;
}

double AirField::distance(const double point[3]) const {
    if (!computed_) {
        return kNone;
    }
    int local[3];
    for (int k = 0; k < 3; ++k) {
        const int64_t d = static_cast<int64_t>(std::floor(point[k])) - centre_[k];
        if (d < -kRadius || d > kRadius) {
            return kNone;
        }
        local[k] = static_cast<int>(d) + kRadius;
    }
    const uint16_t units = units_[index(local[0], local[1], local[2])];
    if (units == kUnreached) {
        return kNone;
    }
    const double dx = point[0] - from_[0];
    const double dy = point[1] - from_[1];
    const double dz = point[2] - from_[2];
    return std::max(static_cast<double>(units) / kFace, std::sqrt(dx * dx + dy * dy + dz * dz));
}

}  // namespace vsa::world
