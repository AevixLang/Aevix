/*
 * Aevix arena runtime: a chunked bump allocator backed by virtual memory.
 *
 * Chunks are mapped on demand, so the arena grows until the OS refuses the
 * mapping instead of hitting a fixed cap. Memory is never freed individually;
 * epoch blocks save/restore the current (chunk, offset) pair, which logically
 * rolls back every allocation made between the two calls.
 *
 * Compiled to runtime.o and linked into every Aevix binary (see CMakeLists;
 * frontend tests and the Go CLI pass it to clang alongside the llc output).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define ARENA_ALIGN 16u
#define ARENA_CHUNK (64u * 1024u * 1024u) /* commit granularity: 64 MiB */
#define ARENA_MAX_EPOCHS 4096

typedef struct arena_frame {
    uint8_t* chunk;
    size_t off;
    size_t cap;
} arena_frame;

static uint8_t* cur_chunk = NULL;
static size_t cur_off = 0;
static size_t cur_cap = 0;

static arena_frame frames[ARENA_MAX_EPOCHS];
static size_t frame_depth = 0;

#if defined(_WIN32)
/* VirtualAlloc commits eagerly; Windows does not overcommit, so the arena is
 * limited by real RAM here rather than by virtual address space. */
static int reserve_chunk(size_t bytes, uint8_t** out) {
    uint8_t* p = (uint8_t*)VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    *out = p;
    return p != NULL;
}
#else
static int reserve_chunk(size_t bytes, uint8_t** out) {
    void* p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        *out = NULL;
        return 0;
    }
    *out = (uint8_t*)p;
    return 1;
}
#endif

static void arena_oom(void) {
    fputs("arena out of memory\n", stderr);
    exit(1);
}

/* Makes sure the current chunk fits `need` more bytes, opening a new mapping
 * otherwise. Allocations never span chunk boundaries. */
static void ensure_capacity(size_t need) {
    if (cur_chunk != NULL && cur_off + need <= cur_cap) return;
    size_t bytes = need > ARENA_CHUNK ? need : ARENA_CHUNK;
    uint8_t* fresh = NULL;
    if (!reserve_chunk(bytes, &fresh)) arena_oom();
    cur_chunk = fresh;
    cur_cap = bytes;
    cur_off = 0;
}

void* __aevix_alloc(int32_t bytes) {
    if (bytes < 0) arena_oom();
    size_t need = (size_t)bytes;
    size_t aligned = (cur_off + (ARENA_ALIGN - 1)) & ~(ARENA_ALIGN - 1);
    cur_off = aligned;
    ensure_capacity(need);
    uint8_t* p = cur_chunk + cur_off;
    cur_off += need;
    return p;
}

int32_t __aevix_epoch_begin(void) {
    if (cur_chunk == NULL) ensure_capacity(ARENA_CHUNK);
    if (frame_depth >= ARENA_MAX_EPOCHS) arena_oom();
    frames[frame_depth].chunk = cur_chunk;
    frames[frame_depth].off = cur_off;
    frames[frame_depth].cap = cur_cap;
    return (int32_t)++frame_depth;
}

void __aevix_epoch_end(int32_t saved) {
    if (saved <= 0 || (size_t)saved > frame_depth) arena_oom();
    frame_depth = (size_t)saved - 1;
    arena_frame f = frames[frame_depth];
    cur_chunk = f.chunk;
    cur_off = f.off;
    cur_cap = f.cap;
}

/* Content equality for string slices: returns 1 if both slices hold the same
 * bytes, 0 otherwise. Lengths are compared first, so mismatched sizes short-cut. */
int32_t __aevix_str_eq(const uint8_t* a, int32_t alen, const uint8_t* b, int32_t blen) {
    if (alen != blen) return 0;
    for (int32_t i = 0; i < alen; ++i) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/* Reads an entire file into arena memory. On failure (missing file, read
 * error) out_ptr/out_len are set to NULL/0, i.e. the caller sees "". */
void __aevix_read_file(const uint8_t* path, uint8_t** out_ptr, int32_t* out_len) {
    FILE* f = fopen((const char*)path, "rb");
    if (f == NULL) {
        *out_ptr = NULL;
        *out_len = 0;
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        *out_ptr = NULL;
        *out_len = 0;
        return;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        *out_ptr = NULL;
        *out_len = 0;
        return;
    }
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = sz > 0 ? (uint8_t*)__aevix_alloc((int32_t)sz) : NULL;
    size_t got = 0;
    if (sz > 0) got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out_ptr = buf;
    *out_len = (int32_t)got;
}

/* Writes a byte buffer to a file, truncating it first. Returns 1 on success,
 * 0 if the file could not be opened or the write was short. */
int32_t __aevix_write_file(const uint8_t* path, const uint8_t* data, int32_t len) {
    FILE* f = fopen((const char*)path, "wb");
    if (f == NULL) return 0;
    size_t w = len > 0 ? fwrite(data, 1, (size_t)len, f) : 0;
    int rc = fclose(f);
    if (rc != 0) return 0;
    return w == (size_t)len ? 1 : 0;
}