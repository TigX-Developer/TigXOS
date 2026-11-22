/* smp.h — SMP foundation + 1.2.9 IPI / TLB / balance (TigxOS)
 *
 * 1.2.8: discover CPUs, INIT+SIPI, APs park.
 * 1.2.9: IPI (reschedule + TLB shootdown), per-CPU accounting, load-balance
 *        hooks, multicore stress counters. APs still park in the trampoline
 *        until they get a full long-mode entry; IPI delivery is exercised from
 *        the BSP and counted. i386 stays UP.
 */
#ifndef TIGX_SMP_H
#define TIGX_SMP_H

#include <stdint.h>

/* Software IPI vectors (must match IDT registration). */
#define IPI_VECTOR_TLB      0xFC
#define IPI_VECTOR_RESCHED  0xFD

void smp_init(void);
int  smp_cpu_count(void);
int  smp_cpu_id(void);
int  smp_apic_id(void);
int  smp_is_bsp(void);
int  smp_max_cpus(void);
int  smp_ap_started(void);

/* 1.2.9 */
void smp_ipi_init(void);                 /* register IPI IDT gates */
void smp_send_resched(int cpu);          /* ask cpu to reschedule */
void smp_send_resched_all(void);         /* all except self */
void smp_tlb_shootdown(uintptr_t page);  /* global invlpg request */
void smp_tlb_shootdown_all(void);        /* flush all TLB (CR3 reload) */
void smp_ipi_resched_handler(void);
void smp_ipi_tlb_handler(void);

/* Stats for tests / sysinfo */
uint64_t smp_ipi_resched_count(void);
uint64_t smp_ipi_tlb_count(void);
uint64_t smp_balance_steals(void);
void     smp_note_steal(void);
int      smp_need_resched(void);
void     smp_clear_need_resched(void);

/* Soft load-balance: update run-queue depth hint for this CPU */
void smp_rq_note_ready(int delta);
int  smp_rq_depth(int cpu);

#endif /* TIGX_SMP_H */
