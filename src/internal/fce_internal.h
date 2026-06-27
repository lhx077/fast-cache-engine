#ifndef FCE_INTERNAL_H
#define FCE_INTERNAL_H

#include "fce_cache.h"
#include "fce_arena.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#define FCE_MKDIR(path) _mkdir(path)
#define FCE_SEP "\\"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>
#define FCE_MKDIR(path) mkdir((path), 0777)
#define FCE_SEP "/"
#endif

#define FCE_MAGIC "FCEv0001"
#define FCE_MANIFEST_FILE "manifest.fce"
#define FCE_INDEX_FILE "index.fce"
#define FCE_KEYS_FILE "keys.fce"
#define FCE_VALUES_FILE "values.fce"
#define FCE_LOG_FILE "log.fce"
#define FCE_LOCK_FILE "cache.lock"
#define FCE_ENDIAN_TAG 0x0102030405060708ULL
#define FCE_LOG_MAGIC 0x31474f4c454346ULL
#define FCE_RADIX_MAGIC 0x3158444152454346ULL
#define FCE_RADIX_NO_VALUE 0xffffffffu
#define FCE_MPH_MAGIC 0x3148504d454346ULL
#define FCE_META_FLAG_EXACT_KEY_CHECK (1ULL << 32)
#define FCE_META_FLAG_MMAP_READ (1ULL << 33)
#define FCE_META_FLAG_READ_ONLY_AFTER_FREEZE (1ULL << 34)
#define FCE_META_FLAG_ALLOW_DUPLICATE_PUT (1ULL << 35)

typedef struct {
    uint64_t lo;
    uint64_t hi;
} Hash128;

typedef struct {
    uint64_t hash_lo;
    uint64_t hash_hi;
    uint64_t key_offset;
    uint32_t key_len;
    uint32_t reserved0;
    uint64_t value_offset;
    uint32_t value_len;
    uint32_t flags;
} SortedEntry;

typedef struct {
    uint8_t present;
    uint8_t reserved[7];
    uint64_t key_u64;
    uint64_t key_offset;
    uint32_t key_len;
    uint32_t value_len;
    uint64_t value_offset;
} DirectSlot;

typedef struct {
    uint8_t present;
    uint8_t reserved[7];
    uint64_t hash_lo;
    uint64_t hash_hi;
    uint64_t key_offset;
    uint32_t key_len;
    uint32_t value_len;
    uint64_t value_offset;
} MphSlot;

typedef struct {
    uint64_t magic;
    uint64_t bucket_count;
    uint64_t slot_count;
    uint64_t buckets_offset;
    uint64_t slots_offset;
} MphHeader;

typedef struct {
    uint64_t seed;
} MphBucket;

typedef struct {
    uint32_t index;
    uint32_t count;
} MphBuildBucket;

typedef struct {
    uint64_t magic;
    uint64_t node_count;
    uint64_t edge_count;
    uint64_t value_count;
    uint64_t nodes_offset;
    uint64_t edges_offset;
    uint64_t values_offset;
} RadixHeader;

typedef struct {
    uint32_t first_edge;
    uint32_t edge_count;
    uint32_t value_index;
    uint32_t reserved0;
} RadixNode;

typedef struct {
    uint8_t label;
    uint8_t reserved0[3];
    uint32_t child_index;
} RadixEdge;

typedef struct {
    uint64_t key_offset;
    uint32_t key_len;
    uint32_t value_len;
    uint64_t value_offset;
} RadixValue;

typedef struct {
    uint8_t label;
    uint32_t child_index;
} RadixTmpEdge;

typedef struct {
    RadixTmpEdge *edges;
    size_t edge_count;
    size_t edge_cap;
    uint32_t value_index;
} RadixTmpNode;

typedef struct {
    uint8_t *key;
    size_t key_len;
    uint8_t *value;
    size_t value_len;
    uint64_t order;
} BuildRecord;

typedef struct {
    uint64_t hash_lo;
    uint64_t hash_hi;
    uint64_t key_offset;
    uint32_t key_len;
    uint32_t value_len;
    uint64_t value_offset;
    uint64_t order;
} LogIndexEntry;

struct FceBuilder {
    FceArena *arena;
    char *cache_dir;
    FceSchema schema;
    BuildRecord *records;
    size_t count;
    size_t cap;
    int frozen;
};

typedef struct {
    uint8_t *data;
    size_t size;
} FileBlob;

