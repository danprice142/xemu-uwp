/*
 * Sampling profiler for the vCPU thread on Windows hosts.
 *
 * A sampler thread suspends the vCPU thread at a fixed interval, records
 * its instruction pointer and resumes it. Nothing that could take a lock
 * runs while the vCPU is suspended. After the window the samples are
 * classified: translated code (by translation block), the executable (by
 * RVA, symbolized offline) or another module, and logged.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/atomic.h"
#include "tcg/tcg.h"
#include "exec/translation-block.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "accel/tcg/xbox-fastmem.h"

#include <windows.h>
#include <winternl.h>
#include <intrin.h>

typedef DWORD (WINAPI *SuspendThreadFn)(HANDLE);
typedef DWORD (WINAPI *ResumeThreadFn)(HANDLE);
typedef BOOL (WINAPI *GetThreadContextFn)(HANDLE, LPCONTEXT);
typedef HANDLE (WINAPI *CreateWaitableTimerExWFn)(LPSECURITY_ATTRIBUTES,
                                                  LPCWSTR, DWORD, DWORD);
typedef BOOL (WINAPI *SetWaitableTimerFn)(HANDLE, const LARGE_INTEGER *, LONG,
                                          PTIMERAPCROUTINE, LPVOID, BOOL);

static SuspendThreadFn prof_SuspendThread;
static ResumeThreadFn prof_ResumeThread;
static GetThreadContextFn prof_GetThreadContext;
static CreateWaitableTimerExWFn prof_CreateWaitableTimerExW;
static SetWaitableTimerFn prof_SetWaitableTimer;

/* The translation buffer, for classifying samples (tcg/tcg.c). */
extern TCGContext tcg_init_ctx;

static HANDLE prof_vcpu;
static bool prof_running;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

void xbox_prof_register_vcpu(void)
{
    HANDLE process = GetCurrentProcess();

    if (prof_vcpu == NULL) {
        DuplicateHandle(process, GetCurrentThread(), process, &prof_vcpu, 0,
                        FALSE, DUPLICATE_SAME_ACCESS);
    }
}

/* Module containing @addr, and its base name from the loader list. */
static const uint8_t *prof_module(uintptr_t addr, char *name, size_t len)
{
    MEMORY_BASIC_INFORMATION mbi;
    const uint8_t *peb = (const uint8_t *)__readgsqword(0x60);
    const uint8_t *ldr = *(const uint8_t *const *)(peb + 0x18);
    const LIST_ENTRY *head = (const LIST_ENTRY *)(ldr + 0x10);

    snprintf(name, len, "?");
    if (VirtualQuery((void *)addr, &mbi, sizeof(mbi)) == 0) {
        return NULL;
    }
    for (const LIST_ENTRY *e = head->Flink; e != head; e = e->Flink) {
        const uint8_t *entry = (const uint8_t *)e;
        const UNICODE_STRING *base_name =
            (const UNICODE_STRING *)(entry + 0x58);

        if (*(void *const *)(entry + 0x30) == mbi.AllocationBase &&
            base_name->Buffer != NULL) {
            int n = MIN((int)(base_name->Length / sizeof(wchar_t)),
                        (int)len - 1);

            for (int i = 0; i < n; i++) {
                name[i] = (char)base_name->Buffer[i];
            }
            name[n] = 0;
            return mbi.AllocationBase;
        }
    }
    snprintf(name, len, "anon@%p", mbi.AllocationBase);
    return mbi.AllocationBase;
}

typedef struct ProfCount {
    uintptr_t key;
    uint64_t phys_pc;           /* translation blocks only */
    uint32_t host_size;         /* translation blocks only */
    uint32_t count;
} ProfCount;

static int prof_cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

    return x < y ? -1 : x > y;
}

