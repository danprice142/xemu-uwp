/*
 * Direct-mapped guest memory for the Xbox vCPU on Windows hosts.
 *
 * Guest accesses in the kernel MMU mode are compiled as a single host
 * access into a 4 GiB arena that mirrors the guest virtual address space.
 * Arena pages start as placeholders; the first access faults, and the
 * vectored handler maps a 4 KiB view of the guest RAM section there after
 * a side-effect free TLB probe. Accesses that need the softmmu (MMIO,
 * watchpoints, translated code, guest faults) patch their site into a jump
 * to the out-of-line slow path generated for every access.
 *
 * The arena is kept coherent with the softmmu TLB: page, range and full
 * flushes of the arena's MMU mode unmap pages, dirty-bit clears write
 * protect them, and watchpoint changes unmap them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/thread.h"
#include "qemu/atomic.h"
#include "hw/core/cpu.h"
#include "exec/cputlb.h"
#include "exec/tlb-flags.h"
#include "system/memory.h"
#include "accel/tcg/xbox-fastmem.h"
#include "exec/tb-flush.h"
#include "ui/xemu-settings.h"

#include <windows.h>
#include <winternl.h>
#include <intrin.h>

typedef PVOID (WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                        MEM_EXTENDED_PARAMETER *, ULONG);
typedef PVOID (WINAPI *MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64,
                                         SIZE_T, ULONG, ULONG,
                                         MEM_EXTENDED_PARAMETER *, ULONG);
typedef BOOL (WINAPI *UnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);
typedef HANDLE (WINAPI *CreateFileMappingWFn)(HANDLE, LPSECURITY_ATTRIBUTES,
                                              DWORD, DWORD, DWORD, LPCWSTR);
typedef BOOL (WINAPI *VirtualFreeFn)(LPVOID, SIZE_T, DWORD);
typedef BOOL (WINAPI *VirtualProtectFn)(LPVOID, SIZE_T, DWORD, PDWORD);
typedef PVOID (WINAPI *AddVectoredExceptionHandlerFn)(
    ULONG, PVECTORED_EXCEPTION_HANDLER);
typedef ULONG (WINAPI *RemoveVectoredExceptionHandlerFn)(PVOID);

static VirtualAlloc2Fn fm_VirtualAlloc2;
static MapViewOfFile3Fn fm_MapViewOfFile3;
static UnmapViewOfFile2Fn fm_UnmapViewOfFile2;
static CreateFileMappingWFn fm_CreateFileMappingW;
static VirtualFreeFn fm_VirtualFree;
static VirtualProtectFn fm_VirtualProtect;
static AddVectoredExceptionHandlerFn fm_AddVectoredExceptionHandler;
static RemoveVectoredExceptionHandlerFn fm_RemoveVectoredExceptionHandler;

/*
 * Image base of the module that implements an imported function. App builds
 * stub GetModuleHandle, so take it from the mapping of the import target.
 */
static const uint8_t *fm_module_of(const void *fn)
{
    MEMORY_BASIC_INFORMATION mbi;

    if (fn == NULL || VirtualQuery(fn, &mbi, sizeof(mbi)) == 0 ||
        mbi.AllocationBase == NULL) {
        return NULL;
    }
    return mbi.AllocationBase;
}

/* Export lookup by name through the PE export directory. */
static FARPROC fm_export(const uint8_t *base, const char *name)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS64 *nt;
    const IMAGE_DATA_DIRECTORY *dir;
    const IMAGE_EXPORT_DIRECTORY *exp;
    const uint32_t *names, *functions;
    const uint16_t *ordinals;

    if (base == NULL || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return NULL;
    }
    nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return NULL;
    }
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir->VirtualAddress == 0) {
        return NULL;
    }
    exp = (const IMAGE_EXPORT_DIRECTORY *)(base + dir->VirtualAddress);
    names = (const uint32_t *)(base + exp->AddressOfNames);
    functions = (const uint32_t *)(base + exp->AddressOfFunctions);
    ordinals = (const uint16_t *)(base + exp->AddressOfNameOrdinals);
    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        if (strcmp((const char *)(base + names[i]), name) == 0) {
            uint32_t rva = functions[ordinals[i]];

            /* Forwarded exports point back into the export directory. */
            if (rva >= dir->VirtualAddress &&
                rva < dir->VirtualAddress + dir->Size) {
                return NULL;
            }
            return (FARPROC)(base + rva);
        }
    }
    return NULL;
}

/*
 * Loaded module by base name, from the loader's in-load-order list (x64
 * PEB/LDR layout).
 */
