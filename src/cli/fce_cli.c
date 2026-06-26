#include "fce_cache.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    puts("fce commands:");
    puts("  fce build --schema <schema.json> --input <records.fce> --output <cache_dir>");
    puts("  fce inspect <cache_dir>");
    puts("  fce validate <cache_dir>");
    puts("  fce dump <cache_dir> [limit]");
    puts("  fce compact <cache_dir> --backend <sorted_index|direct_table|mph|radix> --output <output_cache_dir>");
    puts("  fce compact <cache_dir> <sorted_index|direct_table|mph|radix> <output_cache_dir>");
}

static int read_text_file(const char *path, char **out) {
    FILE *f = fopen(path, "rb");
    long n;
    char *p;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    p = (char *)malloc((size_t)n + 1);
    if (!p) {
        fclose(f);
        return 0;
    }
    if ((size_t)n && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        fclose(f);
        return 0;
    }
    p[n] = 0;
    fclose(f);
    *out = p;
    return 1;
}

static const char *skip_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) p++;
    return p;
}

static const char *json_find_key(const char *json, const char *key) {
    size_t key_len = strlen(key);
    const char *p = json;
    while (p && *p) {
        p = strchr(p, '"');
        if (!p) return NULL;
        p++;
        const char *start = p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p++;
            p++;
        }
        if (!*p) return NULL;
        if ((size_t)(p - start) == key_len && memcmp(start, key, key_len) == 0) {
            const char *q = skip_ws(p + 1);
            if (*q == ':') return skip_ws(q + 1);
        }
        p++;
    }
    return NULL;
}

static int json_get_string(const char *json, const char *key, char *out, size_t out_cap) {
    const char *p = json_find_key(json, key);
    size_t n = 0;
    if (!p) return 0;
    if (*p != '"') return -1;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\') return -1;
        if (n + 1 >= out_cap) return -1;
        out[n++] = *p++;
    }
    if (*p != '"') return -1;
    out[n] = 0;
    return 1;
}

static int json_get_bool(const char *json, const char *key, int *out) {
    const char *p = json_find_key(json, key);
    if (!p) return 0;
    if (strncmp(p, "true", 4) == 0) {
        *out = 1;
        return 1;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = 0;
        return 1;
    }
    return -1;
}

static int json_get_u32(const char *json, const char *key, uint32_t *out) {
    const char *p = json_find_key(json, key);
    char *end = NULL;
    unsigned long v;
    if (!p) return 0;
    if (!isdigit((unsigned char)*p)) return -1;
    v = strtoul(p, &end, 10);
    if (end == p || v > UINT32_MAX) return -1;
    *out = (uint32_t)v;
    return 1;
}

static int parse_codec_name(const char *value, FceCodecKind *out) {
    if (strcmp(value, "none") == 0) *out = FCE_CODEC_NONE;
    else if (strcmp(value, "delta_i32") == 0) *out = FCE_CODEC_DELTA_I32;
    else if (strcmp(value, "rle") == 0) *out = FCE_CODEC_RLE;
    else if (strcmp(value, "bitpack") == 0) *out = FCE_CODEC_BITPACK;
    else if (strcmp(value, "user_bytes") == 0) *out = FCE_CODEC_USER_BYTES;
    else if (strcmp(value, "zstd") == 0) *out = FCE_CODEC_ZSTD;
    else return 0;
    return 1;
}

