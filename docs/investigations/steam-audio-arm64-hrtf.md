# Steam Audio 4.8.1's built-in HRTF on Apple Silicon (20 Sep 2026)

The first macOS CI run failed one assertion in `headphone tiers: sources behind sound duller than
in front`. It is not our defect, and it is not noise: **Steam Audio 4.8.1's built-in default HRTF
renders rear sources with almost none of their high-frequency roll-off on arm64.**

## What was measured

`brightness` is the probe's high-band level minus its low-band level (test_spatial.cpp). Front is
`z = -3`, rear `z = +3`, listener at the origin. The number the test gates on is front minus rear:
a source behind should be the duller of the two.

| tier | measurement | Windows x64 | macOS arm64 |
| --- | --- | --- | --- |
| ambisonic bus | front − rear brightness | +7.15 dB | +7.15 dB |
| binaural (built-in HRTF) | front − rear brightness | **+2.15 dB** | **−0.76 dB** |
| binaural (built-in HRTF) | rear brightness | −5.11 dB | −1.59 dB |
| binaural (MIT KEMAR SOFA) | rear brightness | −0.4758 dB | −0.4749 dB |

## What it is not

- **Not our code.** Nothing on the binaural path is architecture-dependent. `dsp/simd.hpp` has an
  SSE2 and a NEON path but only the resampler uses it, and nothing resamples here (asset and
  engine are both 48 kHz).
- **Not the SIMD level we ask for.** `max_simd_level()` returns `IPL_SIMDLEVEL_NEON` on arm64,
  which *is* `IPL_SIMDLEVEL_SSE2` — the same enumerator. Forcing SSE2 on Windows changes every
  number in the file by less than 0.0001 dB.
- **Not HRTF interpolation landing on a different neighbour.** `IPL_HRTFINTERPOLATION_BILINEAR`
  could in principle be discontinuous where the rear source sits at azimuth 180°, so a
  floating-point difference of one ulp in the direction would matter. It is not: sweeping the
  source from `x = -0.2` to `x = +0.2` moves rear brightness smoothly from −6.48 dB to −3.89 dB,
  with no step at zero. Reaching arm64's −1.59 dB that way would need a direction error of about
  11°, which would also have moved the left/right balance, and the balance assertions pass.
- **Not the convolution.** Given the *same* HRTF read from a SOFA file, the two platforms agree at
  the rear to 0.001 dB. Whatever differs is in the HRTF the library supplies, not in what it does
  with one.

## What it leaves

Steam Audio's arm64 slice either embeds different default HRIRs or prepares them differently when
it loads them. The library is closed source, so that is as far as this goes from here.

## What it means for the mod

Apple Silicon players get a weaker front/back cue than everyone else for as long as the mod uses
the built-in HRTF. The fix is available and already half-built: `hrtf_sofa_path` works, and the
same MIT KEMAR file the tests use renders identically on both platforms. Shipping an HRTF of our
own would settle this and take the front/back cue out of Steam Audio's hands on every platform.
That is a Phase 8 decision, not a CI one.

Until then the binaural half of that test asserts only on x64. The ambisonic tier, which is
bit-identical everywhere, still gates on both.

## Reproducing

The numbers above come from `native/tests/test_spatial.cpp` as it stands, plus a throwaway test
that swept the source position and swapped in the KEMAR SOFA file on both platforms. Anything here
can be re-measured by running `vsaudio_tests --source-file="*test_spatial.cpp"` and reading the
`MESSAGE` lines, which report every value whether the test passes or not.
