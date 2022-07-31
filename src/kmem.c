// kernel-style memory allocator
// SPDX-License-Identifier: Apache-2.0

#include "rsmimpl.h"
#define ILIST_TEST_IMPL
#include "list.h"
#include "bits.h"
#include "mem.h"

//
// This implements a universal heap allocator backed by a few small-size slabs
// and one or more subheaps, which are in turn backed by pages from a memory manager.
//
// When no space is found for an allocation request that fits in a slab, a new slab is
// allocated from a subheap.
// When no space is found for an allocation request in a subheap, another
// subheap is allocated and added to the subheaps list.
//

// KMEM_TRACE: uncomment to enable logging a lot of info via dlog
#define KMEM_TRACE

// KMEM_RUN_TEST_ON_INIT: uncomment to run tests during exe init in DEBUG builds
#define KMEM_RUN_TEST_ON_INIT

// _SCRUB_BYTE defines a byte value that, upon allocating or freeing a region,
// memory is filled with (ie. memset(ptr,_SCRUB_BYTE,size).)
// This is useful for debugging memory management issues, like use after free, since
// memory managed by this allocator is not subject to host memory protection.
// Set to zero to disable scrubbing.
#define KMEM_ALLOC_SCRUB_BYTE 0xbb
#define KMEM_FREE_SCRUB_BYTE  0xaa

// KMEM_SLABHEAP_ENABLE: define to enable use of slabheaps; speeds up small allocations
#define KMEM_SLABHEAP_ENABLE

// KMEM_SLABHEAP_ENABLE_EAGER_ALLOC: define to allocate slab space up front
//#define KMEM_SLABHEAP_ENABLE_EAGER_ALLOC

// SLABHEAP_COUNT dictates the slabheap size classes in increasing pow2, starting
// with SLABHEAP_MIN_SIZE. E.g. SLABHEAP_MIN_SIZE=8 SLABHEAP_COUNT=4 means we'll have
// the following slabheaps: 8, 16, 32, 64  (bytes). SLABHEAP_COUNT=6 means we'll have
// the following slabheaps: 8, 16, 32, 64, 128, 256  (bytes). And so on.
#define SLABHEAP_COUNT       4
#define SLABHEAP_MIN_SIZE    sizeof(void*) /* must be pow2 */
#define SLABHEAP_BLOCK_SIZE  ((usize)(PAGE_SIZE * 16lu))
#define SLABHEAP_BLOCK_MASK  (~(SLABHEAP_BLOCK_SIZE - 1))
#define SLABHEAP_MAX_BLOCKS  (SLABHEAP_BLOCK_SIZE / SLABHEAP_MIN_SIZE)
static_assert(IS_ALIGN2(SLABHEAP_BLOCK_SIZE, PAGE_SIZE), "");
static_assert(SLABHEAP_COUNT > 0, "undef KMEM_SLABHEAP_ENABLE instead");

#define HEAP_MIN_SIZE  (CHUNK_SIZE*2)

// HEAP_MAX_ALIGN: maximum alignment factor that heap_alloc can handle
#define HEAP_MAX_ALIGN  XMAX(PAGE_SIZE, SLABHEAP_BLOCK_SIZE)
static_assert(IS_POW2(HEAP_MAX_ALIGN), "");

typedef struct {
  usize    chunk_cap; // total_chunks
  usize    chunk_len; // number of used (allocated) chunks
  u8*      chunks;
  bitset_t chunk_use; // a set bit means that chunk is in use; is allocated
} heap_t;

typedef struct {
  ilist_t list_entry;
  heap_t  allocator;
} subheap_t;

typedef struct slabchunk_ {
  struct slabchunk_* nullable next;
} slabchunk_t;

typedef struct slabblock_ slabblock_t;
struct slabblock_ {
  slabblock_t* nullable next;    // next block in parent slabheap_t's list
  slabchunk_t*          recycle; // list of recycled chunks
  u32                   cap;     // total chunks at data
  u32                   len;     // unallocated chunks at data (<=SLABHEAP_MAX_BLOCKS)
};

typedef struct {
  usize                 size;   // chunk size
  slabblock_t* nullable usable; // list of blocks with free space
  slabblock_t* nullable full;   // list of blocks which chunks are all allocated
} slabheap_t;

typedef struct rmemalloc_ {
  rmm_t*     mm;
  RHMutex    lock;
  ilist_t    subheaps;
  void*      mem_origin;
  #ifdef KMEM_SLABHEAP_ENABLE
  slabheap_t slabheaps[SLABHEAP_COUNT];
  #endif
  bool       expansion_in_progress;
} rmemalloc_t;