static FceStatus parse_schema_file(const char *path, FceSchema *out_schema) {
    FceSchema s = fce_schema_default();
    char *json = NULL;
    char value[64];
    int got;
    int b;
    if (!out_schema) return FCE_ERR_INVALID_ARGUMENT;
    if (!read_text_file(path, &json)) return FCE_ERR_IO;

    got = json_get_string(json, "backend", value, sizeof(value));
    if (got < 0) goto invalid;
    if (got > 0) {
        if (strcmp(value, "sorted_index") == 0) s.backend = FCE_BACKEND_SORTED_INDEX;
        else if (strcmp(value, "direct_table") == 0) {
            s.backend = FCE_BACKEND_DIRECT_TABLE;
            s.key_kind = FCE_KEY_U64;
        } else if (strcmp(value, "mph") == 0) s.backend = FCE_BACKEND_MPH;
        else if (strcmp(value, "radix") == 0) {
            s.backend = FCE_BACKEND_RADIX;
            s.lookup = FCE_LOOKUP_PREFIX;
        } else if (strcmp(value, "log") == 0) s.backend = FCE_BACKEND_LOG;
        else goto invalid;
    }

    got = json_get_string(json, "lookup", value, sizeof(value));
    if (got < 0) goto invalid;
    if (got > 0) {
        if (strcmp(value, "exact") == 0) s.lookup = FCE_LOOKUP_EXACT;
        else if (strcmp(value, "prefix") == 0) s.lookup = FCE_LOOKUP_PREFIX;
        else if (strcmp(value, "range") == 0) s.lookup = FCE_LOOKUP_RANGE;
        else goto invalid;
    }

    got = json_get_string(json, "key_kind", value, sizeof(value));
    if (got < 0) goto invalid;
    if (got > 0) {
        if (strcmp(value, "bytes") == 0) s.key_kind = FCE_KEY_BYTES;
        else if (strcmp(value, "u64") == 0) s.key_kind = FCE_KEY_U64;
        else if (strcmp(value, "u128") == 0) s.key_kind = FCE_KEY_U128;
        else if (strcmp(value, "int_tuple") == 0) s.key_kind = FCE_KEY_INT_TUPLE;
        else if (strcmp(value, "sequence") == 0) s.key_kind = FCE_KEY_SEQUENCE;
        else goto invalid;
    }

    got = json_get_string(json, "value_kind", value, sizeof(value));
    if (got < 0) goto invalid;
    if (got > 0) {
        if (strcmp(value, "bytes") == 0) s.value_kind = FCE_VALUE_BYTES;
        else if (strcmp(value, "fixed_record") == 0) s.value_kind = FCE_VALUE_FIXED_RECORD;
        else if (strcmp(value, "var_record") == 0) s.value_kind = FCE_VALUE_VAR_RECORD;
        else goto invalid;
    }

    got = json_get_string(json, "key_codec", value, sizeof(value));
    if (got < 0) goto invalid;
    if (got > 0 && !parse_codec_name(value, &s.key_codec)) goto invalid;

    got = json_get_string(json, "value_codec", value, sizeof(value));
    if (got < 0) goto invalid;
    if (got > 0 && !parse_codec_name(value, &s.value_codec)) goto invalid;

    got = json_get_u32(json, "fixed_key_size", &s.fixed_key_size);
    if (got < 0) goto invalid;

    got = json_get_u32(json, "fixed_value_size", &s.fixed_value_size);
    if (got < 0) goto invalid;

    got = json_get_u32(json, "direct_table_key_bits", &s.direct_table_key_bits);
    if (got < 0) goto invalid;

    got = json_get_bool(json, "allow_duplicate_put", &b);
    if (got < 0) goto invalid;
    if (got > 0) s.allow_duplicate_put = (uint8_t)b;

    got = json_get_bool(json, "mmap_read", &b);
    if (got < 0) goto invalid;
    if (got > 0) s.mmap_read = (uint8_t)b;

    got = json_get_bool(json, "exact_key_check", &b);
    if (got < 0) goto invalid;
    if (got > 0) s.exact_key_check = (uint8_t)b;

    got = json_get_bool(json, "read_only_after_freeze", &b);
    if (got < 0) goto invalid;
    if (got > 0) s.read_only_after_freeze = (uint8_t)b;

    got = json_get_bool(json, "allow_sparse_direct_table", &b);
    if (got < 0) goto invalid;
    if (got > 0 && b) s.user_flags |= FCE_FLAG_ALLOW_SPARSE_DIRECT_TABLE;

    free(json);
    *out_schema = s;
    return FCE_OK;

invalid:
    free(json);
    return FCE_ERR_INVALID_ARGUMENT;
}

