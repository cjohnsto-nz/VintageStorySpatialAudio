/*
 * vsaudio — native audio engine for Vintage Story Steam Audio.
 *
 * This is the only interface between the managed mod and the native engine.
 * Rules for this header:
 *   - Plain C, fixed-width types, no C++ or STL types crossing the boundary.
 *   - Every struct passed in or out starts with `struct_size`; callers set it to
 *     sizeof(the struct they were compiled against). The engine rejects sizes it
 *     does not understand, so managed/native mismatches fail loudly instead of
 *     corrupting memory.
 *   - Functions never throw across the boundary. Failures return a vsa_result and
 *     set a thread-local message readable through vsa_get_last_error().
 *   - Struct fields that carry enum values are declared uint32_t, never as the enum
 *     type: the caller may send any 32-bit value, and loading an out-of-range value
 *     through an enum type is undefined behaviour in C++. The engine validates them.
 *   - Bump VSA_ABI_VERSION on any incompatible change to this file.
 */
#ifndef VSAUDIO_H
#define VSAUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define VSA_CALL __cdecl
#  if defined(VSA_BUILDING_LIBRARY)
#    define VSA_API __declspec(dllexport)
#  else
#    define VSA_API __declspec(dllimport)
#  endif
#else
#  define VSA_CALL
#  if defined(VSA_BUILDING_LIBRARY)
#    define VSA_API __attribute__((visibility("default")))
#  else
#    define VSA_API
#  endif
#endif

/** Version of the binary interface described by this header. */
#define VSA_ABI_VERSION 17u

typedef enum vsa_result {
    VSA_OK = 0,
    VSA_ERROR_INVALID_ARGUMENT = 1,
    VSA_ERROR_ABI_MISMATCH = 2,
    VSA_ERROR_ALREADY_EXISTS = 3,
    VSA_ERROR_STEAM_AUDIO = 4,
    VSA_ERROR_OUT_OF_MEMORY = 5,
    VSA_ERROR_UNSUPPORTED = 6,
    VSA_ERROR_INTERNAL = 7,
    /** A voice handle is stale (released) or was never valid. */
    VSA_ERROR_INVALID_HANDLE = 8,
    /** Audio data could not be decoded. */
    VSA_ERROR_DECODE = 9,
    /** The audio device could not be opened or enumerated. */
    VSA_ERROR_DEVICE = 10,
    /** The call is not valid in the engine's current state (e.g. offline render while a device is open). */
    VSA_ERROR_INVALID_STATE = 11,
    /** A fixed-capacity resource is exhausted (voice slots, command queue). */
    VSA_ERROR_CAPACITY = 12
} vsa_result;

typedef enum vsa_log_level {
    VSA_LOG_DEBUG = 0,
    VSA_LOG_INFO = 1,
    VSA_LOG_WARNING = 2,
    VSA_LOG_ERROR = 3
} vsa_log_level;

/**
 * Log sink. May be invoked from any engine thread except the real-time render
 * thread (which queues its messages instead), so implementations must be
 * thread-safe. `message` is only valid for the duration of the call.
 */
typedef void (VSA_CALL *vsa_log_fn)(void* user_data, vsa_log_level level, const char* message);

/** Which ray tracer backs Steam Audio scenes. */
typedef enum vsa_ray_tracer {
    /** Embree if the device can be created on this machine, otherwise Steam Audio's built-in tracer. */
    VSA_RAY_TRACER_AUTO = 0,
    /** Intel Embree. Creation fails with VSA_ERROR_UNSUPPORTED if unavailable. */
    VSA_RAY_TRACER_EMBREE = 1,
    /** Steam Audio's built-in ray tracer (IPL_SCENETYPE_DEFAULT). Always available. */
    VSA_RAY_TRACER_STEAM = 2
} vsa_ray_tracer;

typedef struct vsa_version_info {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t engine_major;
    uint32_t engine_minor;
    uint32_t engine_patch;
    /** Steam Audio version the engine was compiled against. */
    uint32_t steam_audio_major;
    uint32_t steam_audio_minor;
    uint32_t steam_audio_patch;
    /** Static, NUL-terminated build description (compiler, target, configuration). */
    const char* build_description;
} vsa_version_info;

enum {
    /** Enables Steam Audio's API validation layer. Slow; for development builds only. */
    VSA_ENGINE_FLAG_STEAM_AUDIO_VALIDATION = 1u << 0,
    /** Disables the direct simulation: no occlusion or transmission by the world scene. */
    VSA_ENGINE_FLAG_NO_DIRECT_SIMULATION = 1u << 1,
    /** Disables the reflections: no reverb from the world scene. */
    VSA_ENGINE_FLAG_NO_REFLECTIONS = 1u << 2,
    /** Disables pathing: no sound round corners and through doorways beyond what reflects. */
    VSA_ENGINE_FLAG_NO_PATHING = 1u << 3
};

/** Resampler quality: zero crossings per side of the bandlimited-interpolation kernel. */
typedef enum vsa_resampler_quality {
    /** Medium. */
    VSA_RESAMPLER_DEFAULT = 0,
    /** 4 zero crossings. */
    VSA_RESAMPLER_LOW = 1,
    /** 8 zero crossings. */
    VSA_RESAMPLER_MEDIUM = 2,
    /** 16 zero crossings. */
    VSA_RESAMPLER_HIGH = 3
} vsa_resampler_quality;