static_assert(sizeof(subheap_t) <= PAGE_SIZE, "");


// CHUNK_SIZE: allocation chunk size, in bytes (must be a power of two)
// All subheap allocations are at least CHUNK_SIZE large.
// All subheap allocations are aligned to CHUNK_SIZE addresses.
#if UINTPTR_MAX < 0xffffffffffffffff
  #define CHUNK_SIZE 32u
#else
  #define CHUNK_SIZE 64u
#endif


// BEST_FIT_THRESHOLD: if the number of allocation chunks required are at least
// these many, use a "best fit" search instead of a "first fit" search.
// Used by kmem_alloc.
#define BEST_FIT_THRESHOLD 128u


#if defined(KMEM_TRACE) && defined(DEBUG)
  #define trace(fmt, args...) dlog("[kmem] " fmt, ##args)
#else
  #ifdef KMEM_TRACE
    #warning KMEM_TRACE has no effect unless DEBUG is enabled
    #undef KMEM_TRACE
  #endif
  #define trace(...) ((void)0)
#endif


#define CHUNK_MASK  (~((uintptr)CHUNK_SIZE - 1))


// heap_init initializes heap h with memory at p of size bytes
static void heap_init(heap_t* h, u8* p, usize size) {
  assertf(size >= HEAP_MIN_SIZE, "%zu", size);
  // Top (low address; p) of the heap is memory we allocate.
  // Bottom (high address) of the heap contains a bitset index of chunk use.
  // The amount of space we need for the bitset depends on how much space is left
  // after allocating the bitset, so that makes this a little tricky.
  //
  //  p                                                         p+size
  //  ┣━━━━━━━━━┯━━━━━━━━━┯━━━━━━━━━┯━━━━━━━━━┳━━━━━━━━━━━━━━━━━┫
  //  ┃ chunk 1 │ chunk 2 │ ...     │ chunk n ┃ bitset          ┃
  //  ┗━━━━━━━━━┷━━━━━━━━━┷━━━━━━━━━┷━━━━━━━━━╋━━━━━━━━━━━━━━━━━┛
  //                                        split
  //
  // We need to figure out the ideal "split"; where chunks end and bitset begin.
  // The bitset needs one bit per chunk and must be byte aligned.
  //
  // Begin by putting split at the end, leaving just one chunk for the bitset.
  // This is the highest split we can use, for the smallest size HEAP_MIN_SIZE.
  //

  // TODO: try binary search

  // ALT 2: exponential search
  const u8* p_end = p + size;
  usize chunk_cap = (size / CHUNK_SIZE) - 1;
  usize chunk_cap_sub = 1;
  u8* bitset_end = (p + (chunk_cap * CHUNK_SIZE)) + chunk_cap/8;
  while (bitset_end > p_end) {
    // dlog("p %p…%p, end bitset %p", p, p_end, bitset_end);
    chunk_cap -= chunk_cap_sub;
    chunk_cap_sub *= 2;
    bitset_end = (p + (chunk_cap * CHUNK_SIZE)) + chunk_cap/8;
  }
  // usize spill = (usize)((uintptr)p_end - (uintptr)bitset_end);
  // dlog("p %p…%p, split %p, end bitset %p, spill %zu",
  //   p, p_end, p + (chunk_cap * CHUNK_SIZE), bitset_end, spill);
  // assert((uintptr)bitset_end <= (uintptr)p_end);

  // // ALT 1: branchless approximation. Spills ~28 kiB for a 2 MiB memory size (~1.2%).
  // usize chunk_cap = size / (CHUNK_SIZE + 1);

  h->chunk_cap = chunk_cap;
  h->chunk_len = 0;
  h->chunks = p;
  bitset_init(&h->chunk_use, p + (chunk_cap * CHUNK_SIZE), chunk_cap);
}


// heap_contains returns true if h is the owner of allocation at ptr
static bool heap_contains(const heap_t* h, void const* ptr, usize size) {
  uintptr max_addr = (uintptr)h->chunks + (h->chunk_cap * CHUNK_SIZE);
  return ((uintptr)ptr >= (uintptr)h->chunks) & ((uintptr)ptr + size <= max_addr);
}


inline static usize heap_avail(const heap_t* h) {
  return (h->chunk_cap - h->chunk_len) * CHUNK_SIZE;
}


inline static usize heap_cap(const heap_t* h) {
  return h->chunk_cap * CHUNK_SIZE;
}