static int prof_cmp_count(const void *a, const void *b)
{
    const ProfCount *x = a, *y = b;

    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

typedef struct ProfExport {
    uint32_t rva;
    const char *name;
} ProfExport;

static int prof_cmp_export(const void *a, const void *b)
{
    const ProfExport *x = a, *y = b;

    return x->rva < y->rva ? -1 : x->rva > y->rva;
}

/* Named exports of a mapped image, sorted by address; forwarders skipped. */
static ProfExport *prof_exports(const uint8_t *base, size_t *count)
{
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)
        (base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const IMAGE_EXPORT_DIRECTORY *exp;
    const DWORD *funcs, *names;
    const WORD *ords;
    ProfExport *out;
    size_t n = 0;

    *count = 0;
    if (dir->VirtualAddress == 0 || dir->Size == 0) {
        return NULL;
    }
    exp = (const IMAGE_EXPORT_DIRECTORY *)(base + dir->VirtualAddress);
    funcs = (const DWORD *)(base + exp->AddressOfFunctions);
    names = (const DWORD *)(base + exp->AddressOfNames);
    ords = (const WORD *)(base + exp->AddressOfNameOrdinals);
    out = g_new(ProfExport, exp->NumberOfNames + 1);
    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        uint32_t rva = funcs[ords[i]];

        if (rva >= dir->VirtualAddress &&
            rva < dir->VirtualAddress + dir->Size) {
            continue;
        }
        out[n].rva = rva;
        out[n].name = (const char *)(base + names[i]);
        n++;
    }
    qsort(out, n, sizeof(*out), prof_cmp_export);
    *count = n;
    return out;
}

/*
 * Logs the samples of one module by nearest preceding export, with the
 * offset range seen past it (large offsets mean an unexported function).
 */
static void prof_report_exports(const char *id, const char *mod,
                                const uint8_t *base, const uint64_t *rips,
                                const uint8_t *const *bases, size_t n)
{
    size_t nexp;
    ProfExport *exps = prof_exports(base, &nexp);
    uint32_t *counts, *off_min, *off_max;
    size_t shown = 0;

    if (exps == NULL || nexp == 0) {
        g_free(exps);
        return;
    }
    counts = g_new0(uint32_t, nexp);
    off_min = g_new(uint32_t, nexp);
    off_max = g_new0(uint32_t, nexp);
    memset(off_min, 0xff, nexp * sizeof(*off_min));
    for (size_t i = 0; i < n; i++) {
        uint32_t rva;
        size_t lo = 0, hi = nexp;

        if (bases[i] != base) {
            continue;
        }
        rva = (uint32_t)(rips[i] - (uintptr_t)base);
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;

            if (exps[mid].rva <= rva) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        if (exps[lo].rva > rva) {
            continue;
        }
        counts[lo]++;
        off_min[lo] = MIN(off_min[lo], rva - exps[lo].rva);
        off_max[lo] = MAX(off_max[lo], rva - exps[lo].rva);
    }
    while (shown < 16) {
        size_t best = nexp;

        for (size_t i = 0; i < nexp; i++) {
            if (counts[i] != 0 &&
                (best == nexp || counts[i] > counts[best])) {
                best = i;
            }
        }
        if (best == nexp) {
            break;
        }
        fprintf(stderr, "xbox prof %s sym %s!%s %u off=0x%x-0x%x\n", id, mod,
                exps[best].name, counts[best], off_min[best], off_max[best]);
        counts[best] = 0;
        shown++;
    }
    g_free(counts);
    g_free(off_min);
    g_free(off_max);
    g_free(exps);
}

/* Adds @key to a small open-addressed table. */
static ProfCount *prof_bump(ProfCount *table, size_t cap, uintptr_t key)
{
    size_t i = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1);

    while (table[i].count != 0 && table[i].key != key) {
        i = (i + 1) & (cap - 1);
    }
    table[i].key = key;
    table[i].count++;
    return &table[i];
}

