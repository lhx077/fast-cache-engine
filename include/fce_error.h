#ifndef FCE_ERROR_H
#define FCE_ERROR_H

#include "fce_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

FCE_API const char *fce_status_string(FceStatus status);
FCE_API FceStatus fce_memory_stats(FceMemoryStats *out_stats);
FCE_API void fce_free(void *ptr) FCE_OWNERSHIP_TAKES(malloc, 1);

#ifdef __cplusplus
}
#endif

#endif