// heap_alloc finds space in the heap h that is at least *sizep bytes.
// Returns NULL if there's no space, otherwise it returns a pointer to the allocated
// region and updates *sizep to the effective byte size of the region.
static void* nullable heap_alloc(heap_t* h, usize* sizep, usize alignment) {
  // nchunks: the number of chunks we need.
  // We add an extra once since integer division rounds down but we need
  // the "ceiling", enough chunks to fit meta_size and *sizep.
  usize nchunks = (*sizep + CHUNK_SIZE - 1) / CHUNK_SIZE;

  // chunks_align: alignment requirement of chunk range
  usize chunks_align = alignment / CHUNK_SIZE;
  chunks_align += !chunks_align; // branchless "if (x==0) x = 1"

  // dlog("size          %5zu", *sizep);
  // dlog("alignment     %5zu", alignment);
  // dlog("nchunks       %5zu", nchunks);
  // dlog("chunks_align  %5zu", chunks_align);

  // chunk_index is the chunk we start searching
  usize chunk_index = 0;

  // Before we go look for a range of free chunks,
  // exit early if the number of available chunks are less than what's needed
  if (h->chunk_cap - h->chunk_len < nchunks + chunk_index)
    return NULL;

  // chunk_len will contain the number of consecutive chunks found
  usize chunk_len;

  // Now we will search for a free range in the "chunks in use" bitset h->chunk_use
  if (nchunks < BEST_FIT_THRESHOLD) {
    chunk_len = bitset_find_first_fit(&h->chunk_use, &chunk_index, nchunks, chunks_align);
  } else {
    chunk_len = bitset_find_best_fit(&h->chunk_use, &chunk_index, nchunks, chunks_align);
  }

  // Give up if we didn't find a range of chunks large enough
  if (chunk_len == 0)
    return NULL;

  // We found a range of free chunks!
  // Update the bitset to mark the chunks as "in use"
  bitset_set_range(&h->chunk_use, chunk_index, chunk_len, true);

  // Increment total number of chunks "in use" in the heap
  h->chunk_len += chunk_len;

  // chunk1 is the address of the first chunk.
  void* ptr = h->chunks + (chunk_index * CHUNK_SIZE);
  assertf(IS_ALIGN2((uintptr)ptr, alignment),
    "bug in %s (ptr %p, alignment %zu)", __FUNCTION__, ptr, alignment);

  // Return back to the caller the actual usable size of the allocation
  // dlog("req. size     %5zu", *sizep);
  // dlog("usable size   %5zu", chunk_len * CHUNK_SIZE);
  assert(chunk_len * CHUNK_SIZE >= *sizep);
  *sizep = chunk_len * CHUNK_SIZE;

  // fill allocated memory with scrub bytes (if enabled)
  if (KMEM_ALLOC_SCRUB_BYTE)
    memset(ptr, KMEM_ALLOC_SCRUB_BYTE, chunk_len * CHUNK_SIZE);

  trace("[heap] allocating %p (%zu B) in %zu chunks [%zu…%zu)",
    ptr, *sizep, chunk_len, chunk_index, chunk_index + chunk_len);

  return ptr;
}


static void heap_free(heap_t* h, void* ptr, usize size) {
  assert(heap_contains(h, ptr, size));

  // calculate chunk index for the allocation
  uintptr chunk_addr = (uintptr)ptr & CHUNK_MASK;
  uintptr chunk_index = (chunk_addr - (uintptr)h->chunks) / CHUNK_SIZE;
  usize chunk_len = size / CHUNK_SIZE;

  trace("[heap] freeing chunk %p (%zu B) in %zu chunks [%zu…%zu)",
    ptr, size, chunk_len, chunk_index, chunk_index + chunk_len);

  assertf(bitset_get(&h->chunk_use, chunk_index),
    "trying to free segment starting at %zu that is already free (ptr=%p)",
    chunk_index, ptr);

  bitset_set_range(&h->chunk_use, chunk_index, chunk_len, false);

  assert(h->chunk_len >= chunk_len);
  h->chunk_len -= chunk_len;

  // fill freed memory with scrub bytes, if enabled
  if (KMEM_FREE_SCRUB_BYTE)
    memset(ptr, KMEM_FREE_SCRUB_BYTE, size);
}