typedef struct vsa_engine_config {
    uint32_t struct_size;
    /** Must equal VSA_ABI_VERSION. */
    uint32_t abi_version;
    /** Optional log sink; NULL discards engine log output. */
    vsa_log_fn log;
    void* log_user_data;
    /** A vsa_ray_tracer value. */
    uint32_t ray_tracer;
    /** Combination of VSA_ENGINE_FLAG_* values. */
    uint32_t flags;
    /** Rate of the initial offline output (VSA_OUTPUT_NONE): 44100 or 48000, 0 = 48000. */
    uint32_t sample_rate;
    /** Frames per engine block (the render granularity). 0 = 256; 32..4096. */
    uint32_t block_frames;
    /** Voice slot capacity. A storage bound, not an audibility cap. 0 = 4096; 1..65536. */
    uint32_t max_voices;
    /** A vsa_resampler_quality value. */
    uint32_t resampler_quality;
    /** Ogg assets with storage AUTO longer than this are streamed. 0 = 20000 ms. */
    uint32_t stream_threshold_ms;
    /**
     * Positional voices rendered with their own Steam Audio effects at once (the rest are
     * virtual: silent, but still advancing). 0 = 256; 1..4096.
     */
    uint32_t max_real_voices;
    /**
     * Of those, how many get per-voice HRTF rendering in headphones mode (the loudest ones; the
     * rest share a world-space order-3 Ambisonic bus decoded binaurally once per block).
     * 0 = 64; at most max_real_voices.
     */
    uint32_t max_binaural_voices;
    /** Must be 0. */
    uint32_t reserved;
    /**
     * A SOFA file (UTF-8 path) with the HRTF to use instead of Steam Audio's default, or NULL.
     * Copied at creation. If it cannot be loaded (at the output's rate), the engine logs a
     * warning and uses the default HRTF.
     */
    const char* hrtf_sofa_path;
    /**
     * Direct simulation (occlusion by the world scene): rays per source for Steam Audio's
     * volumetric occlusion, 0 = 16; 1..256.
     */
    uint32_t occlusion_samples;
    /** Direct simulation updates per second while a device plays, 0 = 30; 1..120. */
    uint32_t direct_rate_hz;
    /*
     * Reflections (Phase 6): Steam Audio's ray-traced reflections against the world scene, for
     * every sound from where it is (ADR 0012). 0 = the default for each.
     */
    /** Places simulated at once (sounds within 3 m of one another share one), 0 = 10; 1..64. */
    uint32_t reflection_sources;
    /** Rays traced from the listener per simulation, 0 = 4096; 256..32768. */
    uint32_t reflection_rays;
    /** Bounces per ray, 0 = 16; 1..64. */
    uint32_t reflection_bounces;
    /** Impulse response length in seconds, 0 = 1; 0.25..4. */
    float reflection_duration;
    /** Ambisonic order of the reflections, 0 = 2; 1..3. */
    uint32_t reflection_order;
    /**
     * Reflection simulations per second at most while a device plays, 0 = 10; 1..60. A run that
     * takes longer is followed by a rest as long as itself (the simulation's threads are busy at
     * most half the time, whatever the settings).
     */
    uint32_t reflection_rate_hz;
    /** Worker threads for one simulation, 0 = a quarter of the cores (1..4); 1..32. */
    uint32_t reflection_threads;
    /**
     * Seconds of each impulse response rendered by convolution (early reflections, directional);
     * the rest is a parametric tail at the simulated decay time. 0 = 0.1; 0.02..0.5, below
     * reflection_duration.
     */
    float reflection_transition;
    /*
     * Pathing (Phase 7, ADR 0014): sound round corners and through doorways, from Steam Audio's
     * baked probe paths. A box round the listener is baked in the background and baked again
     * when the listener leaves its middle or blocks in it change. 0 = the default for each.
     */
    /** The box baked round the listener: blocks across (x and z), 0 = 64; 32..256; and high,
     *  0 = 64; 16..128. */
    uint32_t pathing_range;
    uint32_t pathing_height;
    /** Metres between probes, 0 = 2.5; 1..8. */
    float pathing_probe_spacing;
    /** Point samples per probe when testing whether two probes see each other, 0 = 1; 1..8. */
    uint32_t pathing_vis_samples;
    /** Pathing simulations per second while a device plays, 0 = 10; 1..60. */
    uint32_t pathing_rate_hz;
    /** Sounds given paths per simulation at most (the loudest blocked ones), 0 = 16; 1..256. */
    uint32_t pathing_sources;
    /** Probes baked at most: the spacing widens to keep within it, since a bake costs about
     *  probes^2.2 and the terrain decides how many a box holds (ADR 0015). 0 = 1200; 64..65536. */
    uint32_t pathing_max_probes;
} vsa_engine_config;

typedef struct vsa_engine_info {
    uint32_t struct_size;
    /** The vsa_ray_tracer actually in use (never VSA_RAY_TRACER_AUTO). */
    uint32_t active_ray_tracer;
    /** Non-zero if an Embree device could be created on this machine. */
    uint32_t embree_available;
} vsa_engine_info;

typedef struct vsa_self_test_report {
    uint32_t struct_size;
    /** Non-zero if every check passed. */
    uint32_t passed;
    /** Steam Audio occlusion (1 = unoccluded, 0 = fully occluded) for a path through a wall. Expected ~0. */
    float occlusion_through_wall;
    /** Steam Audio occlusion for a path that misses the wall. Expected ~1. */
    float occlusion_clear_path;
    /** Wall-clock time for building the scene and running the simulation, in milliseconds. */
    double elapsed_ms;
} vsa_self_test_report;

typedef struct vsa_engine vsa_engine;

/** Fills `out` with version information. Never fails for a correctly sized struct. */
VSA_API vsa_result VSA_CALL vsa_get_version(vsa_version_info* out);

/**
 * Creates the engine: Steam Audio context and ray tracer. Only one engine may
 * exist per process (Steam Audio's log callback is process-global), a second
 * call fails with VSA_ERROR_ALREADY_EXISTS.
 */
VSA_API vsa_result VSA_CALL vsa_engine_create(const vsa_engine_config* config, vsa_engine** out_engine);

/** Destroys the engine. Accepts NULL. */
VSA_API void VSA_CALL vsa_engine_destroy(vsa_engine* engine);

VSA_API vsa_result VSA_CALL vsa_engine_get_info(const vsa_engine* engine, vsa_engine_info* out);

/**
 * Builds a small scene (one wall) with the active ray tracer and runs Steam
 * Audio direct simulation for a blocked and a clear path. Proves the native
 * stack works end to end on this machine. Safe to call at any time.
 */
VSA_API vsa_result VSA_CALL vsa_engine_run_self_test(vsa_engine* engine, vsa_self_test_report* out);

/* ------------------------------------------------------------------------------------------ */
/* Assets                                                                                      */
/* ------------------------------------------------------------------------------------------ */

typedef enum vsa_asset_format {
    /** Detect from the data ("OggS" or "RIFF....WAVE"). */
    VSA_ASSET_FORMAT_AUTO = 0,
    VSA_ASSET_FORMAT_OGG_VORBIS = 1,
    /** RIFF WAVE: PCM 8/16/24/32-bit or IEEE float 32/64-bit, plain or WAVE_FORMAT_EXTENSIBLE. */
    VSA_ASSET_FORMAT_WAV = 2,
    /** Raw interleaved signed 16-bit PCM in host byte order; set pcm_channels and pcm_sample_rate. */
    VSA_ASSET_FORMAT_PCM_S16 = 3
} vsa_asset_format;

typedef enum vsa_asset_storage {
    /** Stream Ogg assets longer than the engine's stream threshold, decode everything else. */
    VSA_ASSET_STORAGE_AUTO = 0,
    /** Decode fully to 16-bit PCM at the source rate. */
    VSA_ASSET_STORAGE_DECODED = 1,
    /** Keep the encoded bytes; each voice decodes ahead on the engine's worker thread. Ogg only. */
    VSA_ASSET_STORAGE_STREAMED = 2
} vsa_asset_storage;

