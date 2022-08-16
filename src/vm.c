// virtual memory
// See vmem.txt for in-depth documentation
// SPDX-License-Identifier: Apache-2.0
#include "rsmimpl.h"
#include "vm.h"

// VM_RUN_TEST_ON_INIT: define to run tests during exe init in DEBUG builds
#define VM_RUN_TEST_ON_INIT

// VM_TRACE: define to enable logging a lot of info via dlog
//#define VM_TRACE

// VFN_BITS: bits needed for VFN (VM_ADDR_BITS-PAGE_SIZE_BITS)
#define VFN_BITS  (VM_ADDR_BITS - PAGE_SIZE_BITS)


#if defined(VM_TRACE) && defined(DEBUG)
  #define trace(fmt, args...) dlog("[vm] " fmt, ##args)
#else
  #ifdef VM_TRACE
    #warning VM_TRACE has no effect unless DEBUG is enabled
    #undef VM_TRACE
  #endif
  #define trace(...) ((void)0)
#endif


static_assert(ALIGN2_X(VM_PTAB_SIZE, PAGE_SIZE) == VM_PTAB_SIZE,
  "VM_PTAB_SIZE not page aligned");

// sanity check; VM_PTAB_SIZE should end up being exactly one page size
static_assert(VM_PTAB_SIZE == (VM_PTAB_LEN * sizeof(vm_pte_t)), "");

static_assert(sizeof(vm_pte_t) == sizeof(u64), "vm_pte_t too large");


// vm_pte_t printf formatting
#define PTE_FMT          "(0x%llx)"
#define PTE_FMTARGS(pte) (pte).outaddr


// getbits returns the (right adjusted) n-bit field of x that begins at position p.
// We assume that bit position 0 is at the right end and that n and p
// are sensible positive values.
// For example, getbits(x, 4, 3) returns the three bits in bit
// positions 4, 3 and 2, right adjusted.
// [from K&R, 2nd Ed., pg. 49: get n bits from position p]
inline static u64 getbits(u64 x, u32 p, u32 n) {
  return (x >> (p+1-n)) & ~(~0llu << n);
}


static vm_pte_t vm_pte_make(u64 outaddr) {
  return (vm_pte_t){ .outaddr = outaddr };
}


static vm_ptab_t nullable vm_ptab_create(rmm_t* mm) {
  // note: VM_PTAB_SIZE is always a multiple of PAGE_SIZE
  vm_ptab_t ptab = rmm_allocpages(mm, VM_PTAB_SIZE / PAGE_SIZE);
  if UNLIKELY(ptab == NULL)
    return NULL;
  #ifdef VM_ZERO_PAGES
  memset(ptab, 0, VM_PTAB_SIZE);
  #endif
  return ptab;
}


static void vm_ptab_free(rmm_t* mm, vm_ptab_t ptab) {
  rmm_freepages(mm, ptab);
}


rerr_t vm_pagedir_init(vm_pagedir_t* pagedir, rmm_t* mm) {
  if (!RHMutexInit(&pagedir->lock))
    return rerr_not_supported;
  vm_ptab_t ptab = vm_ptab_create(mm); // root page table
  if UNLIKELY(!ptab) {
    trace("failed to allocate root page table");
    return rerr_nomem;
  }
  trace("allocated L%u page table %p +0x%lx", 1, ptab, VM_PTAB_SIZE);
  pagedir->root = ptab;
  pagedir->mm = mm;
  return 0;
}


void vm_pagedir_dispose(vm_pagedir_t* pagedir) {
  vm_ptab_free(pagedir->mm, pagedir->root);
  dlog("TODO: free all PTEs");
}


static vm_pagedir_t* nullable vm_pagedir_create(rmm_t* mm) {
  // FIXME whole page allocated!
  static_assert(sizeof(vm_pagedir_t) < PAGE_SIZE, "");
  vm_pagedir_t* pagedir = rmm_allocpages(mm, 1);
  if (!pagedir)
    return NULL;
  if (vm_pagedir_init(pagedir, mm)) {
    rmm_freepages(mm, pagedir);
    return NULL;
  }
  return pagedir;
}


static vm_pte_t vm_pagedir_alloc_backing_page(vm_pagedir_t* pagedir, vm_pte_t* pte) {
  uintptr haddr = (uintptr)rmm_allocpages(pagedir->mm, 1);
  uintptr hpage_addr = haddr >> PAGE_SIZE_BITS;
  if UNLIKELY(hpage_addr == 0) {
    trace("FAILED to allocate backing page");
    panic("TODO: purge a least-recently-used page");
  }
  trace("allocated backing page %p", (void*)haddr);
  *pte = vm_pte_make(hpage_addr);
  return *pte;
}