#if DEBUG
  UNUSED static void heap_debug_dump_state(
    heap_t* h, void* nullable highlight_p, usize highlight_size)
  {
    if (h->chunk_len == 0) {
      fputs("(empty)\n", stderr);
      return;
    }
    // find last set bit in the bitmap
    const usize bucket_bits = 8 * sizeof(usize);
    const bitset_t bset = h->chunk_use;
    const usize last_bucket = bset.len / bucket_bits;
    const usize* buckets = (const usize*)bset.data;
    usize last_used_bit = 0;
    for (usize bucket = 0; bucket < last_bucket; bucket++) {
      if (buckets[bucket] == 0)
        continue;
      last_used_bit = (bucket * bucket_bits) + (usize)rsm_fls(buckets[bucket]);
    }
    usize trailing_bytes = (bset.len % bucket_bits) / 8;
    for (usize i = (bset.len / bucket_bits) * sizeof(usize); i < trailing_bytes; ++i) {
      if (((u8*)bset.data)[i])
        last_used_bit = i * 8;
    }

    assert(h->chunk_use.len > last_used_bit);
    fputs(
      "────┬──────────┬───────────"
      "────────────────────────────────────────────────────────────\n"
      "page│   address│ chunk use\n"
      "────┼──────────┼───────────"
      "────────────────────────────────────────────────────────────\n"
      , stderr);
    uintptr page_addr, chunk_idx;

    uintptr highlight_start_addr = ALIGN2_FLOOR((uintptr)highlight_p, CHUNK_SIZE);
    uintptr highlight_end_addr = highlight_start_addr + ALIGN2(highlight_size, CHUNK_SIZE);

    for (chunk_idx = 0; chunk_idx < last_used_bit+1; chunk_idx++) {
      if ((chunk_idx % (PAGE_SIZE / CHUNK_SIZE)) == 0) {
        if (chunk_idx) fputc('\n', stderr);
        page_addr = (uintptr)h->chunks + (chunk_idx * CHUNK_SIZE);
        usize page_idx = (chunk_idx * CHUNK_SIZE) / PAGE_SIZE;
        fprintf(stderr, "%4zu│%10lx│%6zu ", page_idx, page_addr, chunk_idx);
      }
      uintptr addr = (uintptr)h->chunks + (chunk_idx * CHUNK_SIZE);
      if (highlight_start_addr <= addr && addr < highlight_end_addr) {
        fputs(bitset_get(&h->chunk_use, chunk_idx) ? "▓" : "_", stderr);
      } else {
        fputs(bitset_get(&h->chunk_use, chunk_idx) ? "░" : "_", stderr);
      }
    }
    fprintf(stderr,
      "\n···─┼────···───┼───···─────"
      "────────────────────────────────────────────────────────────\n"
      "%4zu│%10lx│%6zu END\n"
      "────┴──────────┴───────────"
      "────────────────────────────────────────────────────────────\n"
      ,
      ((h->chunk_cap * CHUNK_SIZE) / PAGE_SIZE) + 1,
      (uintptr)h->chunks + (h->chunk_cap * CHUNK_SIZE), // end address
      h->chunk_cap);
  }
#endif // DEBUG


inline static void subheap_init(subheap_t* sh, u8* base, usize size) {
  heap_init(&sh->allocator, base, size);
}

inline static void* nullable subheap_alloc(subheap_t* sh, usize* size, usize alignment) {
  return heap_alloc(&sh->allocator, size, alignment);
}

inline static usize subheap_avail(const subheap_t* sh) {
  return heap_avail(&sh->allocator);
}

inline static usize subheap_cap(const subheap_t* sh) {
  return heap_cap(&sh->allocator);
}


static bool kmem_add_subheap(rmemalloc_t* a, void* storage, usize size) {
  if (size < ALIGN2(sizeof(subheap_t), _Alignof(subheap_t)) + HEAP_MIN_SIZE) {
    trace("[%s] size (%zu) too small", __FUNCTION__, size);
    return false;
  }

  subheap_t* sh;

  // If storage is aligned, place subheap struct at the end to minimize spill.
  // Otherwise we place the subheap struct at the beginning and align storage.
  if (IS_ALIGN2((uintptr)storage, HEAP_MAX_ALIGN)) {
    uintptr end_addr = (uintptr)storage + size;
    sh = (subheap_t*)ALIGN2_FLOOR(end_addr - sizeof(subheap_t), _Alignof(subheap_t));
    if (check_sub_overflow((usize)(uintptr)sh, (usize)(uintptr)storage, &size)) {
      trace("[%s] not enough space at %p for subheap_t", __FUNCTION__,sh);
      return false;
    }
    assert((uintptr)storage + size <= (uintptr)sh);
  } else {
    sh = storage;
    storage = (subheap_t*)ALIGN2((uintptr)storage + sizeof(subheap_t), HEAP_MAX_ALIGN);
    usize size_diff = (usize)((uintptr)storage - (uintptr)sh);
    if (size_diff > 0)
      trace("forfeiting %zu kiB in subheap (HEAP_MAX_ALIGN alignment)", size_diff / kiB);
    if (check_sub_overflow(size, size_diff, &size)) {
      trace("[%s] not enough space at %p for HEAP_MAX_ALIGN alignment", __FUNCTION__,sh);
      return false;
    }
  }

  if (size < HEAP_MIN_SIZE) {
    trace("[%s] size (%zu) too small after HEAP_MAX_ALIGN alignment", __FUNCTION__,size);
    return false;
  }

  trace("add subheap %p (%p … %p, %zu kiB)", sh, storage, storage + size, size / kiB);
  assertf(IS_ALIGN2((uintptr)storage, HEAP_MAX_ALIGN), "%p", storage);

  subheap_init(sh, storage, size);
  ilist_append(&a->subheaps, &sh->list_entry);

  return true;
}