typedef struct vsa_asset_desc {
    uint32_t struct_size;
    /** A vsa_asset_format value. */
    uint32_t format;
    /** The encoded file (or raw PCM). Copied or decoded during the call; the caller keeps ownership. */
    const void* data;
    uint64_t size;
    /** Optional NUL-terminated UTF-8 name for diagnostics. Copied. */
    const char* name;
    /** A vsa_asset_storage value. */
    uint32_t storage;
    /**
     * VSA_ASSET_FORMAT_PCM_S16 only: 1 to 8, interleaved in WAV's default order for the count
     * (FL FR FC LFE BL BR SL SR; see VSA_SPATIAL_NONE for beds).
     */
    uint32_t pcm_channels;
    /** VSA_ASSET_FORMAT_PCM_S16 only. */
    uint32_t pcm_sample_rate;
} vsa_asset_desc;

typedef struct vsa_asset_info {
    uint32_t struct_size;
    /** 1 to 8. */
    uint32_t channels;
    uint32_t sample_rate;
    /** The storage actually used (DECODED or STREAMED). */
    uint32_t storage;
    uint64_t frames;
    /** Native memory held by the asset itself (excluding per-voice stream buffers). */
    uint64_t memory_bytes;
    double duration_seconds;
} vsa_asset_info;

typedef struct vsa_asset vsa_asset;

/**
 * Decodes (or, for streamed storage, validates and copies) an asset on the calling thread.
 * Assets are reference counted and independent of the engine's lifetime: the caller owns
 * one reference, released with vsa_asset_release; every voice playing it holds another.
 */
VSA_API vsa_result VSA_CALL vsa_asset_create(vsa_engine* engine, const vsa_asset_desc* desc, vsa_asset** out_asset);
VSA_API vsa_result VSA_CALL vsa_asset_get_info(const vsa_asset* asset, vsa_asset_info* out);
/** Drops the caller's reference. Accepts NULL. */
VSA_API void VSA_CALL vsa_asset_release(vsa_asset* asset);

/* ------------------------------------------------------------------------------------------ */
/* Voices                                                                                      */
/* ------------------------------------------------------------------------------------------ */

/** Mix buses. They mirror the game's sound categories. */
typedef enum vsa_bus {
    VSA_BUS_SOUND = 0,
    VSA_BUS_ENTITY = 1,
    VSA_BUS_AMBIENT = 2,
    VSA_BUS_WEATHER = 3,
    VSA_BUS_MUSIC = 4
} vsa_bus;
#define VSA_BUS_COUNT 5u

typedef enum vsa_voice_state {
    VSA_VOICE_STOPPED = 0,
    VSA_VOICE_PLAYING = 1,
    VSA_VOICE_PAUSED = 2
} vsa_voice_state;

/**
 * Voice handle. 0 is never valid. Handles are generation-checked: a released handle stays
 * invalid (VSA_ERROR_INVALID_HANDLE) even after its slot is reused.
 */
typedef uint64_t vsa_voice;

/** How a voice is positioned. */
typedef enum vsa_spatial_mode {
    /**
     * Not positioned: straight to its bus (music, UI). Mono is centred at -3 dB per side, stereo
     * plays on the front pair. More channels are a bed (a weather mod's 5.1 rain): each channel
     * plays from its speaker, head-locked (Ogg Vorbis channel order; WAV's speaker mask or default
     * order; a lone surround pair at 110 degrees), panned between speakers where the output lacks
     * one, and binaurally on headphones. The LFE goes to the LFE, or to the front pair.
     * Positioned voices hear every asset as mono: stereo and beds are downmixed.
     */
    VSA_SPATIAL_NONE = 0,
    /** Position in world coordinates, rendered relative to the listener. */
    VSA_SPATIAL_WORLD = 1,
    /** Position in listener space (head-locked): +x right, +y up, -z forward. */
    VSA_SPATIAL_LISTENER = 2
} vsa_spatial_mode;

typedef struct vsa_voice_desc {
    uint32_t struct_size;
    /** A vsa_bus value. */
    uint32_t bus;
    /** The voice takes its own reference. */
    vsa_asset* asset;
    /** Linear gain, >= 0. */
    float gain;
    /** Playback rate multiplier, 0.05..8. */
    float pitch;
    /** Non-zero to loop. */
    uint32_t looping;
    /** A vsa_spatial_mode value. Positioned stereo assets are downmixed to mono. */
    uint32_t spatial;
    float position[3];
    /** Distance (m) within which the source does not get louder; beyond it, 1/distance. 0 = 1 m. */
    float min_distance;
} vsa_voice_desc;

typedef struct vsa_voice_status {
    uint32_t struct_size;
    /**
     * A vsa_voice_state value. Reflects every command already issued, even if the render
     * thread has not applied it yet (a voice reads PLAYING straight after vsa_voice_start).
     */
    uint32_t state;
    /** Playback position in seconds of source time (the pending seek target if a seek is in flight). */
    double position_seconds;
} vsa_voice_status;

enum {
    /** vsa_voice_fade: stop the voice when the fade completes. */
    VSA_FADE_STOP_WHEN_DONE = 1u << 0
};

/**
 * Voice functions are thread-safe and never block on the render thread: they post commands
 * that the render thread applies at its next block. Each voice is created STOPPED.
 */
VSA_API vsa_result VSA_CALL vsa_voice_create(vsa_engine* engine, const vsa_voice_desc* desc, vsa_voice* out_voice);
/** Stops the voice (with a short declick fade if audible) and frees its slot. */
VSA_API vsa_result VSA_CALL vsa_voice_release(vsa_engine* engine, vsa_voice voice);
/** Plays from the current position; resumes a paused voice. */
VSA_API vsa_result VSA_CALL vsa_voice_start(vsa_engine* engine, vsa_voice voice);
/** Pauses, keeping the position. */
VSA_API vsa_result VSA_CALL vsa_voice_pause(vsa_engine* engine, vsa_voice voice);
/** Stops and rewinds to the start. */
VSA_API vsa_result VSA_CALL vsa_voice_stop(vsa_engine* engine, vsa_voice voice);
/** Sets the voice gain with a short smoothing ramp. Cancels a running fade (FADE_DONE with the cancelled flag). */
VSA_API vsa_result VSA_CALL vsa_voice_set_gain(vsa_engine* engine, vsa_voice voice, float gain);
VSA_API vsa_result VSA_CALL vsa_voice_set_pitch(vsa_engine* engine, vsa_voice voice, float pitch);
VSA_API vsa_result VSA_CALL vsa_voice_set_looping(vsa_engine* engine, vsa_voice voice, uint32_t looping);
VSA_API vsa_result VSA_CALL vsa_voice_seek(vsa_engine* engine, vsa_voice voice, double position_seconds);
/** Sets the positioning mode (a vsa_spatial_mode value) and position together. */
VSA_API vsa_result VSA_CALL vsa_voice_set_position(vsa_engine* engine, vsa_voice voice, uint32_t spatial, float x,
                                                   float y, float z);
