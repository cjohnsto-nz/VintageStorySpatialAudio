/* Compiled as C to prove vsaudio.h is a valid C header (the ABI contract is C, not C++). */
#include "vsaudio.h"

#include <stddef.h>

/* Layout facts the managed bindings rely on (checked on every platform we build). */
typedef char vsa_assert_config_first_field[offsetof(vsa_engine_config, struct_size) == 0 ? 1 : -1];
typedef char vsa_assert_report_first_field[offsetof(vsa_self_test_report, struct_size) == 0 ? 1 : -1];

int vsa_header_c_compat_marker(void) {
    vsa_engine_config config = {0};
    config.struct_size = (uint32_t)sizeof config;
    config.abi_version = VSA_ABI_VERSION;
    config.ray_tracer = (uint32_t)VSA_RAY_TRACER_AUTO;
    return (int)config.struct_size;
}