// vm_pagedir_lookup_hfn returns the Host Frame Number for a Virtual Frame Number
static vm_pte_t vm_pagedir_lookup_pte(vm_pagedir_t* pagedir, u64 vfn) {
  assertf(vfn > 0, "invalid VFN 0x0 (vm address likely less than VM_ADDR_MIN)");
  vfn--; // subtract one to make VM_ADDR_MIN VFN 0
  u32 bits = 0;
  u64 masked_vfn = vfn;
  vm_ptab_t ptab = pagedir->root;
  u8 level = 1;

  vm_pte_t pte = {0};
  RHMutexLock(&pagedir->lock);

  for (;;) {
    u64 index = getbits(masked_vfn, VFN_BITS - (1+bits), VM_PTAB_BITS);
    pte = ptab[index];

    trace(
      "lookup vfn 0x%llx L %u; index %llu = getbits(0x%llx, %u-(1+%u), %u)",
      vfn+1, level, index, masked_vfn, VFN_BITS, bits, VM_PTAB_BITS);

    if (level == VM_PTAB_LEVELS) {
      if UNLIKELY(*(u64*)&pte == 0) {
        trace("first access to page vfn=0x%llx", vfn+1);
        pte = vm_pagedir_alloc_backing_page(pagedir, &ptab[index]);
      }
      goto end;
    }

    bits += VM_PTAB_BITS;
    masked_vfn = getbits(masked_vfn, VFN_BITS - (1+bits), VFN_BITS - bits);
    level++;

    if (pte.outaddr) {
      ptab = (vm_ptab_t)(uintptr)(pte.outaddr << PAGE_SIZE_BITS);
      continue;
    }

    // allocate a new page table
    vm_ptab_t ptab2 = vm_ptab_create(pagedir->mm);
    if UNLIKELY(!ptab2) {
      // out of backing memory; try to free some up memory up by purging unused tables
      panic("TODO: purge unused page tables (except for the root)");
    }

    assertf(IS_ALIGN2((u64)(uintptr)ptab2, PAGE_SIZE),
      "ptab_create did not allocate vm_ptab_t on a page boundary (0x%lx/%u)",
      (uintptr)ptab2, PAGE_SIZE);

    u64 ptab2_addr = ((u64)(uintptr)ptab2) >> PAGE_SIZE_BITS;
    ptab[index] = vm_pte_make(ptab2_addr);
    ptab = ptab2;

    trace("allocated L%u page table %p +0x%lx at [%llu]",
      level, ptab, VM_PTAB_SIZE, index);

    if UNLIKELY(!ptab) {
      pte = (vm_pte_t){0};
      goto end;
    }
  }
end:
  RHMutexUnlock(&pagedir->lock);
  return pte;
}


uintptr vm_pagedir_translate(vm_pagedir_t* pagedir, u64 vaddr) {
  vm_pte_t pte = vm_pagedir_lookup_pte(pagedir, VM_VFN(vaddr));
  uintptr host_page_addr = (uintptr)(pte.outaddr << PAGE_SIZE_BITS);
  return host_page_addr + (uintptr)VM_ADDR_OFFSET(vaddr);
}


#ifdef VM_TRACE
  UNUSED static const char* fmtbits(const void* bits, usize len) {
    static char bufs[2][128];
    static int nbuf = 0;
    char* buf = bufs[nbuf++ % 2];
    assert(len <= sizeof(bufs[0]) - 1);
    buf += sizeof(bufs[0]) - 1 - len;
    memset(buf, '0', len);
    buf[len] = 0;
    for (usize i = 0; len--; i++)
      buf[i] = '0' + !!( ((u8*)bits)[len / 8] & (1lu << (len % 8)) );
    return buf;
  }
#endif


void vm_cache_init(vm_cache_t* cache) {
  memset(cache, 0xff, sizeof(vm_cache_t));
}


void vm_cache_invalidate(vm_cache_t* cache) {
  memset(cache, 0xff, sizeof(vm_cache_t));
}


