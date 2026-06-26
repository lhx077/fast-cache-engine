#include "../internal/fce_internal.h"

#if defined(FCE_HAVE_ZSTD)
#include <zstd.h>
#endif

static FceStatus codec_copy(const void *input, size_t input_len, void **out_data, size_t *out_len) {
    void *p = fce_xmalloc(input_len ? input_len : 1);
    if (!p) return FCE_ERR_OUT_OF_MEMORY;
    if (input_len) memcpy(p, input, input_len);
    *out_data = p;
    *out_len = input_len;
    return FCE_OK;
}

FceStatus fce_codec_encode(FceCodecKind codec, const void *input, size_t input_len, void **out_data, size_t *out_len) {
    if ((!input && input_len) || !out_data || !out_len) return FCE_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_len = 0;
    if (codec == FCE_CODEC_NONE || codec == FCE_CODEC_USER_BYTES) return codec_copy(input, input_len, out_data, out_len);
    if (codec == FCE_CODEC_DELTA_I32) {
        if (input_len % 4 != 0) return FCE_ERR_INVALID_ARGUMENT;
        size_t n = input_len / 4;
        const int32_t *in = (const int32_t *)input;
        int32_t *out = (int32_t *)fce_xmalloc(input_len ? input_len : 1);
        if (!out) return FCE_ERR_OUT_OF_MEMORY;
        if (n) {
            out[0] = in[0];
            for (size_t i = 1; i < n; i++) {
                int64_t d = (int64_t)in[i] - (int64_t)in[i - 1];
                if (d < INT32_MIN || d > INT32_MAX) {
                    fce_free(out);
                    return FCE_ERR_INVALID_ARGUMENT;
                }
                out[i] = (int32_t)d;
            }
        }
        *out_data = out;
        *out_len = input_len;
        return FCE_OK;
    }
    if (codec == FCE_CODEC_RLE) {
        if (input_len % 4 != 0) return FCE_ERR_INVALID_ARGUMENT;
        size_t n = input_len / 4;
        const int32_t *in = (const int32_t *)input;
        int32_t *tmp = (int32_t *)fce_xmalloc((2 + n * 2) * sizeof(int32_t));
        if (!tmp) return FCE_ERR_OUT_OF_MEMORY;
        tmp[0] = (int32_t)n;
        size_t w = 1;
        for (size_t i = 0; i < n;) {
            int32_t v = in[i];
            size_t j = i + 1;
            while (j < n && in[j] == v && j - i < INT32_MAX) j++;
            tmp[w++] = v;
            tmp[w++] = (int32_t)(j - i);
            i = j;
        }
        *out_data = tmp;
        *out_len = w * sizeof(int32_t);
        return FCE_OK;
    }
    if (codec == FCE_CODEC_BITPACK) {
        if (input_len % 4 != 0) return FCE_ERR_INVALID_ARGUMENT;
        size_t n = input_len / 4;
        const uint32_t *in = (const uint32_t *)input;
        uint32_t maxv = 0;
        for (size_t i = 0; i < n; i++) if (in[i] > maxv) maxv = in[i];
        uint32_t bits = 0;
        while ((bits < 32) && (maxv >> bits)) bits++;
        if (!bits) bits = 1;
        size_t payload_bits = n * (size_t)bits;
        size_t payload = (payload_bits + 7) / 8;
        uint8_t *out = (uint8_t *)fce_xcalloc(1, 16 + payload);
        if (!out) return FCE_ERR_OUT_OF_MEMORY;
        wr64(out, (uint64_t)n);
        wr64(out + 8, (uint64_t)bits);
        size_t bitpos = 0;
        for (size_t i = 0; i < n; i++) {
            uint32_t v = in[i];
            for (uint32_t b = 0; b < bits; b++, bitpos++) if ((v >> b) & 1U) out[16 + bitpos / 8] |= (uint8_t)(1U << (bitpos % 8));
        }
        *out_data = out;
        *out_len = 16 + payload;
        return FCE_OK;
    }
#if defined(FCE_HAVE_ZSTD)
    if (codec == FCE_CODEC_ZSTD) {
        size_t bound = ZSTD_compressBound(input_len);
        void *out = fce_xmalloc(bound ? bound : 1);
        if (!out) return FCE_ERR_OUT_OF_MEMORY;
        size_t n = ZSTD_compress(out, bound, input, input_len, 1);
        if (ZSTD_isError(n)) {
            fce_free(out);
            return FCE_ERR_INVALID_ARGUMENT;
        }
        *out_data = out;
        *out_len = n;
        return FCE_OK;
    }
#else
    if (codec == FCE_CODEC_ZSTD) return FCE_ERR_UNSUPPORTED;
#endif
    return FCE_ERR_UNSUPPORTED;
}