#if DEBUG
  UNUSED static void kmem_debug_dump_state(
    rmemalloc_t* a, void* nullable highlight_p, usize highlight_size)
  {
    RHMutexLock(&a->lock);
    usize i = 0;
    ilist_for_each(lent, &a->subheaps) {
      subheap_t* sh = ilist_entry(lent, subheap_t, list_entry);
      heap_t* h = &sh->allocator;
      uintptr start_addr = (uintptr)h->chunks;
      uintptr end_addr = (uintptr)h->chunks + (h->chunk_cap * CHUNK_SIZE);
      uintptr end_addr_use = (uintptr)h->chunks + (h->chunk_len * CHUNK_SIZE);
      dlog("subheap %zu %zx…%zx %zu kiB (%zu kiB, %zu chunks in use)",
        i, start_addr, end_addr,
        (end_addr - start_addr) / 1024,
        (end_addr_use - start_addr) / 1024,
        h->chunk_len);
      heap_debug_dump_state(h, highlight_p, highlight_size);
    }
    RHMutexUnlock(&a->lock);
  }
#endif


static bool kmem_expand(rmemalloc_t* a, usize minsize) {
  panic("TODO expand memory");
  return false;
}


static void* nullable kmem_heapalloc(rmemalloc_t* a, usize* size, usize alignment) {
  // assertf(alignment <= PAGE_SIZE, "%zu", alignment);
  ilist_for_each(lent, &a->subheaps) {
    subheap_t* sh = ilist_entry(lent, subheap_t, list_entry);
    void* ptr = subheap_alloc(sh, size, alignment);
    if (ptr)
      return ptr;
  }
  return NULL;
}


#ifdef KMEM_SLABHEAP_ENABLE


static slabblock_t* nullable slabheap_grow(rmemalloc_t* a, slabheap_t* sh) {
  trace("[slab %zu] grow", sh->size);
  assertnull(sh->usable);

  usize size = SLABHEAP_BLOCK_SIZE;
  static_assert(_Alignof(slabblock_t) <= SLABHEAP_MIN_SIZE, "");

  slabblock_t* block;
  for (usize attempt = 10; ; attempt++) {
    if LIKELY(( block = kmem_heapalloc(a, &size, SLABHEAP_BLOCK_SIZE) ))
      break;
    if UNLIKELY(!kmem_expand(a, size) || attempt == 0)
      return NULL;
  }

  trace("[slab %zu] allocated backing block %p", sh->size, block);
  assertf((uintptr)block % SLABHEAP_BLOCK_SIZE == 0,
    "misaligned address %p returned by kmem_heapalloc", block);

  block->cap = SLABHEAP_BLOCK_SIZE / sh->size;
  block->len = 0;
  block->recycle = NULL;
  block->next = NULL;
  sh->usable = block; // set as "usable" list
  return block;
}


static void* nullable slabheap_alloc(rmemalloc_t* a, slabheap_t* sh) {
  slabblock_t* block = sh->usable;

  // If there are no usable blocks, attempt to allocate a new one
  if UNLIKELY(block == NULL) {
    if (!(block = slabheap_grow(a, sh)))
      return NULL;
  }

  // Try to recycle a chunk
  slabchunk_t* chunk = block->recycle;

  // No chunk to recycle; allocate a new one from the block
  if UNLIKELY(chunk == NULL) {
    assertf(block->cap - block->len > 0, "full block %p found on sh->usable!", block);
    uintptr data_addr = ALIGN2((uintptr)block + sizeof(slabblock_t), sh->size);
    assertf(data_addr % sh->size == 0, "misaligned data_addr %p", (void*)data_addr);
    chunk = (slabchunk_t*)( data_addr + (block->len * sh->size) );
    chunk->next = NULL; // for "block->recycle = chunk->next" later on
    block->len++;
  }

  // Dequeue the chunk from the block's recycle list (free list)
  // No matter if the chunk was the last one or the list was empty (branch above),
  // this has the same effect.
  block->recycle = chunk->next;

  // if the recycle list is empty and all chunks are allocated, the block is full
  // and we need to move the block to the sh->full list
  if (chunk->next == NULL && block->len == block->cap) {
    trace("[slab %zu] mark block %p as full", sh->size, block);
    sh->usable = block->next;
    block->next = sh->full;
    sh->full = block;
  }

  #ifdef KMEM_TRACE
  uintptr data_addr = ALIGN2((uintptr)block + sizeof(slabblock_t), sh->size);
  trace("[slab %zu] allocating chunk %zu %p from block %p",
    sh->size, ((uintptr)chunk - data_addr) / sh->size, chunk, block);
  #endif

  return chunk;
}