static void prof_report(const char *id, uint64_t *rips, size_t n,
                        double seconds)
{
    const size_t cap = 1 << 16;
    ProfCount *tbs = g_new0(ProfCount, cap);
    ProfCount *mods = g_new0(ProfCount, 256);
    char (*mod_names)[64] = g_malloc0(256 * 64);
    uint64_t *other_rips = g_new(uint64_t, n);
    const uint8_t **other_bases = g_new(const uint8_t *, n);
    size_t n_other = 0;
    const uint8_t *exe;
    size_t exe_size;
    uint64_t jit = 0, exe_samples = 0, other = 0;
    char name[64];
    GString *line = g_string_new("");
    MEMORY_BASIC_INFORMATION mbi;

    VirtualQuery((void *)prof_report, &mbi, sizeof(mbi));
    exe = mbi.AllocationBase;
    exe_size = ((const IMAGE_NT_HEADERS64 *)
                (exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew))
                   ->OptionalHeader.SizeOfImage;

    qsort(rips, n, sizeof(*rips), prof_cmp_u64);
    for (size_t i = 0; i < n; i++) {
        uintptr_t rip = rips[i];
        TranslationBlock *tb;

        if (rip - (uintptr_t)exe < exe_size) {
            exe_samples++;
            continue;
        }
        tb = tcg_tb_lookup(rip);
        if (tb != NULL) {
            ProfCount *c = prof_bump(tbs, cap, (uintptr_t)tb);

            c->phys_pc = tb_page_addr0(tb);
            c->host_size = (uint32_t)tb->tc.size;
            jit++;
            continue;
        }
        other++;
        {
            const uint8_t *base = prof_module(rip, name, sizeof(name));
            ProfCount *c = prof_bump(mods, 256, (uintptr_t)base);

            other_rips[n_other] = rip;
            other_bases[n_other++] = base;

            if (mod_names[c - mods][0] == 0) {
                g_strlcpy(mod_names[c - mods], name, 64);
            }
        }
    }

    fprintf(stderr, "xbox prof %s: %.1fs samples=%zu jit=%" PRIu64
            " exe=%" PRIu64 " other=%" PRIu64 " exe_base=%p\n", id, seconds,
            n, jit, exe_samples, other, exe);

    /* Executable RVAs with counts, symbolized offline. */
    for (size_t i = 0; i < n;) {
        uintptr_t rip = rips[i];
        size_t j = i;

        while (j < n && rips[j] == rip) {
            j++;
        }
        if (rip - (uintptr_t)exe < exe_size) {
            g_string_append_printf(line, " %zx:%zu", rip - (uintptr_t)exe,
                                   j - i);
            if (line->len > 900) {
                fprintf(stderr, "xbox prof %s exe%s\n", id, line->str);
                g_string_truncate(line, 0);
            }
        }
        i = j;
    }
    if (line->len) {
        fprintf(stderr, "xbox prof %s exe%s\n", id, line->str);
    }

    qsort(mods, 256, sizeof(*mods), prof_cmp_count);
    for (size_t i = 0; i < 256 && mods[i].count; i++) {
        prof_module(mods[i].key, name, sizeof(name));
        fprintf(stderr, "xbox prof %s mod %s %u\n", id, name, mods[i].count);
        if (mods[i].count >= MAX(20, n / 100) &&
            strncmp(name, "anon@", 5) != 0) {
            prof_report_exports(id, name, (const uint8_t *)mods[i].key,
                                other_rips, other_bases, n_other);
        }
    }

    qsort(tbs, cap, sizeof(*tbs), prof_cmp_count);
    for (size_t i = 0; i < 60 && tbs[i].count; i++) {
        const TranslationBlock *tb = (const TranslationBlock *)tbs[i].key;
        uint8_t code[12] = { 0 };
        char hex[sizeof(code) * 2 + 1];

        address_space_read(&address_space_memory, tbs[i].phys_pc,
                           MEMTXATTRS_UNSPECIFIED, code, sizeof(code));
        for (size_t k = 0; k < sizeof(code); k++) {
            snprintf(hex + k * 2, 3, "%02x", code[k]);
        }
        fprintf(stderr, "xbox prof %s tb phys=0x%08" PRIx64 " host=%u n=%u"
                " insns=%u size=%u cflags=0x%x flags=0x%x code=%s\n",
                id, tbs[i].phys_pc, tbs[i].host_size, tbs[i].count,
                tb->icount, tb->size, tb_cflags(tb), tb->flags, hex);
    }
    g_string_free(line, true);
    g_free(tbs);
    g_free(mods);
    g_free(mod_names);
    g_free(other_rips);
    g_free(other_bases);
}