static int build_from_records(const char *schema_path, const char *input_path, const char *output_dir) {
    FceSchema s;
    FceBuilder *b = NULL;
    FceStatus st = parse_schema_file(schema_path, &s);
    FILE *f;
    if (st != FCE_OK) {
        fprintf(stderr, "schema parse failed: %s\n", fce_status_string(st));
        return 1;
    }
    st = fce_builder_open(output_dir, &s, &b);
    if (st != FCE_OK) {
        fprintf(stderr, "builder open failed: %s\n", fce_status_string(st));
        return 1;
    }
    f = fopen(input_path, "rb");
    if (!f) {
        fce_builder_close(b);
        fprintf(stderr, "cannot open input\n");
        return 1;
    }
    for (;;) {
        uint64_t kl, vl;
        unsigned char *k, *v;
        size_t got = fread(&kl, 1, sizeof(kl), f);
        if (got == 0) break;
        if (got != sizeof(kl)) {
            st = FCE_ERR_CORRUPT;
            break;
        }
        if (fread(&vl, 1, sizeof(vl), f) != sizeof(vl)) {
            st = FCE_ERR_CORRUPT;
            break;
        }
        if (kl > (uint64_t)SIZE_MAX || vl > (uint64_t)SIZE_MAX) {
            st = FCE_ERR_INVALID_ARGUMENT;
            break;
        }
        k = (unsigned char *)malloc((size_t)kl ? (size_t)kl : 1);
        v = (unsigned char *)malloc((size_t)vl ? (size_t)vl : 1);
        if (!k || !v) {
            free(k);
            free(v);
            st = FCE_ERR_OUT_OF_MEMORY;
            break;
        }
        if ((kl && fread(k, 1, (size_t)kl, f) != (size_t)kl) ||
            (vl && fread(v, 1, (size_t)vl, f) != (size_t)vl)) {
            free(k);
            free(v);
            st = FCE_ERR_CORRUPT;
            break;
        }
        st = fce_builder_put(b, k, (size_t)kl, v, (size_t)vl);
        free(k);
        free(v);
        if (st != FCE_OK) break;
    }
    fclose(f);
    if (st == FCE_OK) st = fce_builder_freeze(b);
    fce_builder_close(b);
    if (st != FCE_OK) {
        fprintf(stderr, "build failed: %s\n", fce_status_string(st));
        return 1;
    }
    puts("ok");
    return 0;
}

static FceBackendKind parse_backend(const char *s) {
    if (strcmp(s, "sorted_index") == 0) return FCE_BACKEND_SORTED_INDEX;
    if (strcmp(s, "direct_table") == 0) return FCE_BACKEND_DIRECT_TABLE;
    if (strcmp(s, "mph") == 0) return FCE_BACKEND_MPH;
    if (strcmp(s, "radix") == 0) return FCE_BACKEND_RADIX;
    if (strcmp(s, "log") == 0) return FCE_BACKEND_LOG;
    return 0;
}

