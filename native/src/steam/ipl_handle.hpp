#pragma once

#include <phonon.h>

#include <utility>

namespace vsa::steam {

/// Move-only owner of a Steam Audio object. Steam Audio objects are reference
/// counted and released through `iplXxxRelease(T*)`, which also nulls the handle.
template <typename T, void (*Release)(T*)>
class IplHandle {
public:
    IplHandle() noexcept = default;
    explicit IplHandle(T handle) noexcept : handle_(handle) {}
    ~IplHandle() { reset(); }

    IplHandle(const IplHandle&) = delete;
    IplHandle& operator=(const IplHandle&) = delete;

    IplHandle(IplHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    IplHandle& operator=(IplHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    void reset() noexcept {
        if (handle_ != nullptr) {
            Release(&handle_);
            handle_ = nullptr;
        }
    }

    [[nodiscard]] T get() const noexcept { return handle_; }
    /// For Steam Audio's `create(..., T* out)` functions. Releases any current object first.
    [[nodiscard]] T* out() noexcept {
        reset();
        return &handle_;
    }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    T handle_ = nullptr;
};

using Context = IplHandle<IPLContext, iplContextRelease>;
using EmbreeDevice = IplHandle<IPLEmbreeDevice, iplEmbreeDeviceRelease>;
using Scene = IplHandle<IPLScene, iplSceneRelease>;
using StaticMesh = IplHandle<IPLStaticMesh, iplStaticMeshRelease>;
using Simulator = IplHandle<IPLSimulator, iplSimulatorRelease>;
using Source = IplHandle<IPLSource, iplSourceRelease>;
using Hrtf = IplHandle<IPLHRTF, iplHRTFRelease>;
using DirectEffect = IplHandle<IPLDirectEffect, iplDirectEffectRelease>;
using BinauralEffect = IplHandle<IPLBinauralEffect, iplBinauralEffectRelease>;
using PanningEffect = IplHandle<IPLPanningEffect, iplPanningEffectRelease>;
using InstancedMesh = IplHandle<IPLInstancedMesh, iplInstancedMeshRelease>;
using AmbisonicsEncodeEffect = IplHandle<IPLAmbisonicsEncodeEffect, iplAmbisonicsEncodeEffectRelease>;
using AmbisonicsDecodeEffect = IplHandle<IPLAmbisonicsDecodeEffect, iplAmbisonicsDecodeEffectRelease>;
using ReflectionEffect = IplHandle<IPLReflectionEffect, iplReflectionEffectRelease>;
using ProbeArray = IplHandle<IPLProbeArray, iplProbeArrayRelease>;
using ProbeBatch = IplHandle<IPLProbeBatch, iplProbeBatchRelease>;
using PathEffect = IplHandle<IPLPathEffect, iplPathEffectRelease>;

/// Human-readable name for an IPLerror.
[[nodiscard]] const char* error_name(IPLerror error) noexcept;

}  // namespace vsa::steam
