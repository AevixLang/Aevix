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