static void slabheap_free(rmemalloc_t* a, slabheap_t* sh, void* ptr) {
  assertf( !((uintptr)ptr % sh->size), "invalid address %p (slab %zu)", ptr, sh->size);

  // fill freed memory with scrub bytes, if enabled
  if (KMEM_FREE_SCRUB_BYTE)
    memset(ptr, KMEM_FREE_SCRUB_BYTE, sh->size);

  slabblock_t* block = (slabblock_t*)((uintptr)ptr & SLABHEAP_BLOCK_MASK);

  bool block_full = block->recycle == NULL && block->len == block->cap;

  // add chunk to the block's recycle list
  slabchunk_t* chunk = ptr;
  chunk->next = block->recycle;
  block->recycle = chunk;

  #ifdef KMEM_TRACE
  uintptr data_addr = ALIGN2((uintptr)block + sizeof(slabblock_t), sh->size);
  trace("[slab %zu] freeing chunk %zu %p from block %p",
    sh->size, ((uintptr)ptr - data_addr) / sh->size, ptr, block);
  #endif

  // If the block was fully used, it no longer is and we need to
  // move it from the "full" list to the "usable" list.
  if (block_full) {
    trace("[slab %zu] mark block %p as usable", sh->size, block);
    sh->full = block->next;
    block->next = sh->usable;
    sh->usable = block;
  }
}


static usize slabheap_avail(const slabheap_t* sh) {
  usize nbyte = 0;
  for (const slabblock_t* block = sh->usable; block; block = block->next) {
    nbyte += block->len * sh->size;
    for (const slabchunk_t* chunk = block->recycle; chunk; chunk = chunk->next)
      nbyte += sh->size;
  }
  return nbyte;
}


#endif // KMEM_SLABHEAP_ENABLE


// kmem_alloc_aligned attempts to allocate *size bytes.
// On success, *size is updated to its actual size.
rmem2_t kmem_alloc_aligned(rmemalloc_t* a, usize size, usize alignment) {
  assertf(IS_POW2(alignment), "alignment %zu is not a power-of-two", alignment);
  assertf(alignment <= PAGE_SIZE, "%zu", alignment);

  void* ptr = NULL;

  #ifdef KMEM_SLABHEAP_ENABLE
    const usize slabsize = ALIGN2(size, alignment);
  #endif

  RHMutexLock(&a->lock);
  assert(!a->expansion_in_progress);

retry:

  // Attempt to allocate space in a slabheap.
  // This succeeds for the common case of a small allocation size.
  #ifdef KMEM_SLABHEAP_ENABLE
    for (usize i = 0; i < SLABHEAP_COUNT; i++) {
      if (slabsize <= a->slabheaps[i].size) {
        size = a->slabheaps[i].size;
        ptr = slabheap_alloc(a, &a->slabheaps[i]);
        if UNLIKELY(ptr == NULL)
          goto expand;
        goto end;
      }
    }
  #endif

  // Attempt to allocate space in a subheap
  if LIKELY((ptr = kmem_heapalloc(a, &size, alignment)))
    goto end;

#ifdef KMEM_SLABHEAP_ENABLE
expand:
#endif

  if (kmem_expand(a, size))
    goto retry;

end:
  RHMutexUnlock(&a->lock);
  return (rmem2_t){ .start=ptr, .size=size };
}


void kmem_free(rmemalloc_t* a, rmem2_t region) {
  assertnotnull(region.start);

  RHMutexLock(&a->lock);
  assert(!a->expansion_in_progress);

  #ifdef KMEM_SLABHEAP_ENABLE
    for (usize i = 0; i < SLABHEAP_COUNT; i++) {
      if (region.size <= a->slabheaps[i].size) {
        slabheap_free(a, &a->slabheaps[i], region.start);
        goto end;
      }
    }
  #endif

  ilist_for_each(lent, &a->subheaps) {
    subheap_t* sh = ilist_entry(lent, subheap_t, list_entry);
    if (!heap_contains(&sh->allocator, region.start, region.size))
      continue;
    heap_free(&sh->allocator, region.start, region.size);
    goto end;
  }

  safecheckf(0, "kmem_free: invalid region " RMEM_FMT, RMEM_FMT_ARGS(region));
end:
  RHMutexUnlock(&a->lock);
}


