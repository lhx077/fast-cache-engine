#ifndef FCE_SCHEMA_H
#define FCE_SCHEMA_H

#include "fce_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

FCE_API uint32_t fce_abi_version(void);
FCE_API const char *fce_version_string(void);
FCE_API FceSchema fce_schema_default(void);
FCE_API FceStatus fce_schema_plan(const FcePlannerInput *input, FceSchema *out_schema);
FCE_API uint64_t fce_schema_hash(const FceSchema *schema);

#ifdef __cplusplus
}
#endif

#endif