typedef struct ProfJob {
    char id[32];
    int seconds;
} ProfJob;

/*
 * A sample outside the executable and translated code, with the first
 * return addresses into executable code found on its stack.
 */
#define PROF_CALLERS 6

typedef struct ProfStack {
    uint64_t rip;
    uint64_t callers[PROF_CALLERS];
} ProfStack;

typedef struct ProfStackKey {
    const uint8_t *base;
    const char *name;
    uint64_t callers[PROF_CALLERS];
    uint32_t count;
} ProfStackKey;

static int prof_cmp_stack_key(const void *a, const void *b)
{
    const ProfStackKey *x = a, *y = b;

    if (x->name != y->name) {
        return x->name < y->name ? -1 : 1;
    }
    return memcmp(x->callers, y->callers, sizeof(x->callers));
}

static int prof_cmp_stack_count(const void *a, const void *b)
{
    const ProfStackKey *x = a, *y = b;

    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

/* Logs the most frequent (module export, callers) chains. */
static void prof_report_stacks(const char *id, const ProfStack *st, size_t n,
                               const uint8_t *exe)
{
    ProfStackKey *keys = g_new0(ProfStackKey, n + 1);
    size_t nkeys = 0, out = 0;
    const uint8_t *cur_base = NULL;
    ProfExport *exps = NULL;
    size_t nexp = 0;
    char mod[64];

    for (size_t i = 0; i < n; i++) {
        const uint8_t *base = prof_module(st[i].rip, mod, sizeof(mod));
        ProfStackKey *k = &keys[nkeys++];

        if (base != cur_base) {
            g_free(exps);
            exps = base != NULL && strncmp(mod, "anon@", 5) != 0 ?
                prof_exports(base, &nexp) : NULL;
            cur_base = base;
        }
        k->base = base;
        k->name = "?";
        if (exps != NULL && nexp != 0) {
            uint32_t rva = (uint32_t)(st[i].rip - (uintptr_t)base);
            size_t lo = 0, hi = nexp;

            while (hi - lo > 1) {
                size_t mid = (lo + hi) / 2;

                if (exps[mid].rva <= rva) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            if (exps[lo].rva <= rva) {
                k->name = exps[lo].name;
            }
        }
        for (int c = 0; c < PROF_CALLERS; c++) {
            k->callers[c] = st[i].callers[c] ?
                st[i].callers[c] - (uintptr_t)exe : 0;
        }
        k->count = 1;
    }
    g_free(exps);
    qsort(keys, nkeys, sizeof(*keys), prof_cmp_stack_key);
    for (size_t i = 0; i < nkeys;) {
        size_t j = i + 1;

        while (j < nkeys && prof_cmp_stack_key(&keys[i], &keys[j]) == 0) {
            j++;
        }
        keys[out] = keys[i];
        keys[out++].count = j - i;
        i = j;
    }
    qsort(keys, out, sizeof(*keys), prof_cmp_stack_count);
    for (size_t i = 0; i < out && i < 48; i++) {
        GString *chain = g_string_new("");

        for (int c = 0; c < PROF_CALLERS; c++) {
            g_string_append_printf(chain, " %" PRIx64, keys[i].callers[c]);
        }
        prof_module((uintptr_t)keys[i].base, mod, sizeof(mod));
        fprintf(stderr, "xbox prof %s stack %s!%s %u%s\n", id, mod,
                keys[i].name, keys[i].count, chain->str);
        g_string_free(chain, true);
    }
    g_free(keys);
}

static void *prof_thread(void *opaque)
{
    ProfJob *job = opaque;
    size_t cap = (size_t)job->seconds * 5000;
    uint64_t *rips = g_new(uint64_t, cap);
    ProfStack *stacks = g_new0(ProfStack, cap);
    size_t n = 0, nstacks = 0;
    MEMORY_BASIC_INFORMATION mbi;
    const uint8_t *exe, *text = NULL;
    size_t exe_size, text_size = 0;
    uintptr_t jit = (uintptr_t)tcg_init_ctx.code_gen_buffer;
    size_t jit_size = tcg_init_ctx.code_gen_buffer_size;
    HANDLE timer = prof_CreateWaitableTimerExW ?
        prof_CreateWaitableTimerExW(NULL, NULL,
                                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS) : NULL;
    int64_t start = g_get_monotonic_time();
    int64_t end = start + (int64_t)job->seconds * 1000000;
    LARGE_INTEGER due = { .QuadPart = -2500 };   /* 250 us */

    VirtualQuery((void *)prof_thread, &mbi, sizeof(mbi));
    exe = mbi.AllocationBase;
    {
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)
            (exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
        const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);

        exe_size = nt->OptionalHeader.SizeOfImage;
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            if (memcmp(sec[i].Name, ".text", 6) == 0) {
                text = exe + sec[i].VirtualAddress;
                text_size = sec[i].Misc.VirtualSize;
            }
        }
    }

    while (g_get_monotonic_time() < end && n < cap) {
        CONTEXT ctx __attribute__((aligned(16)));

        if (timer != NULL &&
            prof_SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }
        if (prof_SuspendThread(prof_vcpu) == (DWORD)-1) {
            continue;
        }
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (prof_GetThreadContext(prof_vcpu, &ctx)) {
            rips[n++] = ctx.Rip;
            /*
             * Outside the executable and translated code: keep the first
             * return addresses into the executable's code on the stack.
             * Nothing here may take a lock the vCPU could hold.
             */
            if (ctx.Rip - (uintptr_t)exe >= exe_size &&
                ctx.Rip - jit >= jit_size && text != NULL &&
                VirtualQuery((void *)ctx.Rsp, &mbi, sizeof(mbi)) != 0) {
                const uint64_t *sp = (const uint64_t *)ctx.Rsp;
                const uint64_t *top = (const uint64_t *)
                    ((uintptr_t)mbi.BaseAddress + mbi.RegionSize);
                ProfStack *st = &stacks[nstacks++];
                int found = 0;

                st->rip = ctx.Rip;
                for (int k = 0; k < 512 && sp + k < top &&
                     found < PROF_CALLERS; k++) {
                    if (sp[k] - (uintptr_t)text < text_size) {
                        st->callers[found++] = sp[k];
                    }
                }
            }
        }
        prof_ResumeThread(prof_vcpu);
    }
    if (timer != NULL) {
        CloseHandle(timer);
    }
    prof_report(job->id, rips, n,
                (g_get_monotonic_time() - start) / 1e6);
    prof_report_stacks(job->id, stacks, nstacks, exe);
    g_free(stacks);
    g_free(rips);
    g_free(job);
    qatomic_set(&prof_running, false);
    return NULL;
}