/**
 * High-frequency damping, as OpenAL's EFX low-pass: `gain_hf` (0..1) is the gain above about
 * 5 kHz; 1 turns it off. The game uses it underwater.
 */
VSA_API vsa_result VSA_CALL vsa_voice_set_lowpass(vsa_engine* engine, vsa_voice voice, float gain_hf);
/**
 * Fades the voice gain to target_gain over `seconds`, linearly in decibels (a geometric
 * curve), then posts VSA_EVENT_FADE_DONE carrying `token`. `flags` is VSA_FADE_* values.
 */
VSA_API vsa_result VSA_CALL vsa_voice_fade(vsa_engine* engine, vsa_voice voice, float target_gain, float seconds,
                                           uint32_t flags, uint64_t token);
/** Lock-free; never waits for the render thread. */
VSA_API vsa_result VSA_CALL vsa_voice_get_status(vsa_engine* engine, vsa_voice voice, vsa_voice_status* out);

/** Bus gain (linear, smoothed). `bus` is a vsa_bus value. */
VSA_API vsa_result VSA_CALL vsa_bus_set_gain(vsa_engine* engine, uint32_t bus, float gain);
/** Master gain (linear, smoothed), applied before the limiter. */
VSA_API vsa_result VSA_CALL vsa_engine_set_master_gain(vsa_engine* engine, float gain);

/* ------------------------------------------------------------------------------------------ */
/* Listener and spatial rendering                                                              */
/* ------------------------------------------------------------------------------------------ */

typedef struct vsa_listener {
    uint32_t struct_size;
    float position[3];
    /** Unit vector the listener faces. */
    float forward[3];
    /** Unit vector up from the listener's head, orthogonal to forward. */
    float up[3];
    /**
     * Added to the position for rendering only (directions and distances the voices are panned
     * and spatialised with), not for the simulation, which listens from `position`. E.g. a little
     * behind the eyes, so sounds at the player's feet are clearly in front rather than straddling
     * front and back speakers. Zero for none.
     */
    float render_offset[3];
} vsa_listener;

typedef enum vsa_render_mode {
    /** Binaural (Steam Audio HRTF) to the front left/right channels. The default. */
    VSA_RENDER_HEADPHONES = 0,
    /**
     * Amplitude panning to the output's speakers: stereo, quad, 5.1, 7.1 or 7.1.4 for 2/4/6/8/12
     * channels (7.1.4 by 3D vector-base panning, overhead sources on the height speakers; channels
     * are routed by speaker position, so the device's own channel order is respected).
     * The LFE channel is not used. Unpositioned voices stay on the front pair in every layout.
     */
    VSA_RENDER_SPEAKERS = 1
} vsa_render_mode;

/** Listener pose for positional voices. Takes effect at the next block. */
VSA_API vsa_result VSA_CALL vsa_listener_set(vsa_engine* engine, const vsa_listener* listener);
/** A vsa_render_mode value. */
VSA_API vsa_result VSA_CALL vsa_engine_set_render_mode(vsa_engine* engine, uint32_t mode);

/* ------------------------------------------------------------------------------------------ */
/* Output                                                                                      */
/* ------------------------------------------------------------------------------------------ */

/** Opaque backend device identifier, as returned by vsa_device_enumerate. */
typedef struct vsa_device_id {
    uint8_t bytes[512];
} vsa_device_id;

typedef struct vsa_device_info {
    uint32_t struct_size;
    /** Non-zero for the system default output. */
    uint32_t is_default;
    /** NUL-terminated UTF-8. */
    char name[256];
    vsa_device_id id;
} vsa_device_info;

typedef enum vsa_output_kind {
    /** No device; audio is produced only by vsa_engine_render_offline. The initial state. */
    VSA_OUTPUT_NONE = 0,
    /** A playback device through the platform backend (WASAPI, CoreAudio, PipeWire/PulseAudio/ALSA). */
    VSA_OUTPUT_DEVICE = 1,
    /**
     * A playback device through Windows Spatial Audio: the 7.1.4 mix (12 channels; `channels` is
     * ignored) goes to the spatial stream's static bed, so the device's spatial sound format
     * (Dolby Atmos, DTS:X, Windows Sonic) renders the heights. Where that is unavailable (another
     * platform, or no spatial format enabled for the device) the engine opens the device as
     * VSA_OUTPUT_DEVICE instead, and does so again whenever the spatial stream fails; the stats'
     * output_kind says which is running. Meant for VSA_RENDER_SPEAKERS.
     */
    VSA_OUTPUT_SPATIAL = 2
} vsa_output_kind;

typedef struct vsa_output_desc {
    uint32_t struct_size;
    /** A vsa_output_kind value. */
    uint32_t kind;
    /** DEVICE: the device to open, or NULL for the system default (and to follow it when it changes). */
    const vsa_device_id* device_id;
    /**
     * 0 = the device's native layout (NONE: 2); otherwise 2, 4, 6, 8 or 12. The engine's order is
     * FL FR, then FC LFE BL BR, SL SR, TFL TFR TBL TBR as the layout has them (quad: FL FR BL BR).
     */
    uint32_t channels;
    /**
     * NONE: the offline rate, 44100 or 48000 (0 = 48000). DEVICE: ignored; the engine renders at
     * the device's native rate when it is 44.1 or 48 kHz, otherwise at the nearest of the two
     * (the backend converts).
     */
    uint32_t sample_rate;
} vsa_output_desc;

typedef struct vsa_engine_stats {
    uint32_t struct_size;
    /** A vsa_output_kind value. */
    uint32_t output_kind;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t block_frames;
    /** The device's period, 0 when there is no device. */
    uint32_t device_period_frames;
    /** Voices known to the render thread. */
    uint32_t active_voices;
    /** Voice slots currently allocated through the API. */
    uint32_t allocated_voices;
    uint32_t max_voices;
    /** Deepest limiter gain reduction since the previous read, in dB (<= 0). */
    float limiter_peak_reduction_db;
    /** Positional voices currently holding Steam Audio effects. */
    uint32_t real_voices;
    /** Voices currently virtual (inaudible: advancing without rendering). */
    uint32_t virtual_voices;
    uint64_t blocks_rendered;
    /** Blocks whose render time exceeded the block period. */
    uint64_t overloads;
    uint64_t stream_underruns;
    uint64_t events_dropped;
    /** Render time per block since the previous read, in microseconds. */
    double render_time_avg_us;
    double render_time_max_us;
    double block_period_us;
    /** The open device's name, "" when there is none. */
    char device_name[256];
} vsa_engine_stats;