static const uint8_t *fm_find_module(const wchar_t *name)
{
    const uint8_t *peb = (const uint8_t *)__readgsqword(0x60);
    const uint8_t *ldr = *(const uint8_t *const *)(peb + 0x18);
    const LIST_ENTRY *head = (const LIST_ENTRY *)(ldr + 0x10);

    for (const LIST_ENTRY *e = head->Flink; e != head; e = e->Flink) {
        const uint8_t *entry = (const uint8_t *)e;
        const UNICODE_STRING *base_name =
            (const UNICODE_STRING *)(entry + 0x58);
        const uint8_t *dll_base = *(const uint8_t *const *)(entry + 0x30);

        if (base_name->Buffer != NULL &&
            base_name->Length == wcslen(name) * sizeof(wchar_t) &&
            _wcsnicmp(base_name->Buffer, name, wcslen(name)) == 0) {
            return dll_base;
        }
    }
    return NULL;
}

static const uint8_t *fm_modules[3];

/*
 * The SDK maps these names onto the *FromApp variants in app builds; bind
 * the real exports of the modules that implement the memory API.
 */
static FARPROC fm_bind(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(fm_modules); i++) {
        FARPROC fn = fm_export(fm_modules[i], name);

        if (fn != NULL) {
            return fn;
        }
    }
    return NULL;
}

void *xbox_fastmem_bind_export(const char *name)
{
    if (fm_modules[1] == NULL) {
        xbox_fastmem_bind_platform();
    }
    return (void *)fm_bind(name);
}

bool xbox_fastmem_bind_platform(void)
{
    fm_modules[0] = fm_module_of((const void *)VirtualQuery);
    fm_modules[1] = fm_find_module(L"kernelbase.dll");
    fm_modules[2] = fm_find_module(L"ntdll.dll");
    fm_VirtualAlloc2 = (VirtualAlloc2Fn)fm_bind("VirtualAlloc2");
    fm_MapViewOfFile3 = (MapViewOfFile3Fn)fm_bind("MapViewOfFile3");
    fm_UnmapViewOfFile2 = (UnmapViewOfFile2Fn)fm_bind("UnmapViewOfFile2");
    fm_CreateFileMappingW =
        (CreateFileMappingWFn)fm_bind("CreateFileMappingW");
    fm_VirtualFree = (VirtualFreeFn)fm_bind("VirtualFree");
    fm_VirtualProtect = (VirtualProtectFn)fm_bind("VirtualProtect");
    fm_AddVectoredExceptionHandler = (AddVectoredExceptionHandlerFn)
        fm_bind("RtlAddVectoredExceptionHandler");
    fm_RemoveVectoredExceptionHandler = (RemoveVectoredExceptionHandlerFn)
        fm_bind("RtlRemoveVectoredExceptionHandler");
    return fm_VirtualAlloc2 && fm_MapViewOfFile3 && fm_UnmapViewOfFile2 &&
           fm_CreateFileMappingW && fm_VirtualFree && fm_VirtualProtect &&
           fm_AddVectoredExceptionHandler &&
           fm_RemoveVectoredExceptionHandler;
}

/* ------------------------------------------------------------------------ */

#define FM_PAGE_BITS    12
#define FM_PAGE_SIZE    (1u << FM_PAGE_BITS)
#define FM_ARENA_SIZE   (4ULL * GiB)
#define FM_PAGES        ((uint32_t)(FM_ARENA_SIZE >> FM_PAGE_BITS))
/* Accesses that start below 4 GiB may run past it; that tail faults. */
#define FM_GUARD_SIZE   (64 * KiB)
#define FM_NO_PAGE      UINT32_MAX

enum {
    FM_SPLIT = 1,       /* the page is its own 4 KiB placeholder */
    FM_MAPPED = 2,
    FM_WRITABLE = 4,
};

typedef struct FmRmapEntry {
    uint32_t vpage;
    uint32_t next;
} FmRmapEntry;

bool xbox_fastmem_enabled;
bool xbox_fastmem_active;
bool xbox_fastmem_codegen;
int xbox_fastmem_mmu_idx = -1;
uint8_t *xbox_fastmem_arena;
XboxFastmemStats xbox_fastmem_stats;

static HANDLE fm_process;
static HANDLE fm_section;
static uint8_t *fm_ram;
static uint64_t fm_ram_size;
static QemuMutex fm_lock;
/* Bumped whenever a mapping may have become stale; populate rechecks it. */
static uint32_t fm_gen;

static uint8_t *fm_state;          /* per arena page */
static uint32_t *fm_ram_page;      /* arena page -> RAM page */
static uint32_t *fm_rmap;          /* RAM page -> first rmap entry */
static FmRmapEntry *fm_rmap_pool;
static uint32_t fm_rmap_free = FM_NO_PAGE;
static uint32_t fm_rmap_used;
static uint32_t fm_rmap_cap;

static void fm_rmap_add(uint32_t ram_page, uint32_t vpage)
{
    uint32_t i;

    if (fm_rmap_free != FM_NO_PAGE) {
        i = fm_rmap_free;
        fm_rmap_free = fm_rmap_pool[i].next;
    } else {
        if (fm_rmap_used == fm_rmap_cap) {
            fm_rmap_cap = MAX(fm_rmap_cap * 2, 4096);
            fm_rmap_pool = g_renew(FmRmapEntry, fm_rmap_pool, fm_rmap_cap);
        }
        i = fm_rmap_used++;
    }
    fm_rmap_pool[i].vpage = vpage;
    fm_rmap_pool[i].next = fm_rmap[ram_page];
    fm_rmap[ram_page] = i;
}

