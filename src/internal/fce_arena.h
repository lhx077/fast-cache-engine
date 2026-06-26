#ifndef FCE_ARENA_H
#define FCE_ARENA_H

#include "fce_cache.h"

typedef struct FceArena FceArena;

FceStatus fce_arena_create(FceArena **out_arena);
void *fce_arena_alloc(FceArena *arena, size_t size, size_t align);
void *fce_arena_memdup(FceArena *arena, const void *data, size_t len);
FceStatus fce_arena_register_cleanup(
    FceArena *arena,
    void (*cleanup)(void *ctx),
    void *ctx);
void fce_arena_destroy(FceArena *arena);

#endif
