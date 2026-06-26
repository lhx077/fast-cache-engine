#include "../internal/fce_internal.h"

FceStatus fce_schema_plan(const FcePlannerInput *input, FceSchema *out_schema) {
    if (!input || !out_schema) return FCE_ERR_INVALID_ARGUMENT;
    FceSchema s = fce_schema_default();
    s.lookup = input->lookup ? input->lookup : FCE_LOOKUP_EXACT;
    s.key_kind = input->key_kind ? input->key_kind : FCE_KEY_BYTES;
    s.fixed_key_size = 0;
    s.fixed_value_size = 0;

    if (input->needs_online_append) {
        s.backend = FCE_BACKEND_LOG;
        s.allow_duplicate_put = 1;
    } else if (input->prefers_prefix_scan || s.lookup == FCE_LOOKUP_PREFIX || s.key_kind == FCE_KEY_SEQUENCE) {
        s.backend = FCE_BACKEND_RADIX;
        s.lookup = FCE_LOOKUP_PREFIX;
    } else if (s.key_kind == FCE_KEY_U64 && input->dense_u64_max_key && input->estimated_record_count &&
               input->dense_u64_max_key + 1 <= input->estimated_record_count * 4) {
        s.backend = FCE_BACKEND_DIRECT_TABLE;
        s.fixed_key_size = 8;
    } else if (s.key_kind == FCE_KEY_U128) {
        s.fixed_key_size = 16;
        s.backend = input->allow_mph && input->read_mostly ? FCE_BACKEND_MPH : FCE_BACKEND_SORTED_INDEX;
    } else if (input->allow_mph && input->read_mostly && input->estimated_record_count >= 1024) {
        s.backend = FCE_BACKEND_MPH;
    } else {
        s.backend = FCE_BACKEND_SORTED_INDEX;
    }

    *out_schema = s;
    return validate_schema(out_schema);
}