/**
 * Enumerates playback devices. Writes up to `capacity` records (out[0].struct_size must be set
 * when capacity > 0) and sets *out_count to the total number of devices.
 */
VSA_API vsa_result VSA_CALL vsa_device_enumerate(vsa_engine* engine, vsa_device_info* out, uint32_t capacity,
                                                 uint32_t* out_count);
/** Switches output. Voices, assets and gains are kept across switches. */
VSA_API vsa_result VSA_CALL vsa_output_open(vsa_engine* engine, const vsa_output_desc* desc);
/**
 * Renders `frames` interleaved float frames with the NONE output's channel count on the calling
 * thread. Deterministic: streams are refilled synchronously. Fails with VSA_ERROR_INVALID_STATE
 * while a device is open.
 */
VSA_API vsa_result VSA_CALL vsa_engine_render_offline(vsa_engine* engine, float* out, uint32_t frames);
VSA_API vsa_result VSA_CALL vsa_engine_get_stats(vsa_engine* engine, vsa_engine_stats* out);

/* ------------------------------------------------------------------------------------------ */
/* Events                                                                                      */
/* ------------------------------------------------------------------------------------------ */

typedef enum vsa_event_type {
    /** A vsa_voice_fade finished (or was cancelled: VSA_EVENT_FLAG_FADE_CANCELLED). `token` is the fade's. */
    VSA_EVENT_FADE_DONE = 1,
    /** A non-looping voice reached its end and stopped. */
    VSA_EVENT_VOICE_ENDED = 2,
    /** A streamed voice ran out of decoded audio and played silence. */
    VSA_EVENT_STREAM_UNDERRUN = 3,
    /** The default device changed and output followed it. */
    VSA_EVENT_DEVICE_REROUTED = 4,
    /** The output device stopped unexpectedly (e.g. unplugged). The engine keeps trying to reopen. */
    VSA_EVENT_DEVICE_LOST = 5,
    /** Output was reopened after VSA_EVENT_DEVICE_LOST (possibly on the default device instead). */
    VSA_EVENT_DEVICE_RESTORED = 6
} vsa_event_type;

enum {
    VSA_EVENT_FLAG_FADE_CANCELLED = 1u << 0
};

typedef struct vsa_event {
    uint32_t struct_size;
    /** A vsa_event_type value. */
    uint32_t type;
    /** The voice concerned, 0 for engine-wide events. */
    vsa_voice voice;
    uint64_t token;
    /** VSA_EVENT_FLAG_* values. */
    uint32_t flags;
} vsa_event;

/**
 * Moves up to `capacity` pending events into `out` (out[0].struct_size must be set when
 * capacity > 0) and sets *out_count. Events are queued until polled; the oldest are dropped
 * beyond an internal bound (counted in vsa_engine_stats.events_dropped).
 */
VSA_API vsa_result VSA_CALL vsa_engine_poll_events(vsa_engine* engine, vsa_event* out, uint32_t capacity,
                                                   uint32_t* out_count);

/* =============================================================================================
 * World scene (Phase 4): the voxel world as Steam Audio geometry.
 *
 * The world arrives as chunk snapshots (32³ cells of acoustic material ids). A scene thread meshes
 * each one (surfaces where a denser material kind meets a more open one, coplanar faces merged)
 * into its own Steam Audio sub-scene, instanced in the top-level scene. Positions the engine gets
 * (listener, voices) are relative to the scene origin, a block position near the listener; the
 * caller moves it with vsa_scene_set_origin when the listener strays far, to keep floats precise.
 * ============================================================================================= */

typedef enum vsa_material_kind {
    /** Never meshed. Material id 0 is always air. */
    VSA_MATERIAL_AIR = 0,
    /** Water, lava: meshed against air. */
    VSA_MATERIAL_LIQUID = 1,
    /** Leaves, plants, cloth: meshed against air and liquids. */
    VSA_MATERIAL_POROUS = 2,
    /** Stone, wood, metal, …: meshed against everything more open. */
    VSA_MATERIAL_SOLID = 3
} vsa_material_kind;

typedef struct vsa_acoustic_material {
    uint32_t struct_size;
    /** A vsa_material_kind value. */
    uint32_t kind;
    /** Surface properties for reflections, 0..1, bands low/mid/high. */
    float absorption[3];
    float scattering;
    /**
     * Sound through the material (the direct simulation): amplitude 0..1 per band each time a
     * path enters it, and the loss per metre inside it, dB per band.
     */
    float transmission[3];
    float attenuation_db_per_metre[3];
    /** UTF-8 name for debugging output (copied), or NULL. */
    const char* name;
} vsa_acoustic_material;

/** Sets the material table: material id = index; id 0 must be air. Re-meshes every chunk. */
VSA_API vsa_result VSA_CALL vsa_scene_set_materials(vsa_engine* engine, const vsa_acoustic_material* materials,
                                                    uint32_t count);

/** Cells per chunk edge. */
#define VSA_CHUNK_SIZE 32u
#define VSA_CHUNK_CELLS (VSA_CHUNK_SIZE * VSA_CHUNK_SIZE * VSA_CHUNK_SIZE)

/** An axis-aligned box inside a block, in block units (0..1). */
typedef struct vsa_box {
    float min[3];
    float max[3];
} vsa_box;

/**
 * A partial block (slab, stairs, fence, door, …): meshed as its boxes; its cell counts as open
 * for its neighbours. `cell` indexes the chunk like vsa_chunk_desc.materials.
 */
typedef struct vsa_partial_block {
    uint32_t cell;
    uint32_t material;
    /** Its boxes: vsa_chunk_desc.boxes[first_box .. first_box + box_count). */
    uint32_t first_box;
    uint32_t box_count;
} vsa_partial_block;

typedef struct vsa_chunk_desc {
    uint32_t struct_size;
    /** Chunk coordinates (block coordinates / 32). */
    int32_t x;
    int32_t y;
    int32_t z;
    /** 0 = full detail; 1 = 2³-block super-voxels (majority material), for the far ring. */
    uint32_t lod;
    /** Must be 0. */
    uint32_t reserved;
    /** VSA_CHUNK_CELLS material ids, index (y * 32 + z) * 32 + x; partial blocks' cells hold 0. */
    const uint16_t* materials;
    const vsa_partial_block* partials;
    uint32_t partial_count;
    uint32_t box_count;
    const vsa_box* boxes;
} vsa_chunk_desc;

