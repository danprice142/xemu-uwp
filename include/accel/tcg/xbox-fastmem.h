/*
 * Direct-mapped guest memory for the Xbox vCPU on Windows hosts.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_XBOX_FASTMEM_H
#define ACCEL_TCG_XBOX_FASTMEM_H

#ifdef _WIN32

typedef struct XboxFastmemStats {
    uint64_t faults;
    uint64_t maps;
    uint64_t upgrades;
    uint64_t dirty;
    uint64_t patches;
    uint64_t unmaps;
    uint64_t protects;
    uint64_t flush_all;
    uint64_t sites;
    /* Why sites were patched to the TLB path. */
    uint64_t patch_invalid;
    uint64_t patch_mmio;
    uint64_t patch_watch;
    uint64_t patch_not_ram;
    uint64_t patch_code;
    uint64_t patch_read_only;
    uint64_t patch_guard;
    uint64_t patch_map_failed;
} XboxFastmemStats;

/* Arena reserved and fault handler installed (code generation may use it). */
extern bool xbox_fastmem_enabled;
/* Guest RAM is section backed and the arena tracks the softmmu TLB. */
extern bool xbox_fastmem_active;
/* New translations compile accesses in xbox_fastmem_mmu_idx directly. */
extern bool xbox_fastmem_codegen;
extern int xbox_fastmem_mmu_idx;
extern uint8_t *xbox_fastmem_arena;
extern XboxFastmemStats xbox_fastmem_stats;

bool xbox_fastmem_bind_platform(void);
/* Real export of the host memory/thread API modules, or NULL. */
void *xbox_fastmem_bind_export(const char *name);
void xbox_fastmem_platform_probe(void);

/* vCPU sampling profiler. */
void xbox_prof_register_vcpu(void);
void xbox_prof_start(const char *id, int seconds);

bool xbox_fastmem_init(void);
void *xbox_fastmem_alloc_ram(uint64_t size);
void xbox_fastmem_set_ram(void *ram, uint64_t size, int mmu_idx);
/* Switches direct accesses for new code and retranslates everything. */
void xbox_fastmem_set_codegen(bool on);
/* New translations probe the TB jump cache inline at indirect branches. */
extern bool xbox_tb_inline_lookup;
void xbox_tb_set_inline_lookup(bool on);
/* New translations run guest SSE arithmetic as host instructions. */
extern bool xbox_sse_native;
void xbox_set_sse_native(bool on);
/* MMIO accesses inside a TB rewind it to run the access as its last insn. */
extern bool xbox_io_rewind;
/* Periodic diagnostic logging ("diag" in the tune file); off by default. */
extern bool xbox_diag_log;
#define xbox_diag_printf(...)                                                 \
    do {                                                                      \
        if (xbox_diag_log) {                                                  \
            fprintf(stderr, __VA_ARGS__);                                     \
        }                                                                     \
    } while (0)

/* Code generation: sites of one translation are committed together. */
void xbox_fastmem_stage_reset(void);
void xbox_fastmem_stage_site(void *start, const void *stub, const void *end,
                             int mmu_idx);
void xbox_fastmem_stage_commit(void);
void xbox_fastmem_sites_reset(void);

/* Coherence with the softmmu TLB. */
void xbox_fastmem_flush_page(uint64_t vaddr);
void xbox_fastmem_flush_range(uint64_t vaddr, uint64_t len);
void xbox_fastmem_flush_all(void);
void xbox_fastmem_write_protect_host(uintptr_t host, uintptr_t len);
void xbox_fastmem_unmap_host(uintptr_t host, uintptr_t len);

#endif

#endif
