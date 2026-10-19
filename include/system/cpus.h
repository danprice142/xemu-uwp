#ifndef QEMU_CPUS_H
#define QEMU_CPUS_H

/* register accel-specific operations */
void cpus_register_accel(const AccelOpsClass *i);

/* return registers ops */
const AccelOpsClass *cpus_get_accel(void);

/* interface available for cpus accelerator threads */

/* For temporary buffers for forming a name */
#define VCPU_THREAD_NAME_SIZE 16

void cpus_kick_thread(CPUState *cpu);
bool cpu_work_list_empty(CPUState *cpu);
bool cpu_thread_is_idle(CPUState *cpu);
bool all_cpu_threads_idle(void);
bool cpu_can_run(CPUState *cpu);
void qemu_process_cpu_events_common(CPUState *cpu);
extern int64_t qemu_vcpu_idle_us;
extern int64_t qemu_vcpu_exec_us;
extern int64_t qemu_vcpu_bql_us;
extern int64_t qemu_vcpu_tlb_fills;
extern int64_t qemu_mem_access_cb_flushes;
/* Translation block cache activity, for host-side profiling. */
extern int64_t qemu_tb_invalidates;
extern int64_t qemu_tb_jmp_flushes;
extern int64_t qemu_tb_jc_misses;
/* Jump cache misses by kind: empty slot, other pc, same pc other state. */
extern int64_t qemu_tb_jc_miss_kind[3];
extern uint32_t qemu_tb_jc_flag_diff;
extern int64_t qemu_tb_lookup_ptrs;
extern int64_t qemu_tb_gens;
/*
 * Main loop entries by how the previous TB returned: unchained direct
 * jump, exit request, epilogue (indirect jump miss or exit_tb 0), and
 * entries into page-spanning TBs, which are never chained.
 */
extern int64_t qemu_tb_loop_kind[4];
/* Host-pc to guest-state recoveries and I/O rewinds of a TB. */
extern int64_t qemu_tb_restores;
extern int64_t qemu_tb_io_recompiles;
void cpu_thread_signal_created(CPUState *cpu);
void cpu_thread_signal_destroyed(CPUState *cpu);
void cpu_handle_guest_debug(CPUState *cpu);

/* end interface for cpus accelerator threads */

bool qemu_in_vcpu_thread(void);
void qemu_init_cpu_loop(void);
void resume_all_vcpus(void);
void pause_all_vcpus(void);
void cpu_stop_current(void);

/* Unblock cpu */
void qemu_cpu_kick_self(void);

bool cpus_are_resettable(void);

void cpu_synchronize_all_states(void);
void cpu_synchronize_all_post_reset(void);
void cpu_synchronize_all_post_init(void);
void cpu_synchronize_all_pre_loadvm(void);

#endif