/**
 * Adds or replaces a chunk (copied; meshed asynchronously). Fails with INVALID_ARGUMENT for
 * material ids outside the material table, cells out of range or boxes out of range.
 */
VSA_API vsa_result VSA_CALL vsa_scene_set_chunk(vsa_engine* engine, const vsa_chunk_desc* chunk);
/** Removes a chunk (no-op if unknown). */
VSA_API vsa_result VSA_CALL vsa_scene_remove_chunk(vsa_engine* engine, int32_t x, int32_t y, int32_t z);
/** Removes every chunk. */
VSA_API vsa_result VSA_CALL vsa_scene_clear(vsa_engine* engine);
/** Block position of the scene origin; engine positions are relative to it. Initially 0, 0, 0. */
VSA_API vsa_result VSA_CALL vsa_scene_set_origin(vsa_engine* engine, int32_t x, int32_t y, int32_t z);
/**
 * Waits up to `timeout_ms` for pending chunks to be meshed and committed. VSA_ERROR_INVALID_STATE
 * on timeout. For tests and tools.
 */
VSA_API vsa_result VSA_CALL vsa_scene_wait_idle(vsa_engine* engine, uint32_t timeout_ms);

typedef struct vsa_scene_stats {
    uint32_t struct_size;
    /** Chunks held, those with geometry, and those waiting to be meshed or removed. */
    uint32_t chunks;
    uint32_t meshed_chunks;
    uint32_t pending_chunks;
    uint64_t triangles;
    uint64_t vertices;
    /** Voxel and mesh memory (not counting Steam Audio's own copies). */
    uint64_t memory_bytes;
    uint64_t chunks_built;
    double last_build_ms;
    double max_build_ms;
    double last_commit_ms;
    int32_t origin[3];
    uint32_t material_count;
} vsa_scene_stats;

VSA_API vsa_result VSA_CALL vsa_scene_get_stats(vsa_engine* engine, vsa_scene_stats* out);

/**
 * A chunk's mesh exactly as submitted to Steam Audio, for debug views. The caller sets the
 * capacities and buffers (NULL buffers with 0 capacity query the counts). Coordinates are
 * chunk-local block units (add chunk position * 32 for world blocks).
 */
typedef struct vsa_chunk_mesh {
    uint32_t struct_size;
    /** Out: non-zero if the chunk has a mesh (known, meshed and not all air). */
    uint32_t found;
    /** Out: its level of detail. */
    uint32_t lod;
    /** Out: sizes. The buffers are filled only when both capacities suffice. */
    uint32_t vertex_count;
    uint32_t triangle_count;
    /** In: capacities (vertices, triangles) and buffers: 3 floats per vertex, 3 indices and one
     * material id per triangle. */
    uint32_t vertex_capacity;
    uint32_t triangle_capacity;
    /** Out: changes whenever the chunk is meshed again (for caching debug views). */
    uint32_t version;
    float* vertices;
    int32_t* triangles;
    uint16_t* materials;
} vsa_chunk_mesh;

VSA_API vsa_result VSA_CALL vsa_scene_get_chunk_mesh(vsa_engine* engine, int32_t x, int32_t y, int32_t z,
                                                     vsa_chunk_mesh* mesh);

/**
 * Lists the chunks the scene holds: fills up to `capacity` coordinates (3 int32 each) and sets
 * *out_count to the total.
 */
VSA_API vsa_result VSA_CALL vsa_scene_list_chunks(vsa_engine* engine, int32_t* out, uint32_t capacity,
                                                  uint32_t* out_count);

/** A ray's first hit on the scene's meshes (debugging: exactly the geometry Steam Audio has). */
typedef struct vsa_ray_hit {
    uint32_t struct_size;
    /** Out: non-zero if something was hit within the distance. */
    uint32_t hit;
    float distance;
    /** Scene coordinates (relative to the origin). */
    float point[3];
    /** The surface's front: its open side. */
    float normal[3];
    int32_t chunk[3];
    /** Index into the chunk's mesh (vsa_scene_get_chunk_mesh). */
    uint32_t triangle;
    uint32_t material;
    /** Non-zero: a partial block's box; zero: the face of a whole cell. */
    uint32_t from_partial;
    /** World block position that produced the surface (the cell just behind it). */
    int32_t cell[3];
    uint32_t lod;
} vsa_ray_hit;

/**
 * Casts a ray (scene coordinates; the direction need not be normalised) against the meshed
 * scene and reports the first hit within `max_distance`, from either side of a triangle.
 */
VSA_API vsa_result VSA_CALL vsa_scene_raycast(vsa_engine* engine, const float origin[3], const float direction[3],
                                              float max_distance, vsa_ray_hit* hit);

/** Writes the scene as an OBJ file (plus .mtl) in world block coordinates. UTF-8 path. */
VSA_API vsa_result VSA_CALL vsa_scene_save_obj(vsa_engine* engine, const char* path);

/* =============================================================================================
 * Direct simulation (Phase 5): occlusion and transmission for world-positioned voices.
 *
 * Every world voice rendered with its own effects is simulated against the world scene: Steam
 * Audio's volumetric occlusion (the visible fraction of the source), and transmission along the
 * listener-source line through the voxels: each material entered costs its surface transmission
 * once, plus its per-metre attenuation. Where the centre line is clear but the source is partly
 * hidden, the hidden part is attenuated as at an edge. A source inside a solid block (a block
 * being broken) is first moved out of it towards the listener. Results are smoothed over ~60 ms;
 * a new voice waits (up to 80 ms) for its first result rather than play unoccluded.
 * ============================================================================================= */

/** vsa_source_debug.flags: the simulated position was moved out of a solid block. */
#define VSA_SOURCE_ESCAPED (1u << 0)

/** Flags on vsa_audible_voice. */
enum {
    /** Rendered at the listener's head (the player's own sounds, UI). */
    VSA_AUDIBLE_HEAD_LOCKED = 1u << 0,
    /** Inaudible: advancing without being rendered. */
    VSA_AUDIBLE_VIRTUAL = 1u << 1,
    /** Some of it is arriving round a corner (pathing). */
    VSA_AUDIBLE_HAS_PATH = 1u << 2,
    /** It has a place, so it has reflections. */
    VSA_AUDIBLE_HAS_PLACE = 1u << 3,
    /** Rendered with its own HRTF rather than through the shared Ambisonic mix. */
    VSA_AUDIBLE_BINAURAL = 1u << 4
};

/**
 * One sounding voice and the ways it is reaching the listener. The levels are the amplitude
 * each way carries into its effect, in dB, not a measurement of what comes out: enough to say
 * which way a sound arrives by and how much louder one way is than another. -200 is silence.
 */
