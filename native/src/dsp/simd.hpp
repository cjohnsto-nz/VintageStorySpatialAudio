#pragma once

// Minimal 4-wide float SIMD for the hot DSP loops. Only the two architectures we ship:
// x86-64 (SSE2 is part of the baseline, so no CPU dispatch is needed) and arm64 (NEON).

#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#define VSA_SIMD_SSE2 1
#elif defined(_M_ARM64) || defined(__aarch64__)
#include <arm_neon.h>
#define VSA_SIMD_NEON 1
#else
#error "vsaudio supports x86-64 and arm64 only"
#endif

namespace vsa::dsp::simd {

#if defined(VSA_SIMD_SSE2)

using f4 = __m128;
inline f4 load(const float* p) noexcept { return _mm_loadu_ps(p); }
inline void store(float* p, f4 v) noexcept { _mm_storeu_ps(p, v); }
inline f4 splat(float x) noexcept { return _mm_set1_ps(x); }
inline f4 zero() noexcept { return _mm_setzero_ps(); }
/// a * b + c
inline f4 madd(f4 a, f4 b, f4 c) noexcept { return _mm_add_ps(_mm_mul_ps(a, b), c); }
inline float hsum(f4 v) noexcept {
    const __m128 swapped = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 3, 0, 1));
    const __m128 pairs = _mm_add_ps(v, swapped);
    const __m128 high = _mm_movehl_ps(swapped, pairs);
    return _mm_cvtss_f32(_mm_add_ss(pairs, high));
}

#else

using f4 = float32x4_t;
inline f4 load(const float* p) noexcept { return vld1q_f32(p); }
inline void store(float* p, f4 v) noexcept { vst1q_f32(p, v); }
inline f4 splat(float x) noexcept { return vdupq_n_f32(x); }
inline f4 zero() noexcept { return vdupq_n_f32(0.0f); }
inline f4 madd(f4 a, f4 b, f4 c) noexcept { return vmlaq_f32(c, a, b); }
inline float hsum(f4 v) noexcept { return vaddvq_f32(v); }

#endif

}  // namespace vsa::dsp::simd