usize kmem_alloc_size(usize size) {
  assert(size > 0);

  #ifdef KMEM_SLABHEAP_ENABLE
  usize max_slab_size = 1lu << ((SLABHEAP_COUNT - 1) + ILOG2(SLABHEAP_MIN_SIZE));
  if (size <= max_slab_size)
    return CEIL_POW2(size);
  #endif

  return ALIGN2(size, CHUNK_SIZE);
}


rmemalloc_t* nullable kmem_allocator_create(rmm_t* mm, usize min_initmem) {
  const usize allocator_size = ALIGN2(sizeof(rmemalloc_t), _Alignof(rmemalloc_t));

  // initmem needs to be aligned to CHUNK_SIZE
  min_initmem = ALIGN_CEIL(min_initmem, CHUNK_SIZE);

  // rmm requires page allocations in pow2 orders
  usize npages = CEIL_POW2(ALIGN2(allocator_size + min_initmem, PAGE_SIZE) / PAGE_SIZE);
  usize nbyte = npages * PAGE_SIZE;

  trace("create allocator with %zu pages (%zu kiB, %.2f kiB usable)",
    npages,  nbyte / kiB,  (double)(nbyte - allocator_size) / (double)kiB);

  void* p = rmm_allocpages(mm, npages);
  if (!p)
    return NULL;

  // place the allocator at the end of the page range to increase the chances
  // of perfect alignment of the initial heap (which has HEAP_MAX_ALIGN alignment.)
  rmemalloc_t* a = (void*)( ((uintptr)p + nbyte) - allocator_size );
  memset(a, 0, sizeof(*a));
  a->mm = mm;
  a->mem_origin = p;
  ilist_init(&a->subheaps);
  if (!RHMutexInit(&a->lock))
    return NULL;

  // initialize slab heaps, starting with size=sizeof(void*)
  // TODO: tune these sizes once we have some stats on usage.
  #ifdef KMEM_SLABHEAP_ENABLE
    for (usize i = 0; i < SLABHEAP_COUNT; i++) {
      a->slabheaps[i].size = 1lu << (i + ILOG2(SLABHEAP_MIN_SIZE));
      a->slabheaps[i].usable = NULL;
      a->slabheaps[i].full = NULL;
      trace("init slabheaps[%zu] (%zu B)", i, a->slabheaps[i].size);
    }
  #endif

  // use the rest of the memory allocated for the allocator struct as a subheap
  // TODO: consider using this as a slabheap instead (when we have slabheaps)
  if (!kmem_add_subheap(a, p, nbyte - allocator_size))
    trace("failed to add initial subheap; not enough space and/or alignment too small");

  // allocate initial slab blocks up front, if enabled
  #if defined(KMEM_SLABHEAP_ENABLE) && defined(KMEM_SLABHEAP_ENABLE_EAGER_ALLOC)
    for (usize i = SLABHEAP_COUNT; i--; ) {
      if UNLIKELY(!slabheap_grow(a, &a->slabheaps[i])) {
        // We're out of memory, but don't do anything about it since we are
        // just optimistically allocating slab space here
        break;
      }
    }
  #endif

  return a;
}


void kmem_allocator_free(rmemalloc_t* a) {
  // TODO: free slabheaps
  // TODO: free additional subheaps
  rmm_freepages(a->mm, a->mem_origin);
}


usize kmem_avail(rmemalloc_t* a) {
  RHMutexLock(&a->lock);
  usize nbyte = 0;

  #ifdef KMEM_SLABHEAP_ENABLE
    for (usize i = 0; i < SLABHEAP_COUNT; i++) {
      nbyte += slabheap_avail(&a->slabheaps[i]);
    }
  #endif

  ilist_for_each(lent, &a->subheaps) {
    subheap_t* sh = ilist_entry(lent, subheap_t, list_entry);
    nbyte += subheap_avail(sh);
  }

  RHMutexUnlock(&a->lock);
  return nbyte;
}


usize kmem_cap(rmemalloc_t* a) {
  RHMutexLock(&a->lock);

  usize nbyte = 0;

  // note: slabs are allocated in subheaps, so we don't need to count those
  ilist_for_each(lent, &a->subheaps) {
    subheap_t* sh = ilist_entry(lent, subheap_t, list_entry);
    nbyte += subheap_cap(sh);
  }

  RHMutexUnlock(&a->lock);
  return nbyte;
}