typedef struct {
#if defined(_WIN32)
    HANDLE handle;
#else
    int fd;
#endif
    int locked;
    int shared;
} FceFileLock;

struct FceReader {
    FceArena *arena;
    char *cache_dir;
    FceManifestInfo manifest;
    FceSchema schema;
    FileBlob index_blob;
    FileBlob keys_blob;
    FileBlob values_blob;
    FileBlob log_blob;
    LogIndexEntry *log_index;
    size_t log_index_count;
    FceFileLock cache_lock;
    int has_cache_lock;
};

struct FceIterator {
    FceArena *arena;
    FceReader *reader;
    size_t *matches;
    size_t count;
    size_t pos;
    FceBackendKind backend;
};

void *fce_xmalloc(size_t size) FCE_OWNERSHIP_RETURNS(malloc);
void *fce_xcalloc(size_t count, size_t size) FCE_OWNERSHIP_RETURNS(malloc);
void *fce_xrealloc(void *ptr, size_t size) FCE_OWNERSHIP_RETURNS(malloc);

uint64_t rd64(const void *p);
void wr64(void *p, uint64_t v);
uint64_t fnv1a64(const void *data, size_t len, uint64_t seed);
Hash128 hash_key(const FceSchema *schema, const void *data, size_t len);
int range_ok(uint64_t off, uint64_t len, uint64_t size);
int key_equal(const uint8_t *base, size_t size, uint64_t off, uint32_t len, const void *key, size_t key_len);
int key_bytes_cmp(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen);

uint64_t crc64_update(uint64_t crc, const void *data, size_t len);
uint64_t crc64_file(const char *path);

char *join_path_arena(FceArena *arena, const char *dir, const char *name);
char *join_path_heap(const char *dir, const char *name);
FceStatus ensure_dir(const char *path);
FceStatus write_file(const char *path, const void *data, size_t len);
FceStatus read_file_heap_arena(FceArena *arena, const char *path, FileBlob *out);
FceStatus read_file_arena(FceArena *arena, const char *path, FileBlob *out, int use_mmap);
FceStatus fce_cache_lock_acquire(const char *cache_dir, FceFileLock *out_lock);
FceStatus fce_cache_lock_acquire_shared(const char *cache_dir, FceFileLock *out_lock);
void fce_cache_lock_release(FceFileLock *lock);

FceStatus validate_schema(const FceSchema *s);
FceManifestInfo make_manifest(const FceSchema *schema, uint64_t records);
FceSchema schema_from_manifest(const FceManifestInfo *m);
FceStatus read_manifest(const char *cache_dir, FceManifestInfo *out);
FceStatus write_manifest(const char *cache_dir, FceManifestInfo *m);

int cmp_build_by_key(const void *a, const void *b);
int cmp_sorted_entry(const void *a, const void *b);
int cmp_log_index_entry(const void *a, const void *b);
FceStatus dedupe_records(FceBuilder *b, BuildRecord **out, size_t *out_count);
FceStatus write_keys_values(FceBuilder *b, BuildRecord *records, size_t count, SortedEntry *entries, uint64_t *keys_size, uint64_t *values_size);

FceStatus freeze_sorted_like(FceBuilder *b, FceBackendKind physical_backend);
FceStatus freeze_radix(FceBuilder *b);
FceStatus freeze_direct(FceBuilder *b);
FceStatus freeze_mph(FceBuilder *b);
FceStatus freeze_log(FceBuilder *b);

FceStatus get_sorted(FceReader *r, const void *key, size_t key_len, const void **out_value, size_t *out_value_len);
int radix_parts(FceReader *r, RadixHeader **out_h, RadixNode **out_nodes, RadixEdge **out_edges, RadixValue **out_values);
int radix_find_child(RadixNode *nodes, RadixEdge *edges, uint32_t node_index, uint8_t label, uint32_t *out_child);
FceStatus get_radix(FceReader *r, const void *key, size_t key_len, const void **out_value, size_t *out_value_len);
FceStatus get_direct(FceReader *r, uint64_t key, const void **out_value, size_t *out_value_len);
FceStatus get_mph(FceReader *r, const void *key, size_t key_len, const void **out_value, size_t *out_value_len);
FceStatus get_log(FceReader *r, const void *key, size_t key_len, const void **out_value, size_t *out_value_len);
FceStatus build_log_index(FceReader *r);

#endif
