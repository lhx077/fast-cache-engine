#include "../internal/fce_internal.h"

typedef struct FceAllocation {
    void *ptr;
    size_t size;
    struct FceAllocation *next;
} FceAllocation;

static FceMemoryStats g_mem_stats;
static FceAllocation *g_allocations;

typedef struct ArenaPtr {
    void *ptr;
    struct ArenaPtr *next;
} ArenaPtr;

typedef struct ArenaCleanup {
    void (*cleanup)(void *ctx);
    void *ctx;
    struct ArenaCleanup *next;
} ArenaCleanup;

struct FceArenaImpl {
    ArenaPtr *ptrs;
    ArenaCleanup *cleanups;
};

static FceAllocation *find_allocation(void *ptr, FceAllocation **out_prev) {
    FceAllocation *prev = NULL;
    FceAllocation *cur = g_allocations;
    while (cur) {
        if (cur->ptr == ptr) {
            if (out_prev) *out_prev = prev;
            return cur;
        }
        prev = cur;
        cur = cur->next;
    }
    if (out_prev) *out_prev = NULL;
    return NULL;
}

static void add_stats(size_t n) {
    g_mem_stats.active_allocations++;
    g_mem_stats.total_allocations++;
    g_mem_stats.active_bytes += n;
    if (g_mem_stats.active_bytes > g_mem_stats.peak_bytes) {
        g_mem_stats.peak_bytes = g_mem_stats.active_bytes;
    }
}

void *fce_xmalloc(size_t size) {
    size_t n = size ? size : 1;
    void *p = malloc(n);
    if (!p) return NULL;
    FceAllocation *node = (FceAllocation *)malloc(sizeof(*node));
    if (!node) {
        free(p);
        return NULL;
    }
    node->ptr = p;
    node->size = n;
    node->next = g_allocations;
    g_allocations = node;
    add_stats(n);
    return p;
}

void *fce_xcalloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    size_t total = count * size;
    void *p = fce_xmalloc(total);
    if (p && total) memset(p, 0, total);
    return p;
}

void *fce_xrealloc(void *ptr, size_t size) {
    if (!ptr) return fce_xmalloc(size);
    if (!size) {
        fce_free(ptr);
        return NULL;
    }
    FceAllocation *node = find_allocation(ptr, NULL);
    if (!node) return NULL;
    size_t old_size = node->size;
    void *p = realloc(ptr, size);
    if (!p) return NULL;
    node->ptr = p;
    node->size = size;
    if (size >= old_size) {
        g_mem_stats.active_bytes += (uint64_t)(size - old_size);
    } else {
        g_mem_stats.active_bytes -= (uint64_t)(old_size - size);
    }
    if (g_mem_stats.active_bytes > g_mem_stats.peak_bytes) {
        g_mem_stats.peak_bytes = g_mem_stats.active_bytes;
    }
    return p;
}

void fce_free(void *ptr) {
    if (!ptr) return;
    FceAllocation *prev = NULL;
    FceAllocation *node = find_allocation(ptr, &prev);
    if (node) {
        size_t size = node->size;
        if (prev) prev->next = node->next;
        else g_allocations = node->next;
        free(node);
        if (g_mem_stats.active_allocations) g_mem_stats.active_allocations--;
        if (g_mem_stats.active_bytes >= size) g_mem_stats.active_bytes -= size;
        g_mem_stats.total_frees++;
    }
    free(ptr);
}

FceStatus fce_memory_stats(FceMemoryStats *out_stats) {
    if (!out_stats) return FCE_ERR_INVALID_ARGUMENT;
    *out_stats = g_mem_stats;
    return FCE_OK;
}

FceStatus fce_arena_create(FceArena **out_arena) {
    if (!out_arena) return FCE_ERR_INVALID_ARGUMENT;
    *out_arena = (FceArena *)fce_xcalloc(1, sizeof(struct FceArenaImpl));
    return *out_arena ? FCE_OK : FCE_ERR_OUT_OF_MEMORY;
}

void *fce_arena_alloc(FceArena *arena, size_t size, size_t align) {
    (void)align;
    if (!arena) return NULL;
    struct FceArenaImpl *a = (struct FceArenaImpl *)arena;
    void *p = fce_xmalloc(size);
    if (!p) return NULL;
    ArenaPtr *n = (ArenaPtr *)fce_xmalloc(sizeof(*n));
    if (!n) {
        fce_free(p);
        return NULL;
    }
    n->ptr = p;
    n->next = a->ptrs;
    a->ptrs = n;
    return p;
}

void *fce_arena_memdup(FceArena *arena, const void *data, size_t len) {
    if (!data && len) return NULL;
    void *p = fce_arena_alloc(arena, len ? len : 1, 1);
    if (p && len) memcpy(p, data, len);
    return p;
}

FceStatus fce_arena_register_cleanup(FceArena *arena, void (*cleanup)(void *ctx), void *ctx) {
    if (!arena || !cleanup) return FCE_ERR_INVALID_ARGUMENT;
    struct FceArenaImpl *a = (struct FceArenaImpl *)arena;
    ArenaCleanup *n = (ArenaCleanup *)fce_xmalloc(sizeof(*n));
    if (!n) return FCE_ERR_OUT_OF_MEMORY;
    n->cleanup = cleanup;
    n->ctx = ctx;
    n->next = a->cleanups;
    a->cleanups = n;
    return FCE_OK;
}

void fce_arena_destroy(FceArena *arena) {
    if (!arena) return;
    struct FceArenaImpl *a = (struct FceArenaImpl *)arena;
    ArenaCleanup *c = a->cleanups;
    while (c) {
        ArenaCleanup *next = c->next;
        c->cleanup(c->ctx);
        fce_free(c);
        c = next;
    }
    ArenaPtr *p = a->ptrs;
    while (p) {
        ArenaPtr *next = p->next;
        fce_free(p->ptr);
        fce_free(p);
        p = next;
    }
    fce_free(arena);
}