// vm_cache_lookup looks up the host page address for a virtual address.
// Returns the host address, or 0 if the virtual page is not present in the cache.
static uintptr vm_cache_lookup(vm_cache_t* cache, u64 vaddr, u64 alignment) {
  u64 index = VM_VFN(vaddr) & VM_CACHE_INDEX_VFN_MASK;
  u64 actual_tag = cache->entries[index].tag;
  u64 expected_tag = vaddr & (VM_ADDR_PAGE_MASK ^ (alignment - 1llu));
  u64 is_valid = actual_tag == expected_tag;
  return (uintptr)(cache->entries[index].haddr_diff + vaddr) * is_valid;
}


// returns vm_cache_ent_t.haddr_diff
static u64 vm_cache_add(vm_cache_t* cache, u64 vpaddr, uintptr hpaddr) {
  assertf(IS_ALIGN2(vpaddr, PAGE_SIZE), "vpaddr not a page address 0x%llx", vpaddr);
  assertf(IS_ALIGN2(hpaddr, PAGE_SIZE), "hpaddr not a page address %p", (void*)hpaddr);

  vm_cache_ent_t* entry = VM_CACHE_ENTRY(cache, vpaddr);
  entry->haddr_diff = (u64)hpaddr - vpaddr;
  entry->tag = vpaddr;

  trace("%s 0x%llx => {.haddr_diff=0x%llx, .tag=0x%llx}",
    __FUNCTION__, vpaddr, entry->haddr_diff, entry->tag);

  return entry->haddr_diff;
}


// returns vm_cache_ent_t.haddr_diff
u64 _vm_cache_miss(vm_cache_t* cache, vm_pagedir_t* pagedir, u64 vaddr, vm_op_t op) {
  trace("%s 0x%llx op=0x%x", __FUNCTION__, vaddr, op);

  // check validity
  if UNLIKELY(VM_ADDR_MIN > vaddr || vaddr > VM_ADDR_MAX) {
    panic("invalid address 0x%llx (out of range)", vaddr); // FIXME
    return 0;
  }

  // check alignment
  if UNLIKELY(!IS_ALIGN2(vaddr, VM_OP_ALIGNMENT(op))) {
    const char* opname = VM_OP_TYPE(op) == VM_OP_LOAD ? "load from" : "store to";
    panic("misaligned %uB %s 0x%llx", VM_OP_ALIGNMENT(op), opname, vaddr); // FIXME
  }

  // get page table entry for the virtual page address (lookup via VFN)
  vm_pte_t pte = vm_pagedir_lookup_pte(pagedir, VM_VFN(vaddr));
  uintptr hpaddr = (uintptr)(pte.outaddr << PAGE_SIZE_BITS); // host page address

  trace("%s 0x%llx -> %p", __FUNCTION__, vaddr, (void*)hpaddr);

  // check if the lookup failed
  // TODO: Or is result=0 how "out of memory" is signalled?
  if UNLIKELY(hpaddr == 0) {
    trace("invalid address 0x%llx (vm_pagedir_lookup_pte failed)", vaddr);
    return 0;
  }

  // add to cache
  // TODO: in the future, check pte "uncacheable" bit here
  u64 vpaddr = VM_PAGE_ADDR(vaddr);
  return vm_cache_add(cache, vpaddr, hpaddr);
}


