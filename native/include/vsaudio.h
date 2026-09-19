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
#define VSA_ABI_VERSION 1u

typedef enum vsa_result {
    VSA_OK = 0,
    VSA_ERROR_INVALID_ARGUMENT = 1,
    VSA_ERROR_ABI_MISMATCH = 2,
    VSA_ERROR_ALREADY_EXISTS = 3,
    VSA_ERROR_STEAM_AUDIO = 4,
    VSA_ERROR_OUT_OF_MEMORY = 5,
    VSA_ERROR_UNSUPPORTED = 6,
    VSA_ERROR_INTERNAL = 7
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

/**
 * Message for the most recent failure on the calling thread, or "" if none.
 * The pointer stays valid until the next vsa_* call on the same thread.
 */
VSA_API const char* VSA_CALL vsa_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* VSAUDIO_H */