FceStatus fce_codec_decode(FceCodecKind codec, const void *input, size_t input_len, void **out_data, size_t *out_len) {
    if ((!input && input_len) || !out_data || !out_len) return FCE_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_len = 0;
    if (codec == FCE_CODEC_NONE || codec == FCE_CODEC_USER_BYTES) return codec_copy(input, input_len, out_data, out_len);
    if (codec == FCE_CODEC_DELTA_I32) {
        if (input_len % 4 != 0) return FCE_ERR_CORRUPT;
        size_t n = input_len / 4;
        const int32_t *in = (const int32_t *)input;
        int32_t *out = (int32_t *)fce_xmalloc(input_len ? input_len : 1);
        if (!out) return FCE_ERR_OUT_OF_MEMORY;
        if (n) {
            out[0] = in[0];
            for (size_t i = 1; i < n; i++) {
                int64_t v = (int64_t)out[i - 1] + (int64_t)in[i];
                if (v < INT32_MIN || v > INT32_MAX) {
                    fce_free(out);
                    return FCE_ERR_CORRUPT;
                }
                out[i] = (int32_t)v;
            }
        }
        *out_data = out;
        *out_len = input_len;
        return FCE_OK;
    }
    if (codec == FCE_CODEC_RLE) {
        if (input_len < 4 || input_len % 4 != 0) return FCE_ERR_CORRUPT;
        const int32_t *in = (const int32_t *)input;
        size_t pairs = input_len / 8;
        size_t n = (size_t)in[0];
        int32_t *out = (int32_t *)fce_xmalloc(n * sizeof(int32_t));
        if (!out) return FCE_ERR_OUT_OF_MEMORY;
        size_t w = 0;
        for (size_t i = 0; i < pairs; i++) {
            int32_t v = in[1 + i * 2];
            int32_t c = in[2 + i * 2];
            if (c < 0 || w + (size_t)c > n) {
                fce_free(out);
                return FCE_ERR_CORRUPT;
            }
            for (int32_t j = 0; j < c; j++) out[w++] = v;
        }
        if (w != n) {
            fce_free(out);
            return FCE_ERR_CORRUPT;
        }
        *out_data = out;
        *out_len = n * sizeof(int32_t);
        return FCE_OK;
    }
    if (codec == FCE_CODEC_BITPACK) {
        if (input_len < 16) return FCE_ERR_CORRUPT;
        size_t n = (size_t)rd64(input);
        uint64_t bits64 = rd64((const uint8_t *)input + 8);
        if (bits64 == 0 || bits64 > 32) return FCE_ERR_CORRUPT;
        uint32_t bits = (uint32_t)bits64;
        size_t payload = (n * (size_t)bits + 7) / 8;
        if (16 + payload != input_len) return FCE_ERR_CORRUPT;
        uint32_t *out = (uint32_t *)fce_xcalloc(n ? n : 1, sizeof(uint32_t));
        if (!out) return FCE_ERR_OUT_OF_MEMORY;
        const uint8_t *p = (const uint8_t *)input + 16;
        size_t bitpos = 0;
        for (size_t i = 0; i < n; i++) {
            uint32_t v = 0;
            for (uint32_t b = 0; b < bits; b++, bitpos++) if ((p[bitpos / 8] >> (bitpos % 8)) & 1U) v |= 1U << b;
            out[i] = v;
        }
        *out_data = out;
        *out_len = n * sizeof(uint32_t);
        return FCE_OK;
    }
#if defined(FCE_HAVE_ZSTD)
    if (codec == FCE_CODEC_ZSTD) {
        unsigned long long content_size = ZSTD_getFrameContentSize(input, input_len);
        if (content_size == ZSTD_CONTENTSIZE_ERROR || content_size == ZSTD_CONTENTSIZE_UNKNOWN ||
            content_size > (unsigned long long)SIZE_MAX) {
            return FCE_ERR_CORRUPT;
        }
        void *out = fce_xmalloc((size_t)content_size ? (size_t)content_size : 1);
        if (!out) return FCE_ERR_OUT_OF_MEMORY;
        size_t n = ZSTD_decompress(out, (size_t)content_size, input, input_len);
        if (ZSTD_isError(n) || n != (size_t)content_size) {
            fce_free(out);
            return FCE_ERR_CORRUPT;
        }
        *out_data = out;
        *out_len = n;
        return FCE_OK;
    }
#else
    if (codec == FCE_CODEC_ZSTD) return FCE_ERR_UNSUPPORTED;
#endif
    return FCE_ERR_UNSUPPORTED;
}

int fce_codec_available(FceCodecKind codec) {
    switch (codec) {
        case FCE_CODEC_NONE:
        case FCE_CODEC_DELTA_I32:
        case FCE_CODEC_RLE:
        case FCE_CODEC_BITPACK:
        case FCE_CODEC_USER_BYTES:
            return 1;
        case FCE_CODEC_ZSTD:
#if defined(FCE_HAVE_ZSTD)
            return 1;
#else
            return 0;
#endif
        default:
            return 0;
    }
}
