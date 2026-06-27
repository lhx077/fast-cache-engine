#include "../internal/fce_internal.h"

uint32_t fce_abi_version(void) {
    return FCE_ABI_VERSION;
}

const char *fce_version_string(void) {
    return "1.1.0";
}