static void fm_rmap_remove(uint32_t ram_page, uint32_t vpage)
{
    uint32_t *link = &fm_rmap[ram_page];

    while (*link != FM_NO_PAGE) {
        uint32_t i = *link;

        if (fm_rmap_pool[i].vpage == vpage) {
            *link = fm_rmap_pool[i].next;
            fm_rmap_pool[i].next = fm_rmap_free;
            fm_rmap_free = i;
            return;
        }
        link = &fm_rmap_pool[i].next;
    }
}

/* Diagnostic: what the host has at an arena page. */
static void fm_log_region(const char *what, uint32_t vpage, DWORD err)
{
    static unsigned budget = 32;
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *addr = xbox_fastmem_arena + ((uintptr_t)vpage << FM_PAGE_BITS);

    if (budget == 0) {
        return;
    }
    budget--;
    memset(&mbi, 0, sizeof(mbi));
    VirtualQuery(addr, &mbi, sizeof(mbi));
    fprintf(stderr, "xbox fastmem: %s vpage=0x%05x err=%lu state=%u "
            "region=%p+%zx alloc=%p mem_state=0x%lx type=0x%lx "
            "protect=0x%lx\n", what, vpage, err, fm_state[vpage],
            mbi.BaseAddress, (size_t)mbi.RegionSize, mbi.AllocationBase,
            mbi.State, mbi.Type, mbi.Protect);
}

/* Called with fm_lock held. */
static void fm_unmap_locked(uint32_t vpage)
{
    if (!(fm_state[vpage] & FM_MAPPED)) {
        return;
    }
    if (!fm_UnmapViewOfFile2(fm_process,
                             xbox_fastmem_arena +
                                 ((uintptr_t)vpage << FM_PAGE_BITS),
                             MEM_PRESERVE_PLACEHOLDER)) {
        fm_log_region("unmap failed", vpage, GetLastError());
    }
    fm_rmap_remove(fm_ram_page[vpage], vpage);
    fm_ram_page[vpage] = FM_NO_PAGE;
    fm_state[vpage] = FM_SPLIT;
    fm_gen++;
    qatomic_inc(&xbox_fastmem_stats.unmaps);
}

/* Called with fm_lock held. */
static void fm_write_protect_locked(uint32_t vpage)
{
    DWORD old;

    if ((fm_state[vpage] & (FM_MAPPED | FM_WRITABLE)) !=
        (FM_MAPPED | FM_WRITABLE)) {
        return;
    }
    if (!fm_VirtualProtect(xbox_fastmem_arena +
                               ((uintptr_t)vpage << FM_PAGE_BITS),
                           FM_PAGE_SIZE, PAGE_READONLY, &old)) {
        /* Never leave a writable mapping behind a cleared dirty bit. */
        fm_unmap_locked(vpage);
        return;
    }
    fm_state[vpage] &= ~FM_WRITABLE;
    fm_gen++;
    qatomic_inc(&xbox_fastmem_stats.protects);
}