void xbox_prof_start(const char *id, int seconds)
{
    QemuThread thread;
    ProfJob *job;

    if (prof_vcpu == NULL || qatomic_xchg(&prof_running, true)) {
        return;
    }
    if (prof_SuspendThread == NULL) {
        prof_SuspendThread =
            (SuspendThreadFn)xbox_fastmem_bind_export("SuspendThread");
        prof_ResumeThread =
            (ResumeThreadFn)xbox_fastmem_bind_export("ResumeThread");
        prof_GetThreadContext =
            (GetThreadContextFn)xbox_fastmem_bind_export("GetThreadContext");
        prof_CreateWaitableTimerExW = (CreateWaitableTimerExWFn)
            xbox_fastmem_bind_export("CreateWaitableTimerExW");
        prof_SetWaitableTimer =
            (SetWaitableTimerFn)xbox_fastmem_bind_export("SetWaitableTimer");
    }
    if (prof_SuspendThread == NULL || prof_ResumeThread == NULL ||
        prof_GetThreadContext == NULL) {
        fprintf(stderr, "xbox prof: thread APIs unavailable\n");
        qatomic_set(&prof_running, false);
        return;
    }
    job = g_new0(ProfJob, 1);
    g_strlcpy(job->id, id, sizeof(job->id));
    job->seconds = seconds;
    fprintf(stderr, "xbox prof %s: sampling %d s\n", id, seconds);
    qemu_thread_create(&thread, "xbox.prof", prof_thread, job,
                       QEMU_THREAD_DETACHED);
}
