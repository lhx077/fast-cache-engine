#include "../internal/fce_internal.h"

#if defined(_WIN32)
typedef struct {
    void *view;
    HANDLE mapping;
    HANDLE file;
} MapCleanup;

static void map_cleanup(void *ctx) {
    MapCleanup *m = (MapCleanup *)ctx;
    if (m->view) UnmapViewOfFile(m->view);
    if (m->mapping) CloseHandle(m->mapping);
    if (m->file && m->file != INVALID_HANDLE_VALUE) CloseHandle(m->file);
    fce_free(m);
}
#else
typedef struct {
    void *view;
    size_t size;
} MapCleanup;

static void map_cleanup(void *ctx) {
    MapCleanup *m = (MapCleanup *)ctx;
    if (m->view && m->size) munmap(m->view, m->size);
    fce_free(m);
}
#endif


char *join_path_arena(FceArena *arena, const char *dir, const char *name) {
    size_t a = strlen(dir), b = strlen(name), s = strlen(FCE_SEP);
    char *p = (char *)fce_arena_alloc(arena, a + s + b + 1, 1);
    if (!p) return NULL;
    memcpy(p, dir, a);
    memcpy(p + a, FCE_SEP, s);
    memcpy(p + a + s, name, b + 1);
    return p;
}

char *join_path_heap(const char *dir, const char *name) {
    size_t a = strlen(dir), b = strlen(name), s = strlen(FCE_SEP);
    char *p = (char *)fce_xmalloc(a + s + b + 1);
    if (!p) return NULL;
    memcpy(p, dir, a);
    memcpy(p + a, FCE_SEP, s);
    memcpy(p + a + s, name, b + 1);
    return p;
}

FceStatus ensure_dir(const char *path) {
    if (!path || !*path) return FCE_ERR_INVALID_ARGUMENT;
    if (FCE_MKDIR(path) == 0) return FCE_OK;
    return errno == EEXIST ? FCE_OK : FCE_ERR_IO;
}

FceStatus write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return FCE_ERR_IO;
    if (len && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return FCE_ERR_IO;
    }
    return fclose(f) == 0 ? FCE_OK : FCE_ERR_IO;
}

FceStatus read_file_heap_arena(FceArena *arena, const char *path, FileBlob *out) {
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "rb");
    if (!f) return FCE_OK;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return FCE_ERR_IO;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return FCE_ERR_IO;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return FCE_ERR_IO;
    }
    out->size = (size_t)sz;
    out->data = (uint8_t *)fce_arena_alloc(arena, out->size ? out->size : 1, 1);
    if (!out->data) {
        fclose(f);
        return FCE_ERR_OUT_OF_MEMORY;
    }
    if (out->size && fread(out->data, 1, out->size, f) != out->size) {
        fclose(f);
        return FCE_ERR_IO;
    }
    fclose(f);
    return FCE_OK;
}

FceStatus read_file_arena(FceArena *arena, const char *path, FileBlob *out, int use_mmap) {
    if (!use_mmap) return read_file_heap_arena(arena, path, out);
    memset(out, 0, sizeof(*out));
#if defined(_WIN32)
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FCE_OK;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(file, &sz) || sz.QuadPart < 0 || (uint64_t)sz.QuadPart > SIZE_MAX) {
        CloseHandle(file);
        return FCE_ERR_IO;
    }
    out->size = (size_t)sz.QuadPart;
    if (!out->size) {
        CloseHandle(file);
        out->data = (uint8_t *)fce_arena_alloc(arena, 1, 1);
        return out->data ? FCE_OK : FCE_ERR_OUT_OF_MEMORY;
    }
    HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mapping) {
        CloseHandle(file);
        return FCE_ERR_IO;
    }
    void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mapping);
        CloseHandle(file);
        return FCE_ERR_IO;
    }
    MapCleanup *ctx = (MapCleanup *)fce_xmalloc(sizeof(*ctx));
    if (!ctx) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        CloseHandle(file);
        return FCE_ERR_OUT_OF_MEMORY;
    }
    ctx->view = view;
    ctx->mapping = mapping;
    ctx->file = file;
    FceStatus st = fce_arena_register_cleanup(arena, map_cleanup, ctx);
    if (st != FCE_OK) {
        map_cleanup(ctx);
        return st;
    }
    out->data = (uint8_t *)view;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return FCE_OK;
    struct stat stbuf;
    if (fstat(fd, &stbuf) != 0 || stbuf.st_size < 0 || (uint64_t)stbuf.st_size > SIZE_MAX) {
        close(fd);
        return FCE_ERR_IO;
    }
    out->size = (size_t)stbuf.st_size;
    if (!out->size) {
        close(fd);
        out->data = (uint8_t *)fce_arena_alloc(arena, 1, 1);
        return out->data ? FCE_OK : FCE_ERR_OUT_OF_MEMORY;
    }
    void *view = mmap(NULL, out->size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (view == MAP_FAILED) return FCE_ERR_IO;
    MapCleanup *ctx = (MapCleanup *)fce_xmalloc(sizeof(*ctx));
    if (!ctx) {
        munmap(view, out->size);
        return FCE_ERR_OUT_OF_MEMORY;
    }
    ctx->view = view;
    ctx->size = out->size;
    FceStatus st = fce_arena_register_cleanup(arena, map_cleanup, ctx);
    if (st != FCE_OK) {
        map_cleanup(ctx);
        return st;
    }
    out->data = (uint8_t *)view;
#endif
    return FCE_OK;
}

