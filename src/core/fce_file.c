#include "../internal/fce_internal.h"

#if !defined(_WIN32)
#include <pthread.h>
#endif

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

#if defined(_WIN32)
static SRWLOCK g_cache_lock_mutex = SRWLOCK_INIT;
#else
static pthread_rwlock_t g_cache_lock_mutex = PTHREAD_RWLOCK_INITIALIZER;
#endif

static FceStatus process_lock_enter(int shared) {
#if defined(_WIN32)
    if (shared) AcquireSRWLockShared(&g_cache_lock_mutex);
    else AcquireSRWLockExclusive(&g_cache_lock_mutex);
    return FCE_OK;
#else
    int rc = shared ? pthread_rwlock_rdlock(&g_cache_lock_mutex) : pthread_rwlock_wrlock(&g_cache_lock_mutex);
    return rc == 0 ? FCE_OK : FCE_ERR_IO;
#endif
}

static void process_lock_leave(int shared) {
#if defined(_WIN32)
    if (shared) ReleaseSRWLockShared(&g_cache_lock_mutex);
    else ReleaseSRWLockExclusive(&g_cache_lock_mutex);
#else
    (void)shared;
    pthread_rwlock_unlock(&g_cache_lock_mutex);
#endif
}

static FceStatus cache_lock_acquire_impl(const char *cache_dir, FceFileLock *out_lock, int shared) {
    if (!cache_dir || !out_lock) return FCE_ERR_INVALID_ARGUMENT;
    memset(out_lock, 0, sizeof(*out_lock));
#if defined(_WIN32)
    out_lock->handle = INVALID_HANDLE_VALUE;
#else
    out_lock->fd = -1;
#endif
    out_lock->shared = shared ? 1 : 0;
    FceStatus st = process_lock_enter(out_lock->shared);
    if (st != FCE_OK) return st;

    char *path = join_path_heap(cache_dir, FCE_LOCK_FILE);
    if (!path) {
        process_lock_leave(out_lock->shared);
        return FCE_ERR_OUT_OF_MEMORY;
    }

#if defined(_WIN32)
    HANDLE handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    fce_free(path);
    if (handle == INVALID_HANDLE_VALUE) {
        process_lock_leave(out_lock->shared);
        return FCE_ERR_IO;
    }
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    DWORD flags = shared ? 0 : LOCKFILE_EXCLUSIVE_LOCK;
    if (!LockFileEx(handle, flags, 0, MAXDWORD, MAXDWORD, &ov)) {
        CloseHandle(handle);
        process_lock_leave(out_lock->shared);
        return FCE_ERR_IO;
    }
    out_lock->handle = handle;
    out_lock->locked = 1;
    return FCE_OK;
#else
    int fd;
    do {
        fd = open(path, O_RDWR | O_CREAT, 0666);
    } while (fd < 0 && errno == EINTR);
    fce_free(path);
    if (fd < 0) {
        process_lock_leave(out_lock->shared);
        return FCE_ERR_IO;
    }

    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = shared ? F_RDLCK : F_WRLCK;
    lock.l_whence = SEEK_SET;
    int rc;
    do {
        rc = fcntl(fd, F_SETLKW, &lock);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        close(fd);
        process_lock_leave(out_lock->shared);
        return FCE_ERR_IO;
    }
    out_lock->fd = fd;
    out_lock->locked = 1;
    return FCE_OK;
#endif
}

FceStatus fce_cache_lock_acquire(const char *cache_dir, FceFileLock *out_lock) {
    return cache_lock_acquire_impl(cache_dir, out_lock, 0);
}

FceStatus fce_cache_lock_acquire_shared(const char *cache_dir, FceFileLock *out_lock) {
    return cache_lock_acquire_impl(cache_dir, out_lock, 1);
}

void fce_cache_lock_release(FceFileLock *lock) {
    if (!lock || !lock->locked) return;
#if defined(_WIN32)
    if (lock->handle != INVALID_HANDLE_VALUE) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        UnlockFileEx(lock->handle, 0, MAXDWORD, MAXDWORD, &ov);
        CloseHandle(lock->handle);
        lock->handle = INVALID_HANDLE_VALUE;
    }
#else
    if (lock->fd >= 0) {
        struct flock unlock;
        memset(&unlock, 0, sizeof(unlock));
        unlock.l_type = F_UNLCK;
        unlock.l_whence = SEEK_SET;
        fcntl(lock->fd, F_SETLK, &unlock);
        close(lock->fd);
        lock->fd = -1;
    }
#endif
    lock->locked = 0;
    process_lock_leave(lock->shared);
}


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
