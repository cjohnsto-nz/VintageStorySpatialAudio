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
#define VSA_ABI_VERSION 3u

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
    VSA_ENGINE_FLAG_STEAM_AUDIO_VALIDATION = 1u << 0
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
    /** VSA_ASSET_FORMAT_PCM_S16 only: 1 or 2. */
    uint32_t pcm_channels;
    /** VSA_ASSET_FORMAT_PCM_S16 only. */
    uint32_t pcm_sample_rate;
} vsa_asset_desc;

typedef struct vsa_asset_info {
    uint32_t struct_size;
    /** 1 or 2. */
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
    /** Not positioned: straight to its bus (music, UI). Mono is centred at -3 dB per side. */
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
} vsa_listener;

typedef enum vsa_render_mode {
    /** Binaural (Steam Audio HRTF) to the front left/right channels. The default. */
    VSA_RENDER_HEADPHONES = 0,
    /**
     * Amplitude panning to the output's speakers: stereo, quad, 5.1 or 7.1 for 2/4/6/8 channels
     * (channels are routed by speaker position, so the device's own channel order is respected).
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
    VSA_OUTPUT_DEVICE = 1
} vsa_output_kind;

typedef struct vsa_output_desc {
    uint32_t struct_size;
    /** A vsa_output_kind value. */
    uint32_t kind;
    /** DEVICE: the device to open, or NULL for the system default (and to follow it when it changes). */
    const vsa_device_id* device_id;
    /** 0 = the device's native layout (NONE: 2); otherwise 2, 4, 6 or 8. */
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

/**
 * Message for the most recent failure on the calling thread, or "" if none.
 * The pointer stays valid until the next vsa_* call on the same thread.
 */
VSA_API const char* VSA_CALL vsa_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* VSAUDIO_H */
