# ADR 0015: Multichannel beds play from their speakers

**Status:** Accepted, 20 Sep 2026.

## Context

Chris's VintageStorySurroundSound mod replaces the game's weather tracks (rain, wind, hail, rumble, distant thunder) with 5.1 recordings. Under vanilla OpenAL it plays them straight to the speakers (`AL_SOFT_direct_channels`). It is to live on as a separate weather-only mod next to this one.

The engine took mono and stereo only. A 6-channel asset failed to decode, and the sound was dropped. Unpositioned stereo went to the front pair, so even a downmix would have lost the surround.

The assets are 5.1 Ogg Vorbis (Vorbis channel order: FL C FR surround-left surround-right LFE) and one plain 6-channel WAV (default order: FL FR FC LFE BL BR). The LFE channels are silent.

## Decision

- **Assets of up to 8 channels decode and stream.** Each asset records which speaker every channel is for (`SourceLayout`): its direction, or LFE.
  - Ogg follows the Vorbis specification's order for its channel count.
  - WAV follows the extensible format's speaker mask, or Windows' default order for the count when there is no mask.
  - Raw PCM uses WAV's default order.
  - A layout with a single surround pair (quad, 5.0, 5.1) has it at ±110°, where ITU-R BS.775 puts 5.1 surrounds, whether the file calls them back or side. With both pairs (7.1), the sides are at ±90° and the backs at ±150°.
- **An unpositioned sound of more than two channels is a bed.** Each channel plays from its speaker direction, head-locked, as the speakers themselves are.
  - **Speakers, four or more (`BedPanner`):** VBAP over the output's layout. A layout without sides has its back pair at 110°, so a 5.1 bed on 5.1 is copied channel for channel. On 7.1 and 7.1.4, 5.1 surrounds fall between the sides and backs, nearer the sides.
  - **Stereo:** panned by azimuth. A channel behind is mirrored to the front at −3 dB, as BS.775's downmix weights the surrounds.
  - **LFE:** to the LFE channel where the layout has one, otherwise the front pair at −3 dB.
  - **Headphones:** a second, head-locked order-3 Ambisonic bus, decoded binaurally facing ahead with its own decoder. The directions are fixed, so there are no coefficient ramps. The world bus's diffuse-field make-up applies.
- **Positioned voices hear every asset as mono.** A bed is averaged over its speaker channels, without the LFE.
- **The ABI is unchanged.** Only the accepted channel count and the header's documentation change.

## Alternatives considered

- **The weather mod splits its beds into mono files and plays them as head-locked voices at the speaker angles.** This needs no engine change. But it takes five or six voices per track, keeping them sample-locked relies on commands landing in the same block, and every other mod's multichannel asset would still fail.
- **Steam Audio's virtual surround effect for headphones.** It is one effect per layout per voice, created off the audio thread. The shared head bus serves every bed with one decode.
- **Converting bed directions to world space and using the world bus.** This needs per-channel coefficient ramps as the head turns (state for eight channels per voice). A second bus is simpler and cheaper.

## Consequences

- A weather mod can replace vanilla's weather tracks with 5.1 (or up to 7.1) files and have them play through this engine as intended. It needs no OpenAL code for our engine.
- The render path still allocates nothing with beds on either tier (`test_render_budget.cpp`).
- With a bed playing, headphones cost one more binaural decode per block.
- Tested: `core/test_bed_panner.cpp` covers layouts and gains. `test_beds.cpp` covers 5.1 on 5.1, 7.1.4, stereo and headphones, bus gains, the positioned downmix and a streamed 5.1 Ogg.
