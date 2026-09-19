# ADR 0002: Output backends

**Status:** Accepted, 19 Sep 2026

## Context

We need headphones (Steam Audio HRTF), stereo, 5.1, 7.1, 7.1.4, and Windows Sonic / Dolby Atmos with dynamic objects, on Windows, Linux and macOS.

OpenAL cannot take buffers with more than 8 channels, would add a second buffering stage, and would require feeding it from managed code. The VintageStorySurroundSound work showed that Windows Spatial Audio (a 12-channel 7.1.4 bed) works on the target receiver. It also showed that OpenAL Soft's activation path had a `PROPVARIANT` ownership bug.

## Decision

- **Main backend:** miniaudio (public domain / MIT-0) for device I/O on every platform: WASAPI, CoreAudio, PipeWire, PulseAudio and ALSA. It handles stereo, 5.1 and 7.1, and binaural output over stereo.
- **Windows Spatial backend:** our own `ISpatialAudioClient` backend, with a 7.1.4 (or 8.1.4.4) static bed plus dynamic objects for the most important voices. It queries `GetMaxDynamicObjectCount()` and falls back to the bed, then to miniaudio.
- **OpenAL:** used only by vanilla at the main menu (ADR 0006).

## Consequences

- We own device enumeration, hot-plug and the settings-menu device list.
- The Windows Spatial backend handles COM ownership rules correctly. That is the same bug class as OpenAL Soft's, and it gets explicit tests.
- The HRTF is never applied twice: when Windows Sonic or Atmos-for-headphones is active, we send objects and a bed, never binaural audio.