/* Called with fm_lock held. */
static bool fm_map_locked(uint32_t vpage, uint32_t ram_page, bool writable)
{
    uint8_t *addr = xbox_fastmem_arena + ((uintptr_t)vpage << FM_PAGE_BITS);
    DWORD old;

    if ((fm_state[vpage] & FM_MAPPED) && fm_ram_page[vpage] != ram_page) {
        fm_unmap_locked(vpage);
    }
    if (fm_state[vpage] & FM_MAPPED) {
        if (writable && !(fm_state[vpage] & FM_WRITABLE)) {
            if (!fm_VirtualProtect(addr, FM_PAGE_SIZE, PAGE_READWRITE,
                                   &old)) {
                fm_unmap_locked(vpage);
                return false;
            }
            fm_state[vpage] |= FM_WRITABLE;
            qatomic_inc(&xbox_fastmem_stats.upgrades);
        }
        return true;
    }
    if (!(fm_state[vpage] & FM_SPLIT)) {
        if (!fm_VirtualFree(addr, FM_PAGE_SIZE,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            MEMORY_BASIC_INFORMATION mbi;
            DWORD err = GetLastError();

            /*
             * Splitting both neighbours leaves this page as a 4 KiB
             * placeholder of its own, which cannot be split again.
             */
            if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0 ||
                mbi.BaseAddress != addr || mbi.RegionSize != FM_PAGE_SIZE ||
                mbi.State != MEM_RESERVE) {
                fm_log_region("split failed", vpage, err);
                return false;
            }
        }
        fm_state[vpage] = FM_SPLIT;
    }
    /*
     * Views are mapped writable so that protection can be toggled later;
     * a read-only view cannot be raised to read/write.
     */
    if (fm_MapViewOfFile3(fm_section, fm_process, addr,
                          (ULONG64)ram_page << FM_PAGE_BITS, FM_PAGE_SIZE,
                          MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                          NULL, 0) != addr) {
        fm_log_region("map failed", vpage, GetLastError());
        return false;
    }
    if (!writable &&
        !fm_VirtualProtect(addr, FM_PAGE_SIZE, PAGE_READONLY, &old)) {
        fm_UnmapViewOfFile2(fm_process, addr, MEM_PRESERVE_PLACEHOLDER);
        return false;
    }
    fm_state[vpage] = FM_SPLIT | FM_MAPPED | (writable ? FM_WRITABLE : 0);
    fm_ram_page[vpage] = ram_page;
    fm_rmap_add(ram_page, vpage);
    qatomic_inc(&xbox_fastmem_stats.maps);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Access sites. Only the vCPU thread generates, commits, finds and patches. */

typedef struct FmSite {
    uintptr_t start;
    uintptr_t stub;
    uintptr_t end;
    int mmu_idx;
} FmSite;

static FmSite *fm_sites;
static size_t fm_sites_cap;         /* power of two */
static size_t fm_sites_used;
static FmSite *fm_stage;
static size_t fm_stage_used, fm_stage_cap;

static inline size_t fm_site_hash(uintptr_t start)
{
    return (size_t)(((uint64_t)start * 0x9E3779B97F4A7C15ull) >> 24);
}

static void fm_site_insert(const FmSite *site)
{
    size_t mask, i;

    if ((fm_sites_used + 1) * 2 > fm_sites_cap) {
        FmSite *old = fm_sites;
        size_t old_cap = fm_sites_cap;

        fm_sites_cap = MAX(old_cap * 2, (size_t)1 << 16);
        fm_sites = g_new0(FmSite, fm_sites_cap);
        fm_sites_used = 0;
        for (size_t j = 0; j < old_cap; j++) {
            if (old[j].start != 0) {
                fm_site_insert(&old[j]);
            }
        }
        g_free(old);
    }
    mask = fm_sites_cap - 1;
    for (i = fm_site_hash(site->start) & mask;
         fm_sites[i].start != 0 && fm_sites[i].start != site->start;
         i = (i + 1) & mask) {
    }
    if (fm_sites[i].start == 0) {
        fm_sites_used++;
    }
    fm_sites[i] = *site;
}

static FmSite *fm_site_lookup(uintptr_t start)
{
    size_t mask, i;

    if (fm_sites_cap == 0) {
        return NULL;
    }
    mask = fm_sites_cap - 1;
    for (i = fm_site_hash(start) & mask; fm_sites[i].start != 0;
         i = (i + 1) & mask) {
        if (fm_sites[i].start == start) {
            return &fm_sites[i];
        }
    }
    return NULL;
}

/* The faulting instruction may follow the site start in a sequence. */
static FmSite *fm_site_find(uintptr_t pc)
{
    for (uintptr_t d = 0; d < 64; d++) {
        FmSite *site = fm_site_lookup(pc - d);

        if (site != NULL) {
            return pc < site->end ? site : NULL;
        }
    }
    return NULL;
}

void xbox_fastmem_stage_reset(void)
{
    fm_stage_used = 0;
}

void xbox_fastmem_stage_site(void *start, const void *stub, const void *end,
                             int mmu_idx)
{
    if (fm_stage_used == fm_stage_cap) {
        fm_stage_cap = MAX(fm_stage_cap * 2, 256);
        fm_stage = g_renew(FmSite, fm_stage, fm_stage_cap);
    }
    fm_stage[fm_stage_used++] = (FmSite) {
        .start = (uintptr_t)start,
        .stub = (uintptr_t)stub,
        .end = (uintptr_t)end,
        .mmu_idx = mmu_idx,
    };
}

void xbox_fastmem_stage_commit(void)
{
    for (size_t i = 0; i < fm_stage_used; i++) {
        fm_site_insert(&fm_stage[i]);
    }
    qatomic_add(&xbox_fastmem_stats.sites, fm_stage_used);
    fm_stage_used = 0;
}

void xbox_fastmem_sites_reset(void)
{
    if (fm_sites != NULL) {
        memset(fm_sites, 0, fm_sites_cap * sizeof(*fm_sites));
    }
    fm_sites_used = 0;
    fm_stage_used = 0;
}

/* Route the site to its slow path for good: jmp rel32 to the stub. */
static void fm_patch(FmSite *site)
{
    uint8_t *p = (uint8_t *)site->start;
    int32_t rel = (int32_t)(site->stub - (site->start + 5));

    p[0] = 0xe9;
    memcpy(p + 1, &rel, 4);
    FlushInstructionCache(fm_process, p, 5);
    qatomic_inc(&xbox_fastmem_stats.patches);
}

/* ------------------------------------------------------------------------ */

enum {
    FM_OK,
    FM_INVALID,
    FM_MMIO,
    FM_WATCH,
    FM_NOT_RAM,
    FM_CODE,
    FM_READ_ONLY,
    FM_MAP_FAILED,
};

static unsigned fm_patch_log_budget = 64;
static void *fm_last_host;
static DWORD fm_last_error;

static int fm_flags_reason(int flags)
{
    if (flags & TLB_INVALID_MASK) {
        return FM_INVALID;
    }
    if (flags & TLB_WATCHPOINT) {
        return FM_WATCH;
    }
    return FM_MMIO;
}

static void fm_count_patch(int reason, uint32_t vaddr, bool is_write)
{
    static const char *const names[] = {
        "ok", "invalid", "mmio", "watch", "not_ram", "code", "read_only",
        "map_failed",
    };
    uint64_t *counters[] = {
        NULL,
        &xbox_fastmem_stats.patch_invalid,
        &xbox_fastmem_stats.patch_mmio,
        &xbox_fastmem_stats.patch_watch,
        &xbox_fastmem_stats.patch_not_ram,
        &xbox_fastmem_stats.patch_code,
        &xbox_fastmem_stats.patch_read_only,
        &xbox_fastmem_stats.patch_map_failed,
    };
    static unsigned rare_budget = 64;

    if (reason > FM_OK && reason < (int)ARRAY_SIZE(counters)) {
        qatomic_inc(counters[reason]);
    }
    if (fm_patch_log_budget > 0 ||
        (rare_budget > 0 && reason != FM_MMIO)) {
        if (fm_patch_log_budget > 0) {
            fm_patch_log_budget--;
        } else {
            rare_budget--;
        }
        fprintf(stderr, "xbox fastmem: patch vaddr=0x%08x %s %s host=%p "
                "ram=%p+%" PRIx64 " err=%lu\n", vaddr,
                is_write ? "write" : "read",
                reason < (int)ARRAY_SIZE(names) ? names[reason] : "?",
                fm_last_host, fm_ram, fm_ram_size, fm_last_error);
    }
}

/*
 * Map the guest page behind a faulting access if the softmmu would treat it
 * as plain RAM. Returns FM_OK when the access can be retried directly.
 */
static int fm_populate(FmSite *site, uint32_t vaddr, bool is_write)
{
    CPUState *cpu = current_cpu;
    uint32_t vpage = vaddr >> FM_PAGE_BITS;
    void *host;
    uint64_t ram_addr;
    uint32_t ram_page, gen;
    int rflags, wflags;
    bool writable, ok;

    if (cpu == NULL) {
        return FM_INVALID;
    }
    gen = qatomic_read(&fm_gen);
    rflags = tlb_fastmem_probe(cpu, vaddr, MMU_DATA_LOAD, site->mmu_idx,
                               &host, &ram_addr);
    fm_last_host = host;
    fm_last_error = 0;
    if (rflags != 0) {
        return fm_flags_reason(rflags);
    }
    if (host == NULL || (uint8_t *)host < fm_ram ||
        (uint64_t)((uint8_t *)host - fm_ram) >= fm_ram_size) {
        return FM_NOT_RAM;
    }
    ram_page = (uint32_t)(((uint8_t *)host - fm_ram) >> FM_PAGE_BITS);

    wflags = tlb_fastmem_probe(cpu, vaddr, MMU_DATA_STORE, site->mmu_idx,
                               &host, &ram_addr);
    writable = wflags == 0;
    if (wflags == TLB_NOTDIRTY && is_write) {
        /*
         * Clean page: record the write like notdirty_write would, unless
         * translated code lives there.
         */
        if (!tlb_fastmem_set_dirty(cpu, vaddr, ram_addr)) {
            return FM_CODE;
        }
        writable = true;
        qatomic_inc(&xbox_fastmem_stats.dirty);
    }
    if (is_write && !writable) {
        return wflags & TLB_INVALID_MASK ? FM_READ_ONLY :
               fm_flags_reason(wflags);
    }

    qemu_mutex_lock(&fm_lock);
    if (fm_gen != gen) {
        /*
         * An invalidation raced with the probe: let the access fault again
         * against the new state.
         */
        qemu_mutex_unlock(&fm_lock);
        return FM_OK;
    }
    ok = fm_map_locked(vpage, ram_page, writable);
    if (!ok) {
        fm_last_error = GetLastError();
    }
    qemu_mutex_unlock(&fm_lock);
    return ok ? FM_OK : FM_MAP_FAILED;
}

static LONG CALLBACK fm_fault_handler(EXCEPTION_POINTERS *info)
{
    EXCEPTION_RECORD *rec = info->ExceptionRecord;
    CONTEXT *ctx = info->ContextRecord;
    uintptr_t off;
    FmSite *site;
    bool is_write;

    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        rec->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    off = (uintptr_t)rec->ExceptionInformation[1] -
          (uintptr_t)xbox_fastmem_arena;
    if (off >= FM_ARENA_SIZE + FM_GUARD_SIZE) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    site = fm_site_find(ctx->Rip);
    if (site == NULL) {
        fprintf(stderr, "xbox fastmem: unhandled arena fault rip=%p "
                "addr=%p write=%d\n", (void *)ctx->Rip,
                (void *)rec->ExceptionInformation[1],
                (int)rec->ExceptionInformation[0]);
        fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    qatomic_inc(&xbox_fastmem_stats.faults);
    is_write = rec->ExceptionInformation[0] == 1;
    if (off >= FM_ARENA_SIZE) {
        qatomic_inc(&xbox_fastmem_stats.patch_guard);
    } else {
        int reason = fm_populate(site, (uint32_t)off, is_write);

        if (reason == FM_OK) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        fm_count_patch(reason, (uint32_t)off, is_write);
    }
    fm_patch(site);
    ctx->Rip = site->start;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* ------------------------------------------------------------------------ */

static bool fm_flag_file_set(void)
{
    const char *base = xemu_settings_get_base_path();
    g_autofree char *path = NULL;
    g_autofree gchar *text = NULL;

    if (base == NULL) {
        return false;
    }
    path = g_strdup_printf("%sfastmem.txt", base);
    return g_file_get_contents(path, &text, NULL, NULL) && text[0] == '1';
}

bool xbox_fastmem_init(void)
{
    static bool done;

    if (done) {
        return xbox_fastmem_enabled;
    }
    done = true;
    if (!fm_flag_file_set()) {
        return false;
    }
    if (!xbox_fastmem_bind_platform()) {
        fprintf(stderr, "xbox fastmem: host API unavailable\n");
        return false;
    }
    fm_process = GetCurrentProcess();
    xbox_fastmem_arena = fm_VirtualAlloc2(fm_process, NULL,
                                          FM_ARENA_SIZE + FM_GUARD_SIZE,
                                          MEM_RESERVE |
                                              MEM_RESERVE_PLACEHOLDER,
                                          PAGE_NOACCESS, NULL, 0);
    if (xbox_fastmem_arena == NULL) {
        fprintf(stderr, "xbox fastmem: arena reservation failed (%lu)\n",
                GetLastError());
        return false;
    }
    /* Keep the guard tail outside the page placeholders. */
    fm_VirtualFree(xbox_fastmem_arena + FM_ARENA_SIZE, FM_GUARD_SIZE,
                   MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    fm_state = g_malloc0(FM_PAGES);
    fm_ram_page = g_malloc(FM_PAGES * sizeof(*fm_ram_page));
    memset(fm_ram_page, 0xff, FM_PAGES * sizeof(*fm_ram_page));
    qemu_mutex_init(&fm_lock);
    if (fm_AddVectoredExceptionHandler(1, fm_fault_handler) == NULL) {
        fprintf(stderr, "xbox fastmem: handler registration failed\n");
        return false;
    }
    xbox_fastmem_enabled = true;
    fprintf(stderr, "xbox fastmem: arena %p\n", xbox_fastmem_arena);
    return true;
}

void *xbox_fastmem_alloc_ram(uint64_t size)
{
    void *ram;

    if (!xbox_fastmem_enabled || size == 0) {
        return NULL;
    }
    fm_section = fm_CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
                                       PAGE_READWRITE, (DWORD)(size >> 32),
                                       (DWORD)size, NULL);
    if (fm_section == NULL) {
        fprintf(stderr, "xbox fastmem: RAM section failed (%lu)\n",
                GetLastError());
        return NULL;
    }
    ram = fm_MapViewOfFile3(fm_section, fm_process, NULL, 0, size, 0,
                            PAGE_READWRITE, NULL, 0);
    if (ram == NULL) {
        fprintf(stderr, "xbox fastmem: RAM view failed (%lu)\n",
                GetLastError());
        CloseHandle(fm_section);
        fm_section = NULL;
        return NULL;
    }
    return ram;
}

void xbox_fastmem_set_ram(void *ram, uint64_t size, int mmu_idx)
{
    uint32_t pages = (uint32_t)(size >> FM_PAGE_BITS);

    fm_ram = ram;
    fm_ram_size = size;
    fm_rmap = g_malloc(pages * sizeof(*fm_rmap));
    memset(fm_rmap, 0xff, pages * sizeof(*fm_rmap));
    xbox_fastmem_mmu_idx = mmu_idx;
    qatomic_set(&xbox_fastmem_active, true);
    qatomic_set(&xbox_fastmem_codegen, true);
    fprintf(stderr, "xbox fastmem: active, RAM %p size %" PRIu64
            " mmu_idx %d\n", ram, size, mmu_idx);
}

void xbox_fastmem_set_codegen(bool on)
{
    if (!qatomic_read(&xbox_fastmem_active) ||
        qatomic_read(&xbox_fastmem_codegen) == on || first_cpu == NULL) {
        return;
    }
    qatomic_set(&xbox_fastmem_codegen, on);
    fm_patch_log_budget = 64;
    queue_tb_flush(first_cpu);
    fprintf(stderr, "xbox fastmem: direct accesses %s\n", on ? "on" : "off");
}

bool xbox_tb_inline_lookup = true;

void xbox_tb_set_inline_lookup(bool on)
{
    if (qatomic_read(&xbox_tb_inline_lookup) == on || first_cpu == NULL) {
        return;
    }
    qatomic_set(&xbox_tb_inline_lookup, on);
    queue_tb_flush(first_cpu);
    fprintf(stderr, "xbox tb: inline jump cache lookup %s\n",
            on ? "on" : "off");
}

bool xbox_sse_native = true;
bool xbox_io_rewind = true;
bool xbox_diag_log;

void xbox_set_sse_native(bool on)
{
    if (qatomic_read(&xbox_sse_native) == on || first_cpu == NULL) {
        return;
    }
    qatomic_set(&xbox_sse_native, on);
    queue_tb_flush(first_cpu);
    fprintf(stderr, "xbox tb: native SSE arithmetic %s\n", on ? "on" : "off");
}

/* ------------------------------------------------------------------------ */
/* Coherence with the softmmu TLB. */

void xbox_fastmem_flush_page(uint64_t vaddr)
{
    if (!qatomic_read(&xbox_fastmem_active) || vaddr >= FM_ARENA_SIZE) {
        return;
    }
    qemu_mutex_lock(&fm_lock);
    fm_unmap_locked((uint32_t)(vaddr >> FM_PAGE_BITS));
    qemu_mutex_unlock(&fm_lock);
}

void xbox_fastmem_flush_range(uint64_t vaddr, uint64_t len)
{
    uint64_t end = MIN(vaddr + len, FM_ARENA_SIZE);

    if (!qatomic_read(&xbox_fastmem_active)) {
        return;
    }
    qemu_mutex_lock(&fm_lock);
    for (uint64_t a = vaddr & ~(uint64_t)(FM_PAGE_SIZE - 1); a < end;
         a += FM_PAGE_SIZE) {
        fm_unmap_locked((uint32_t)(a >> FM_PAGE_BITS));
    }
    qemu_mutex_unlock(&fm_lock);
}

void xbox_fastmem_flush_all(void)
{
    uint32_t pages;

    if (!qatomic_read(&xbox_fastmem_active)) {
        return;
    }
    pages = (uint32_t)(fm_ram_size >> FM_PAGE_BITS);
    qemu_mutex_lock(&fm_lock);
    for (uint32_t p = 0; p < pages; p++) {
        uint32_t i = fm_rmap[p];

        while (i != FM_NO_PAGE) {
            uint32_t next = fm_rmap_pool[i].next;

            fm_unmap_locked(fm_rmap_pool[i].vpage);
            i = next;
        }
    }
    fm_gen++;
    qemu_mutex_unlock(&fm_lock);
    qatomic_inc(&xbox_fastmem_stats.flush_all);
}

/* Applies @fn to every arena page mapping RAM in [host, host + len). */
static void fm_for_host_range(uintptr_t host, uintptr_t len,
                              void (*fn)(uint32_t vpage))
{
    uintptr_t base = (uintptr_t)fm_ram;
    uintptr_t start, end;

    if (!qatomic_read(&xbox_fastmem_active) || host + len <= base ||
        host >= base + fm_ram_size) {
        return;
    }
    start = MAX(host, base) - base;
    end = MIN(host + len, base + fm_ram_size) - base;
    qemu_mutex_lock(&fm_lock);
    for (uintptr_t p = start >> FM_PAGE_BITS;
         p < (end + FM_PAGE_SIZE - 1) >> FM_PAGE_BITS; p++) {
        uint32_t i = fm_rmap[p];

        while (i != FM_NO_PAGE) {
            uint32_t next = fm_rmap_pool[i].next;

            fn(fm_rmap_pool[i].vpage);
            i = next;
        }
    }
    qemu_mutex_unlock(&fm_lock);
}

void xbox_fastmem_write_protect_host(uintptr_t host, uintptr_t len)
{
    fm_for_host_range(host, len, fm_write_protect_locked);
}

void xbox_fastmem_unmap_host(uintptr_t host, uintptr_t len)
{
    fm_for_host_range(host, len, fm_unmap_locked);
}

/* ------------------------------------------------------------------------ */

static volatile uint8_t *probe_fault_page;
static volatile LONG probe_faults;

static LONG CALLBACK probe_fault_handler(EXCEPTION_POINTERS *info)
{
    EXCEPTION_RECORD *rec = info->ExceptionRecord;
    DWORD old;

    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        rec->NumberParameters < 2 || probe_fault_page == NULL ||
        (uintptr_t)rec->ExceptionInformation[1] -
            (uintptr_t)probe_fault_page >= 4096) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    InterlockedIncrement(&probe_faults);
    fm_VirtualProtect((void *)probe_fault_page, 4096, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static double probe_us(LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER freq,
                       int n)
{
    return (double)(b.QuadPart - a.QuadPart) * 1e6 / freq.QuadPart / n;
}

void xbox_fastmem_platform_probe(void)
{
    const SIZE_T section_size = 64 * MiB;
    const SIZE_T arena_size = 4 * GiB;
    const uintptr_t vpage = 0x80003000;
    const ULONG64 offset = 0x5000;
    HANDLE process = GetCurrentProcess();
    HANDLE section = NULL;
    uint8_t *primary = NULL;
    uint8_t *arena = NULL;
    uint8_t *view = NULL;
    LARGE_INTEGER freq, t0, t1;
    PVOID handler = NULL;
    double map_us = 0, protect_us = 0, fault_us = 0;
    int n = 1000;
    bool alias_ok = false;

    QueryPerformanceFrequency(&freq);
    if (!xbox_fastmem_bind_platform()) {
        fprintf(stderr, "xbox fastmem probe: bind failed (modules %p %p; "
                "VirtualAlloc2=%p MapViewOfFile3=%p UnmapViewOfFile2=%p "
                "CreateFileMappingW=%p VirtualFree=%p VirtualProtect=%p "
                "AddVEH=%p)\n",
                fm_modules[1], fm_modules[2], fm_VirtualAlloc2,
                fm_MapViewOfFile3, fm_UnmapViewOfFile2, fm_CreateFileMappingW,
                fm_VirtualFree, fm_VirtualProtect,
                fm_AddVectoredExceptionHandler);
        return;
    }

    section = fm_CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
                                    PAGE_READWRITE, 0, section_size, NULL);
    if (section == NULL) {
        fprintf(stderr, "xbox fastmem probe: section failed (%lu)\n",
                GetLastError());
        return;
    }
    primary = fm_MapViewOfFile3(section, process, NULL, 0, section_size, 0,
                                PAGE_READWRITE, NULL, 0);
    arena = fm_VirtualAlloc2(process, NULL, arena_size,
                             MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                             PAGE_NOACCESS, NULL, 0);
    if (primary == NULL || arena == NULL) {
        fprintf(stderr, "xbox fastmem probe: primary=%p arena=%p (%lu)\n",
                primary, arena, GetLastError());
        goto out;
    }

    /* Carve a 4 KiB placeholder at a page that is not 64 KiB aligned. */
    if (!fm_VirtualFree(arena + vpage, 4096,
                        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        fprintf(stderr, "xbox fastmem probe: split failed (%lu)\n",
                GetLastError());
        goto out;
    }
    view = fm_MapViewOfFile3(section, process, arena + vpage, offset, 4096,
                             MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    if (view != arena + vpage) {
        fprintf(stderr, "xbox fastmem probe: 4 KiB view failed view=%p (%lu)\n",
                view, GetLastError());
        goto out;
    }
    primary[offset + 8] = 0x5a;
    view[16] = 0xa5;
    alias_ok = view[8] == 0x5a && primary[offset + 16] == 0xa5;

    QueryPerformanceCounter(&t0);
    for (int i = 0; i < n; i++) {
        fm_UnmapViewOfFile2(process, view, MEM_PRESERVE_PLACEHOLDER);
        view = fm_MapViewOfFile3(section, process, arena + vpage,
                                 offset + (i & 15) * 4096, 4096,
                                 MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                 NULL, 0);
        if (view == NULL) {
            break;
        }
    }
    QueryPerformanceCounter(&t1);
    map_us = probe_us(t0, t1, freq, n);

    QueryPerformanceCounter(&t0);
    for (int i = 0; i < n; i++) {
        DWORD old;

        fm_VirtualProtect(view, 4096, (i & 1) ? PAGE_READWRITE :
                          PAGE_READONLY, &old);
    }
    QueryPerformanceCounter(&t1);
    protect_us = probe_us(t0, t1, freq, n);

    handler = fm_AddVectoredExceptionHandler(1, probe_fault_handler);
    probe_fault_page = view;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < n; i++) {
        DWORD old;

        fm_VirtualProtect(view, 4096, PAGE_NOACCESS, &old);
        view[32] = (uint8_t)i;
    }
    QueryPerformanceCounter(&t1);
    fault_us = probe_us(t0, t1, freq, n) - 2 * protect_us;
    probe_fault_page = NULL;

out:
    fprintf(stderr,
            "xbox fastmem probe: arena=%p primary=%p view=%p alias=%d "
            "handler=%p faults=%ld map+unmap=%.2fus protect=%.2fus "
            "fault=%.2fus\n",
            arena, primary, view, alias_ok, handler, probe_faults, map_us,
            protect_us, fault_us);
    if (handler != NULL) {
        fm_RemoveVectoredExceptionHandler(handler);
    }
    if (view != NULL) {
        fm_UnmapViewOfFile2(process, view, MEM_PRESERVE_PLACEHOLDER);
    }
    if (arena != NULL) {
        fm_VirtualFree(arena, 0, MEM_RELEASE);
    }
    if (primary != NULL) {
        UnmapViewOfFile(primary);
    }
    if (section != NULL) {
        CloseHandle(section);
    }
}
