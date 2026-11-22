

#include "timer.h"
#include "interrupt.h"
#include "console.h"
#include "spinlock.h"
#include "io.h"
#include "process.h"
#include "wait.h"
#include <stdint.h>

#define PIT_COMMAND_PORT  0x43
#define PIT_CHANNEL0_PORT 0x40
#define PIT_FREQUENCY     1193180

static volatile uint32_t tick_count = 0;
static uint32_t timer_hz = 100;

static void timer_handler(struct interrupt_frame *frame) {
    (void)frame;
    tick_count++;
    /* Wake sleepers whose deadline has passed, then reschedule.
       Both run with IF=0 (interrupt gate). */
    process_wake_timed();
    scheduler_tick();
}

void timer_init(uint32_t frequency) {
    if (frequency == 0)
        frequency = 100;
    timer_hz = frequency;
    uint32_t d = PIT_FREQUENCY / frequency;
    if (d == 0) d = 1;
    outb(PIT_COMMAND_PORT, 0x36);
    outb(PIT_CHANNEL0_PORT, d & 0xFF);
    outb(PIT_CHANNEL0_PORT, (d >> 8) & 0xFF);
    register_interrupt_handler(32, timer_handler);
    pic_enable_irq(0);
    kprintf("Timer %d Hz (preemptive, blocking sleep)\n", frequency);
}

void timer_sleep(uint32_t s) {
    /* Avoid 32-bit overflow on s * 1000 */
    if (s >= 0xFFFFFFFFu / 1000u)
        timer_sleep_ms(0xFFFFFFFFu);
    else
        timer_sleep_ms(s * 1000u);
}

void timer_sleep_ms(uint32_t ms) {
    uint32_t hz = timer_hz ? timer_hz : 100;
    /* 64-bit math so ms * hz cannot wrap for any uint32_t ms */
    uint64_t ticks64 = ((uint64_t)ms * (uint64_t)hz + 999ull) / 1000ull;
    if (ticks64 == 0)
        ticks64 = 1;
    if (ticks64 > 0xffffffffull)
        ticks64 = 0xffffffffull;
    uint32_t ticks = (uint32_t)ticks64;

    /* Before the scheduler is up, we cannot block — fall back to HLT loop
       with IRQs enabled (timer IRQ still advances tick_count). */
    if (!scheduler_is_enabled()) {
        uint32_t until = tick_count + ticks;
        if (until == 0)
            until = 1; /* avoid 0 sentinel if tick_count wraps */
        while ((int32_t)(tick_count - until) < 0)
            __asm__ volatile("sti; hlt");
        return;
    }

    /* IRQs off, then block with timeout.  process_block_current returns
       with IF still 0 — we re-enable here.  Never pass wake_tick 0. */
    uint32_t wake = tick_count + ticks;
    if (wake == 0)
        wake = 1;

    unsigned long flags = irq_save();
    process_block_current(wake);
    irq_restore(flags);
}

uint32_t timer_get_ticks(void) { return tick_count; }
uint32_t timer_get_hz(void)    { return timer_hz; }