#if defined(VM_RUN_TEST_ON_INIT) && DEBUG
static void test_vm() {
  dlog("%s", __FUNCTION__);
  dlog("host pagesize:     %5u", (u32)os_pagesize());
  dlog("PAGE_SIZE:         %5u", PAGE_SIZE);
  dlog("PAGE_SIZE_BITS:    %5u", PAGE_SIZE_BITS);
  dlog("VM_ADDR_BITS:      %5u", VM_ADDR_BITS);
  dlog("VM_ADDR_MIN…MAX:   0x%llx … 0x%llx", VM_ADDR_MIN, VM_ADDR_MAX);
  dlog("VFN_BITS:       %5u", VFN_BITS);
  dlog("VM_PTAB_LEVELS:    %5u", VM_PTAB_LEVELS);
  dlog("VM_PTAB_BITS:      %5u", VM_PTAB_BITS);

  { // test the "store host page address diff in cache" logic
    const struct { u64 vaddr, hpage; } tests[] = {
      { 0xdeadbee4, 0x1065f0000 },
      { 0x1065f0000, 0xdeadbee4 },
    };
    for (usize i = 0; i < countof(tests); i++) {
      u64 vaddr = tests[i].vaddr;
      u64 hpage = tests[i].hpage;
      u64 vpage = vaddr & VM_ADDR_PAGE_MASK; // VM_PAGE_ADDR(vaddr)
      u64 diff = hpage - vpage;
      u64 haddr = diff + vaddr;
      u64 haddr_expected = hpage + VM_ADDR_OFFSET(vaddr);

      // log("vaddr  0x%-10llx %s", vaddr, fmtbits(&vaddr,64));
      // log("vpage  0x%-10llx %s", vpage, fmtbits(&vpage,64));
      // log("hpage  0x%-10llx %s", hpage, fmtbits(&hpage,64));
      // log("diff   0x%-10llx %s",  diff, fmtbits(&diff,64));
      // log("haddr  0x%-10llx %s", haddr, fmtbits(&haddr,64));
      // log("haddr′ 0x%-10llx %s", haddr_expected, fmtbits(&haddr_expected,64));

      assert(haddr == haddr_expected);
    }
  }

  // create a memory manager
  usize memsize = 4 * MiB;
  rmm_t* mm = assertnotnull( rmm_create_host_vmmap(memsize) );

  // create a page directory with memory manager
  vm_pagedir_t* pagedir = assertnotnull( vm_pagedir_create(mm) );

  // creat a translation cache
  vm_cache_t* cache = assertnotnull( rmm_allocpages(mm,
    ALIGN_CEIL(sizeof(vm_cache_t), PAGE_SIZE) / PAGE_SIZE) );
  vm_cache_init(cache);

  // { u64 vaddr = 0xdeadbeef;
  //   u64 vfn = vaddr_to_vfn(vaddr);
  //   dlog("—— vm_pagedir_lookup(addr 0x%llx, vfn 0x%llx) ——", vaddr, vfn);
  //   vm_pte_t pte = vm_pagedir_lookup_pte(pagedir, vfn);
  //   uintptr hpage = (uintptr)(pte.outaddr << PAGE_SIZE_BITS);
  //   uintptr haddr = hpage + (uintptr)vaddr_offs(vaddr);
  //   // dlog("=> PTE" PTE_FMT ", host page 0x%lx, host address 0x%lx",
  //   //   PTE_FMTARGS(pte), hpage, haddr);
  //   dlog("vaddr 0x%llx => host address 0x%lx (page 0x%lx)", vaddr, haddr, hpage);
  // }

  // make sure cache lookups work
  { u64 vaddr = 0xdeadbeef;
    assert(vm_cache_lookup(cache, vaddr, 1) == 0);
    vm_cache_add(cache, VM_PAGE_ADDR(vaddr), 0x1044f000);
    uintptr haddr = vm_cache_lookup(cache, vaddr, 1);
    //dlog("vm_cache_lookup(0x%llx) => %p", vaddr, (void*)haddr);
    assert(haddr == 0x1044feef);
    vm_cache_invalidate_one(cache, vaddr);
    assert(vm_cache_lookup(cache, vaddr, 1) == 0);
  }


  { // perform full real memory operations with virtual memory
    u64 vaddr = 0xdeadbee4;
    u32 value = 12345;
    dlog("VM_STORE(u32, 0x%llx, %u)", vaddr, value);
    VM_STORE(u32, cache, pagedir, vaddr, value);
    value = VM_LOAD(u32, cache, pagedir, vaddr);
    dlog("VM_LOAD(u32, 0x%llx) => %u", vaddr, value);
    value = VM_LOAD(u32, cache, pagedir, vaddr);
    dlog("VM_LOAD(u32, 0x%llx) => %u", vaddr, value);

    // loading an invalid address
    //VM_LOAD(u64, cache, pagedir, 0xffffffffffffffffllu);

    // loading the same address, which is 4B aligned, with a type of stronger
    // alignment will cause a cache miss and subsequent alignment error.
    // ——WIP—— (see vm_translate)
    //value = (u64)VM_LOAD(u64, cache, pagedir, vaddr);
  }

  // // allocate all pages (should panic just shy of rmm_avail_total pages)
  // for (u64 vaddr = VM_ADDR_MIN; vaddr <= VM_ADDR_MAX; vaddr += PAGE_SIZE) {
  //   u64 vfn = vaddr_to_vfn(vaddr);
  //   vm_pte_t pte = vm_pagedir_lookup_pte(pagedir, vfn);
  // }

  rmm_dispose(mm);
  dlog("—— end %s", __FUNCTION__);
}
#endif // VM_RUN_TEST_ON_INIT


rerr_t init_vmem() {
  #if defined(VM_RUN_TEST_ON_INIT) && DEBUG
  test_vm();
  #endif

  return 0;
}