static const char *backend_name(uint32_t backend) {
    switch (backend) {
        case FCE_BACKEND_SORTED_INDEX: return "sorted_index";
        case FCE_BACKEND_MPH: return "mph";
        case FCE_BACKEND_DIRECT_TABLE: return "direct_table";
        case FCE_BACKEND_RADIX: return "radix";
        case FCE_BACKEND_LOG: return "log";
        default: return "unknown";
    }
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
        usage();
        return 0;
    }
    if (strcmp(argv[1], "build") == 0) {
        const char *schema = NULL, *input = NULL, *output = NULL;
        for (int i = 2; i + 1 < argc; i += 2) {
            if (strcmp(argv[i], "--schema") == 0) schema = argv[i + 1];
            else if (strcmp(argv[i], "--input") == 0) input = argv[i + 1];
            else if (strcmp(argv[i], "--output") == 0) output = argv[i + 1];
            else break;
        }
        if (!schema || !input || !output) {
            usage();
            return 1;
        }
        return build_from_records(schema, input, output);
    }
    if (strcmp(argv[1], "inspect") == 0 && argc >= 3) {
        FceManifestInfo m;
        FceStatus st = fce_inspect(argv[2], &m);
        if (st != FCE_OK) {
            fprintf(stderr, "inspect failed: %s\n", fce_status_string(st));
            return 1;
        }
        double avg_key = m.record_count ? (double)m.keys_size / (double)m.record_count : 0.0;
        double avg_value = m.record_count ? (double)m.values_size / (double)m.record_count : 0.0;
        printf("backend=%s\nbackend_kind=%u\nrecord_count=%llu\nindex_size=%llu\nkeys_size=%llu\nvalues_size=%llu\nlog_size=%llu\navg_key_len=%.3f\navg_value_len=%.3f\nschema_hash=%llu\nindex_crc64=%llu\nkeys_crc64=%llu\nvalues_crc64=%llu\nlog_crc64=%llu\n",
            backend_name(m.backend_kind), m.backend_kind, (unsigned long long)m.record_count, (unsigned long long)m.index_size,
            (unsigned long long)m.keys_size, (unsigned long long)m.values_size,
            (unsigned long long)m.log_size, avg_key, avg_value, (unsigned long long)m.schema_hash,
            (unsigned long long)m.index_crc64, (unsigned long long)m.keys_crc64,
            (unsigned long long)m.values_crc64, (unsigned long long)m.log_crc64);
        return 0;
    }
    if (strcmp(argv[1], "validate") == 0 && argc >= 3) {
        FceStatus st = fce_validate(argv[2], NULL);
        if (st != FCE_OK) {
            fprintf(stderr, "validate failed: %s\n", fce_status_string(st));
            return 1;
        }
        puts("ok");
        return 0;
    }
    if (strcmp(argv[1], "dump") == 0 && argc >= 3) {
        size_t limit = 10;
        if (argc >= 5 && strcmp(argv[3], "--limit") == 0) {
            limit = (size_t)strtoull(argv[4], NULL, 10);
        } else if (argc >= 4) {
            limit = (size_t)strtoull(argv[3], NULL, 10);
        }
        FceReader *r = NULL;
        FceStatus st = fce_reader_open(argv[2], &r);
        if (st != FCE_OK) {
            fprintf(stderr, "open failed: %s\n", fce_status_string(st));
            return 1;
        }
        FceIterator *it = NULL;
        st = fce_reader_scan_all(r, &it);
        if (st != FCE_OK) {
            fprintf(stderr, "dump unsupported for this backend: %s\n", fce_status_string(st));
            fce_reader_close(r);
            return 1;
        }
        for (size_t i = 0; i < limit; i++) {
            const void *k, *v;
            size_t kl, vl;
            st = fce_iterator_next(it, &k, &kl, &v, &vl);
            if (st == FCE_ERR_NOT_FOUND) break;
            if (st != FCE_OK) {
                fprintf(stderr, "dump failed: %s\n", fce_status_string(st));
                break;
            }
            printf("%zu key_len=%zu value_len=%zu\n", i, kl, vl);
        }
        fce_iterator_close(it);
        fce_reader_close(r);
        return st == FCE_OK || st == FCE_ERR_NOT_FOUND ? 0 : 1;
    }
    if (strcmp(argv[1], "compact") == 0 && argc >= 5) {
        const char *backend_arg = NULL;
        const char *output_arg = NULL;
        if (argc == 5 && argv[3][0] != '-') {
            backend_arg = argv[3];
            output_arg = argv[4];
        } else {
            for (int i = 3; i + 1 < argc; i += 2) {
                if (strcmp(argv[i], "--backend") == 0) backend_arg = argv[i + 1];
                else if (strcmp(argv[i], "--output") == 0) output_arg = argv[i + 1];
                else {
                    usage();
                    return 1;
                }
            }
        }
        if (!backend_arg || !output_arg) {
            usage();
            return 1;
        }
        FceBackendKind b = parse_backend(backend_arg);
        if (!b) {
            fprintf(stderr, "unknown backend\n");
            return 1;
        }
        FceStatus st = fce_compact(argv[2], b, output_arg);
        if (st != FCE_OK) {
            fprintf(stderr, "compact failed: %s\n", fce_status_string(st));
            return 1;
        }
        puts("ok");
        return 0;
    }
    usage();
    return 1;
}