#if defined(KMEM_RUN_TEST_ON_INIT) && defined(DEBUG)
static void test_kmem() {

  // test "not enough memory to create allocator"
  {
    const usize allocator_size = ALIGN2(sizeof(rmemalloc_t), _Alignof(rmemalloc_t));

    usize memsize = (ALIGN2(allocator_size, PAGE_SIZE) + 2) * PAGE_SIZE;
    void* memp = assertnotnull( osvmem_alloc(memsize) );
    rmm_t* mm = assertnotnull( rmm_create(memp, memsize) );

    // allocate all pages but what's required for the allocator
    // (must allocate one at a time since rmm_allocpages needs pow2(count))
    usize npages = rmm_avail(mm) - (ALIGN2(allocator_size, PAGE_SIZE) / PAGE_SIZE);
    while (npages--)
      assertnotnull( rmm_allocpages(mm, 1) );

    // kmem_allocator_create with initmem = 0 should succeed
    rmemalloc_t* a = assertnotnull( kmem_allocator_create(mm, 0) );
    kmem_allocator_free(a);

    // kmem_allocator_create with initmem > 0 should fail
    assertnull( kmem_allocator_create(mm, PAGE_SIZE) );

    osvmem_free(memp, memsize);
    rmm_dispose(mm);
  }

  // create a memory manager
  usize memsize = 16 * MiB;
  rmm_t* mm = rmm_create(assertnotnull( osvmem_alloc(memsize) ), memsize);

  rmemalloc_t* a = assertnotnull( kmem_allocator_create(mm, 4 * MiB) );

  usize z = kmem_alloc_size(123);
  dlog("kmem_alloc_size(123) => %zu", z);

  rmem2_t p1, p2, p3, p4, p5;

  // slabheap
  // rmem2_t regions[SLABHEAP_BLOCK_SIZE / 64];
  rmem2_t regions[4];
  for (usize i = 0; i < countof(regions); i++)
    regions[i] = kmem_alloc(a, 64);
  for (usize i = 0; i < countof(regions); i++)
    kmem_free(a, regions[i]);
  // push it over the limit
  p1 = kmem_alloc(a, 64);
  kmem_free(a, p1);

  p1 = kmem_alloc(a, z - 3);
  dlog("kmem_alloc(%zu) => " RMEM_FMT, z, RMEM_FMT_ARGS(p1));

  usize req_size = 100;
  p2 = kmem_alloc_aligned(a, req_size, 512);
  dlog("kmem_alloc_aligned(%zu,512) => " RMEM_FMT " (expect %p)",
    req_size, RMEM_FMT_ARGS(p2), (void*)ALIGN2((uintptr)p2.start,512) );

  kmem_free(a, p1);
  kmem_free(a, p2);

  p3 = kmem_alloc(a, 800);
  dlog("kmem_alloc(800) => " RMEM_FMT, RMEM_FMT_ARGS(p3));
  kmem_free(a, p3);

  p1 = kmem_alloc(a, CHUNK_SIZE*(BEST_FIT_THRESHOLD-2));
  p1 = kmem_alloc(a, CHUNK_SIZE);   // 0-2
  p2 = kmem_alloc(a, CHUNK_SIZE*3); // 2-6
  p3 = kmem_alloc(a, CHUNK_SIZE);   // 6-8
  p4 = kmem_alloc(a, CHUNK_SIZE);   // 8-10
  p5 = kmem_alloc(a, CHUNK_SIZE*3); // 10-14
  kmem_free(a, p2);
  kmem_free(a, p4);
  // kmem_debug_dump_state(a, NULL, 0);
  // now, for a CHUNK_SIZE allocation,
  // the "best fit" allocation strategy should select chunks 8-10, and
  // the "first fit" allocation strategy should select chunks 2-4.

  p2 = kmem_alloc(a, CHUNK_SIZE);
  // kmem_debug_dump_state(a, p2, CHUNK_SIZE);
  kmem_free(a, p2);

  kmem_free(a, p5);
  kmem_free(a, p3);
  kmem_free(a, p1);

  kmem_allocator_free(a);
  osvmem_free((void*)rmm_startaddr(mm), memsize);
  rmm_dispose(mm);

  log("——————————————————");
}
#endif // KMEM_RUN_TEST_ON_INIT


rerror init_rmem_allocator() {
  #if defined(KMEM_RUN_TEST_ON_INIT) && defined(DEBUG)
  test_kmem();
  #endif
  // currently nothing to initialize
  return 0;
}