typedef struct vsa_audible_voice {
    uint32_t struct_size;
    /** A vsa_bus value. */
    uint32_t bus;
    vsa_voice voice;
    /** VSA_AUDIBLE_* flags. */
    uint32_t flags;
    /** Metres to the listener (0 when head-locked). */
    float distance;
    /** The voice itself, after its gain, fades, the bus and the distance. */
    float heard_db;
    /** Of that, what the direct effect passes: what is visible, plus what gets through. */
    float direct_db;
    /** What is arriving round corners. */
    float path_db;
    /** What it is feeding its place's reflections. */
    float reflection_db;
    /** Where the sound is, in scene coordinates. */
    float position[3];
    /**
     * The direction the way round brings it from, a unit vector in scene coordinates: where to
     * look to find the doorway it is coming through. All zero when nothing is arriving that way.
     */
    float arrival[3];
    uint32_t reserved;
} vsa_audible_voice;

/**
 * The voices that sounded in the latest block, loudest first. Needs the inspector on
 * (vsa_engine_set_inspect): off, this reports nothing and costs the render thread nothing.
 * Fills up to `capacity` entries (out[0].struct_size set) and sets *out_count to the total.
 */
VSA_API vsa_result VSA_CALL vsa_engine_get_audible(vsa_engine* engine, vsa_audible_voice* out, uint32_t capacity,
                                                   uint32_t* out_count);

/**
 * Debugging: silences a voice whatever its gain and fades say. A silenced voice asks for no
 * simulation, so with every voice but one silenced, everything the overlays draw is that one's.
 */
VSA_API vsa_result VSA_CALL vsa_voice_set_muted(vsa_engine* engine, vsa_voice voice, uint32_t muted);

/**
 * Debugging: gains (0..4, 1 = as simulated) on the direct sound and on the sound arriving round
 * corners, to hear one way at a time. The reflections have vsa_engine_set_reflection_gain.
 */
VSA_API vsa_result VSA_CALL vsa_engine_set_route_gains(vsa_engine* engine, float direct, float path);

/** Turns the sound inspector on or off. Off by default. */
VSA_API vsa_result VSA_CALL vsa_engine_set_inspect(vsa_engine* engine, uint32_t on);

typedef struct vsa_source_debug {
    uint32_t struct_size;
    uint32_t flags;
    vsa_voice voice;
    /** Scene coordinates: where the voice is, and where it was simulated from. */
    float position[3];
    float simulated_position[3];
    /** Visible fraction 0..1, and amplitude per band through what is in the way. */
    float occlusion;
    float transmission[3];
    /** Metres of material on the centre line, and how many materials it entered. */
    float solid_metres;
    uint32_t crossings;
} vsa_source_debug;

/**
 * The sources simulated in the latest tick: fills up to `capacity` entries (out[0].struct_size
 * set) and sets *out_count to the total.
 */
VSA_API vsa_result VSA_CALL vsa_engine_get_sources(vsa_engine* engine, vsa_source_debug* out, uint32_t capacity,
                                                   uint32_t* out_count);

typedef struct vsa_simulation_stats {
    uint32_t struct_size;
    /** Sources simulated in the latest tick. */
    uint32_t sources;
    uint64_t ticks;
    double last_tick_ms;
    double max_tick_ms;
    /** The latest tick's Steam Audio occlusion part, and the voxel transmission part. */
    double occlusion_ms;
    double transmission_ms;
    uint32_t rate_hz;
    uint32_t occlusion_samples;
    /** Where the latest tick listened from (scene coordinates, after leaving any solid block). */
    float listener[3];
    /** The scene origin the latest tick used. */
    int32_t origin[3];
} vsa_simulation_stats;

VSA_API vsa_result VSA_CALL vsa_engine_get_simulation_stats(vsa_engine* engine, vsa_simulation_stats* out);

/* =============================================================================================
 * Reflections (Phase 6): reverb simulated from the world scene (ADR 0012).
 *
 * Every world sound is simulated by Steam Audio from where it is: rays from the listener find
 * the paths its sound takes off the scene's surfaces (their absorption and scattering), so its
 * reflections come from the right directions, round corners and through doorways, and not
 * through walls. Sounds within 3 m of one another share a "place", which keeps its simulation
 * (refining it) while sounds keep happening there: a sound struck again and again at one spot
 * always rings the same. A new place's first result is run at once (the first 10-20 ms of a new
 * place's reflections are lost; nothing else changes). Head-locked sounds use a source at the
 * listener.
 *
 * Per place, the early reflections are convolved (directional, Ambisonic) and the tail is a
 * diffuse reverb at the simulated decay time per band, decoded with the world's Ambisonic bus
 * (headphones) or to the speaker layout, heights included.
 * ============================================================================================= */

typedef struct vsa_reflection_stats {
    uint32_t struct_size;
    /** Non-zero if the reflections run. */
    uint32_t enabled;
    /** Places (the listener's not counted), and how many are rendering reflections, waiting for
     *  their first simulation, or letting a tail die away after their sounds ended. */
    uint32_t slots;
    uint32_t live_slots;
    uint32_t waiting_slots;
    uint32_t draining_slots;
    /** The settings in use. */
    uint32_t rays;
    uint32_t bounces;
    uint32_t order;
    uint32_t rate_hz;
    uint32_t threads;
    float duration;
    float transition;
    uint32_t reserved;
    uint64_t ticks;
    double last_tick_ms;
    double max_tick_ms;
    /** The latest tick's Steam Audio run. */
    double simulate_ms;
    /** Decay time (RT60, seconds) at the listener, bands below 800 Hz, to 8 kHz, above. */
    float listener_reverb_times[3];
    /** Level of the reflections' output (omnidirectional), dB full scale; -120 when silent. */
    float output_db;
    /** vsa_engine_set_reflection_gain. */
    float gain;
    /** Where the latest simulation listened from (scene coordinates). */
    float listener[3];
} vsa_reflection_stats;

VSA_API vsa_result VSA_CALL vsa_engine_get_reflection_stats(vsa_engine* engine, vsa_reflection_stats* out);

typedef struct vsa_reflection_source {
    uint32_t struct_size;
    /** 0: the listener's own reverb (head-locked sounds); otherwise a place. */
    uint32_t slot;
    /** The voice that made the place; other sounds within 3 m share it. */
    vsa_voice voice;
    /** Simulated from (scene coordinates; moved out of any solid block). */
    float position[3];
    /** Decay time per band, seconds. */
    float reverb_times[3];
    /** The tail's starting level per band (amplitude). */
    float eq[3];
    /** Samples from the sound to the start of its tail. */
    int32_t delay;
} vsa_reflection_source;

/**
 * The sources simulated in the latest reflection tick: fills up to `capacity` entries
 * (out[0].struct_size set) and sets *out_count to the total.
 */
VSA_API vsa_result VSA_CALL vsa_engine_get_reflection_sources(vsa_engine* engine, vsa_reflection_source* out,
                                                              uint32_t capacity, uint32_t* out_count);

/** Scales every reflection (smoothly): 1 = as simulated; 0..4. */
VSA_API vsa_result VSA_CALL vsa_engine_set_reflection_gain(vsa_engine* engine, float gain);

/**
 * Scales the two parts of the reflections separately (smoothly, on top of the reflection gain):
 * the convolved early reflections, and the diffuse tail. 1 = as simulated; 0 turns a part off;
 * 0..4.
 */
VSA_API vsa_result VSA_CALL vsa_engine_set_reflection_mix(vsa_engine* engine, float early_gain, float tail_gain);

/* =============================================================================================
 * Pathing (Phase 7, ADR 0014): sound round corners and through doorways.
 *
 * Steam Audio's baked pathing: probes 1.6 m above every floor of a box round the listener, the
 * shortest paths between them baked in the background (about half a second per 64^3 of world on
 * one thread), and, for each sound whose straight path is blocked, the path from it to the
 * listener looked up and validated against the live scene several times a second. Its sound is
 * then also heard arriving from the way round, at the attenuation of that way's length, through
 * an order-1 Ambisonic path effect decoded with the reflections.
 * ============================================================================================= */

typedef struct vsa_pathing_stats {
    uint32_t struct_size;
    /** Non-zero if pathing runs. */
    uint32_t enabled;
    /** The baker: whether a bake runs now, whether one is due, bakes so far, timings, and the
     *  current batch's probes and box centre (world block coordinates). */
    uint32_t baking;
    uint32_t bake_due;
    uint64_t bakes;
    double last_bake_ms;
    double max_bake_ms;
    uint32_t probes;
    /** Bakes abandoned because the listener left the box before they finished. */
    uint32_t cancelled_bakes;
    double box_centre[3];
    /** The simulation: runs, timings, and in the latest run how many sounds wanted a path, how
     *  many were simulated and how many have one. */
    uint64_t ticks;
    double last_tick_ms;
    double max_tick_ms;
    uint32_t wanted;
    uint32_t simulated;
    uint32_t found;
    uint32_t rate_hz;
    /** Where the latest run listened from (scene coordinates). */
    float listener[3];
    /** Metres between the current batch's probes: wider than configured when the budget bit. */
    float probe_spacing;
} vsa_pathing_stats;

VSA_API vsa_result VSA_CALL vsa_engine_get_pathing_stats(vsa_engine* engine, vsa_pathing_stats* out);

/** One leg of a path the pathing simulation considered in its latest run (scene coordinates). */
typedef struct vsa_path_segment {
    uint32_t struct_size;
    /** Non-zero: the leg is blocked in the live scene. */
    uint32_t occluded;
    float from[3];
    float to[3];
} vsa_path_segment;

/**
 * The path legs of the latest pathing run: fills up to `capacity` (out[0].struct_size set) and
 * sets *out_count to the total.
 */
VSA_API vsa_result VSA_CALL vsa_engine_get_path_segments(vsa_engine* engine, vsa_path_segment* out, uint32_t capacity,
                                                         uint32_t* out_count);

/* ---- Profiling (Phase 8) ------------------------------------------------------------------- */

/** Whose a thread is. */
typedef enum vsa_thread_kind {
    /** One of the engine's, named. */
    VSA_THREAD_ENGINE = 0,
    /** Steam Audio's own workers (its ray tracing runs on threads it creates). */
    VSA_THREAD_STEAM_AUDIO = 1,
    /** The rest of the process: the game, the runtime, drivers; named by module. */
    VSA_THREAD_OTHER = 2
} vsa_thread_kind;

typedef struct vsa_thread_stats {
    uint32_t struct_size;
    /** A vsa_thread_kind value. */
    uint32_t kind;
    uint32_t thread_id;
    uint32_t reserved;
    /** User + kernel CPU time since the thread started, in milliseconds. */
    double cpu_ms;
    /** The engine's name for it, or (Windows) the module its start address lies in. */
    char name[48];
} vsa_thread_stats;

/**
 * The process's threads and their CPU time: the engine's by name; on Windows every other thread
 * too, classified by module, so the game's and Steam Audio's own can be told from ours. Two
 * readings some seconds apart give each thread's share of a core. Fills up to `capacity` entries
 * (out[0].struct_size set) and sets *out_count to the total.
 */
VSA_API vsa_result VSA_CALL vsa_engine_get_thread_stats(vsa_engine* engine, vsa_thread_stats* out, uint32_t capacity,
                                                        uint32_t* out_count);

/**
 * Fills `out` with every setting resolved: what a zeroed config would actually run as, the
 * values that depend on the machine included (the reflection threads). `out->struct_size` must
 * be set. No engine is needed. A settings file written from this holds real numbers rather than
 * zeros, which is what a reader of it needs (ADR 0018); a zero in a config still means "the
 * default", so an older file keeps working.
 */
VSA_API vsa_result VSA_CALL vsa_get_default_config(vsa_engine_config* out);

/** One leg of a traced sound path (vsa_scene_trace_rays). */
typedef struct vsa_ray_segment {
    uint32_t struct_size;
    /** 0 for the leg leaving the origin. */
    uint32_t bounce;
    /** Scene coordinates. */
    float from[3];
    float to[3];
    /** Mid-band energy left on arrival (1 at the origin). */
    float energy;
    /** The material at `to`; 0 if the leg ended in the open. */
    uint32_t material;
} vsa_ray_segment;

/**
 * Debugging: follows `rays` sound paths from `origin` (scene coordinates) through up to `bounces`
 * reflections off the voxel world (specular, or diffuse by each material's scattering; losing
 * each surface's absorption), legs up to `max_distance` long. Deterministic. Fills up to
 * `capacity` segments (out[0].struct_size set) and sets *out_count to how many were filled.
 * Not Steam Audio's own rays, but the same surfaces: it shows where sound goes.
 */
VSA_API vsa_result VSA_CALL vsa_scene_trace_rays(vsa_engine* engine, const float origin[3], uint32_t rays,
                                                 uint32_t bounces, float max_distance, vsa_ray_segment* out,
                                                 uint32_t capacity, uint32_t* out_count);

/**
 * Message for the most recent failure on the calling thread, or "" if none.
 * The pointer stays valid until the next vsa_* call on the same thread.
 */
VSA_API const char* VSA_CALL vsa_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* VSAUDIO_H */
