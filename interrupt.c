// interrupt.c
#include "interrupt.h"
#include "console.h"
#include "io.h"
#include "process.h"
#include "string.h"
#include "arch/arch.h"
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define IDT_SIZE 256
#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI 0x20
#define ICW1_ICW4 0x01
#define ICW1_INIT 0x10

// IDT and handlers
static struct idt_entry idt[IDT_SIZE];
static struct idt_ptr idtp;
static isr_handler_t handlers[IDT_SIZE];

// Exception names
static const char* exception_names[] = {
    "Division By Zero",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved"
};

// Set an IDT gate
void set_idt_gate(uint8_t vector, uintptr_t base, uint16_t selector, uint8_t flags) {
#if defined(__i386__)
    idt[vector].base_low  = (uint16_t)(base & 0xFFFF);
    idt[vector].base_high = (uint16_t)((base >> 16) & 0xFFFF);
    idt[vector].selector  = selector;
    idt[vector].zero      = 0;
    idt[vector].flags     = flags;
#else
    idt[vector].base_low  = (uint16_t)(base & 0xFFFF);
    idt[vector].base_mid  = (uint16_t)((base >> 16) & 0xFFFF);
    idt[vector].base_high = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    idt[vector].selector  = selector;
    idt[vector].ist       = 0;
    idt[vector].flags     = flags;
    idt[vector].zero      = 0;
#endif
}

// PIC remapping
void pic_remap(void) {
    // Start initialization
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    
    // Set offsets (IRQ0=32, IRQ8=40)
    outb(PIC1_DATA, 32);
    outb(PIC2_DATA, 40);
    
    // Tell PICs about each other
    outb(PIC1_DATA, 4);  // IRQ2 -> slave
    outb(PIC2_DATA, 2);  // Slave ID
    
    // Set 8086 mode
    outb(PIC1_DATA, 1);
    outb(PIC2_DATA, 1);
    
    // Mask all IRQs initially (enable selectively later)
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_enable_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    
    value = inb(port) & ~(1 << irq);
    outb(port, value);
}

void pic_disable_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    
    value = inb(port) | (1 << irq);
    outb(port, value);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

/*
 * Shared panic renderer — full-screen white-on-blue takeover + serial mirror.
 * frame may be NULL (software kernel_panic).
 *
 * Note: for CPL0→CPL0 faults the CPU does not push SS/RSP, so the original
 * user/kernel RSP at the fault site is not in interrupt_frame. We therefore
 * omit a true RSP value rather than invent one. R8–R15 are present.
 *
 * Double-fault (vector 8) on a corrupted stack can still triple-fault before
 * we get here; fixing that needs an IST via TSS (ist field is already in
 * idt_entry but currently hardcoded to 0). Track as a known gap until TSS
 * exists for ring-3.
 */
void panic_render(const char *reason, struct interrupt_frame *frame) {
    /*
     * cli FIRST. Hardware exceptions already have IF=0 (interrupt gate),
     * but kernel_panic() is called from ordinary kernel code with IRQs on.
     * A timer tick mid-render would preempt the panic screen itself.
     */
    __asm__ volatile("cli" ::: "memory");

    /*
     * Escape hatch: a fault during kprintf / console_* leaves console_lock
     * held by a context that will never resume.  Without releasing it here,
     * every subsequent spin_lock in the panic path spins forever (IRQs off,
     * holder dead) and the panic screen never appears.
     */
    console_force_unlock();

    /* Ensure serial is usable even if never initialised earlier */
    serial_init();

    /* Full-screen solid blue background, white text (0x1F) */
    console_setcolor(0x1F);
    console_clear();
    console_hide_cursor();

    /* --- dual-output helpers (VGA + serial) --- */
    #define P(...) do { kprintf(__VA_ARGS__); serial_printf(__VA_ARGS__); } while (0)

    /* 1. Banner — centered */
    {
        const char *banner = "*** TigxOS PANIC ***";
        int len = (int)strlen(banner);
        int pad = (80 - len) / 2;
        if (pad < 0) pad = 0;
        for (int i = 0; i < pad; i++)
            P(" ");
        P("%s\n\n", banner);
    }

    /* 2. One-line plain-English cause */
    if (reason && reason[0])
        P("%s\n\n", reason);
    else
        P("Unknown fatal error\n\n");

    /* 3. Fault specifics (only when we have a frame) */
    if (frame) {
        uint64_t vec = frame->interrupt_number;
        P("Vector: %d", (int)vec);
        if (vec < 32)
            P("  (%s)", exception_names[vec]);
        P("\n");

        if (frame->error_code != 0)
            P("Error code: 0x%x\n", frame->error_code);

        /* Page Fault — read CR2 for the faulting virtual address */
        if (vec == 14) {
            uintptr_t cr2 = arch_read_cr2();
            P("CR2 (fault addr): 0x%08x\n", (unsigned long)cr2);
        }
        P("\n");
    }

    /* 4. Context — current process */
    {
        struct process *cur = process_current();
        if (cur)
            P("Process: pid=%d  name=\"%s\"\n\n", cur->pid, cur->name);
        else
            P("Process: (none / early boot)\n\n");
    }

    /* 5. Full register dump */
    if (frame) {
        P("Registers:\n");
#if defined(__i386__)
        P("EAX: 0x%08x  EBX: 0x%08x\n", frame->eax, frame->ebx);
        P("ECX: 0x%08x  EDX: 0x%08x\n", frame->ecx, frame->edx);
        P("ESI: 0x%08x  EDI: 0x%08x\n", frame->esi, frame->edi);
        P("EBP: 0x%08x  ESP: 0x%08x\n", frame->ebp, frame->esp);
        P("EIP: 0x%08x  CS:  0x%04x (RPL=%d)\n",
          frame->eip, (unsigned)frame->cs, (int)(frame->cs & 3));
        P("EFLAGS: 0x%08x\n", frame->eflags);
        if ((frame->cs & 3) == 3) {
            /* After eflags on the iret frame: esp, ss (CPU-pushed) */
            uint32_t *extra = (uint32_t *)(frame + 1);
            P("User ESP: 0x%08x  SS: 0x%04x\n", extra[0], extra[1] & 0xFFFF);
        }
#else
        P("RAX: 0x%016x  RBX: 0x%016x\n", frame->rax, frame->rbx);
        P("RCX: 0x%016x  RDX: 0x%016x\n", frame->rcx, frame->rdx);
        P("RSI: 0x%016x  RDI: 0x%016x\n", frame->rsi, frame->rdi);
        P("RBP: 0x%016x  R8:  0x%016x\n", frame->rbp, frame->r8);
        P("R9:  0x%016x  R10: 0x%016x\n", frame->r9, frame->r10);
        P("R11: 0x%016x  R12: 0x%016x\n", frame->r11, frame->r12);
        P("R13: 0x%016x  R14: 0x%016x\n", frame->r13, frame->r14);
        P("R15: 0x%016x\n", frame->r15);
        P("RIP: 0x%016x  CS:  0x%04x (RPL=%d)\n",
          frame->rip, (unsigned)frame->cs, (int)(frame->cs & 3));
        P("RFLAGS: 0x%016x\n", frame->rflags);
        if ((frame->cs & 3) == 3) {
            /* Avoid taking address of packed member: copy via memcpy */
            uint64_t tail[3];
            memcpy(tail, (const char *)frame + offsetof(struct interrupt_frame, rflags),
                   sizeof(tail));
            P("User RSP: 0x%016x  SS: 0x%04x\n", tail[1], (unsigned)(tail[2] & 0xFFFF));
        } else {
            P("RSP:  (not pushed — CPL0 fault)\n");
        }
#endif
        P("\n");
    }

    /* 6. Footer — stay halted so the message can be read */
    P("System halted.\n");

    #undef P

    /* IF already cleared at entry; just park the CPU */
    for (;;)
        __asm__ volatile("hlt");
}

/* Software-triggered fatal error (OOM, assertion, corrupted structure, …) */
void kernel_panic(const char *fmt, ...) {
    char buf[192];
    va_list args;
    va_start(args, fmt);

    /* Minimal in-place formatting so we don't depend on a full vsprintf.
     * Support the same %s %d %u %x %p that the rest of the kernel uses. */
    {
        char *out = buf;
        char *end = buf + sizeof(buf) - 1;
        const char *p = fmt ? fmt : "kernel_panic";

        while (*p && out < end) {
            if (*p != '%') {
                *out++ = *p++;
                continue;
            }
            p++;
            if (!*p)
                break;
            /* skip optional width / 'l' */
            while (*p == '0' || (*p >= '1' && *p <= '9') || *p == 'l')
                p++;
            switch (*p) {
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s && out < end)
                    *out++ = *s++;
                break;
            }
            case 'c':
                *out++ = (char)va_arg(args, int);
                break;
            case 'd': {
                int v = va_arg(args, int);
                unsigned u;
                if (v < 0) {
                    if (out < end) *out++ = '-';
                    u = (unsigned)(-(v + 1)) + 1u;
                } else {
                    u = (unsigned)v;
                }
                char tmp[16];
                int i = 0;
                if (u == 0) tmp[i++] = '0';
                else while (u) { tmp[i++] = '0' + (u % 10); u /= 10; }
                while (i-- > 0 && out < end) *out++ = tmp[i];
                break;
            }
            case 'u': {
                unsigned long long u = va_arg(args, unsigned long long);
                char tmp[24];
                int i = 0;
                if (u == 0) tmp[i++] = '0';
                else while (u) { tmp[i++] = '0' + (u % 10); u /= 10; }
                while (i-- > 0 && out < end) *out++ = tmp[i];
                break;
            }
            case 'x': case 'X': {
                unsigned long long u = va_arg(args, unsigned long long);
                char tmp[24];
                int i = 0;
                const char *digits = (*p == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
                if (u == 0) tmp[i++] = '0';
                else while (u) { tmp[i++] = digits[u & 0xF]; u >>= 4; }
                while (i-- > 0 && out < end) *out++ = tmp[i];
                break;
            }
            case 'p': {
                unsigned long long u = (unsigned long long)(uintptr_t)va_arg(args, void *);
                if (out + 2 < end) { *out++ = '0'; *out++ = 'x'; }
                char tmp[24];
                int i = 0;
                if (u == 0) tmp[i++] = '0';
                else while (u) { tmp[i++] = "0123456789abcdef"[u & 0xF]; u >>= 4; }
                while (i-- > 0 && out < end) *out++ = tmp[i];
                break;
            }
            case '%':
                *out++ = '%';
                break;
            default:
                *out++ = '%';
                if (out < end) *out++ = *p;
                break;
            }
            p++;
        }
        *out = '\0';
    }
    va_end(args);

    panic_render(buf, NULL);
}

/* Default path for any unhandled exception (0–31).
 * Ring-3 faults kill the faulting process (do not blue-screen the OS).
 * Kernel faults still panic. */
static void default_exception_handler(struct interrupt_frame *frame) {
    const char *name = "Unknown";
    if (frame->interrupt_number < 32)
        name = exception_names[frame->interrupt_number];

    int ring = (int)(frame->cs & 3);
    if (ring == 3) {
        struct process *cur = process_current();
        int pid = cur ? cur->pid : -1;
        {
            uintptr_t cr2 = 0, ip = 0;
            if (frame->interrupt_number == 14)
                cr2 = arch_read_cr2();
#if defined(__i386__)
            ip = frame->eip;
#else
            ip = frame->rip;
#endif
            kprintf("user fault: %s  pid=%d ip=%p cr2=%p err=0x%x — killing\n",
                    name, pid, (void *)ip, (void *)cr2,
                    (unsigned)frame->error_code);
        }
        /* 128+vec style status; process_exit schedules away and does not return */
        if (cur && cur->pid != 0 && cur->is_user) {
            process_exit(128 + (int)(frame->interrupt_number & 0x1f));
        }
        /* If we cannot kill (idle / early), fall through to panic */
    }
    panic_render(name, frame);
}

// Initialize IDT
void idt_init(void) {
    // Clear handlers
    for (int i = 0; i < IDT_SIZE; i++) {
        handlers[i] = NULL;
    }

    // Set up IDT pointer
    idtp.limit = sizeof(struct idt_entry) * IDT_SIZE - 1;
    idtp.base = (uintptr_t)idt;

    // Set default handlers for exceptions 0-31
    for (int i = 0; i < 32; i++) {
        set_idt_gate(i, isr_stub_table[i], ARCH_KERNEL_CS, 0x8E);
    }
    
    // Set handlers for IRQs 0-15 (vectors 32-47)
    for (int i = 0; i < 16; i++) {
        set_idt_gate(32 + i, irq_stub_table[i], ARCH_KERNEL_CS, 0x8E);
    }

    /* No per-vector special cases: every unhandled exception goes through
       default_exception_handler → panic_render.  Register overrides only
       when you intentionally want different behaviour. */

    // Load IDT
    load_idt(&idtp);
    
    // Remap PIC
    pic_remap();
}

// Register handler for specific interrupt
void register_interrupt_handler(uint8_t vector, isr_handler_t handler) {
    handlers[vector] = handler;
}

// C handler called from assembly stub for exceptions
void isr_handler(struct interrupt_frame* frame) {
    if (handlers[frame->interrupt_number] != NULL) {
        handlers[frame->interrupt_number](frame);
    } else {
        default_exception_handler(frame);
    }
}

// C handler called from assembly stub for IRQs
void irq_handler(struct interrupt_frame* frame) {
    uint8_t irq = frame->interrupt_number - 32;

    /* EOI first: if the handler context-switches, the PIC must already
       be free or the timer never fires again until we unwind this IRQ. */
    pic_send_eoi(irq);

    if (handlers[frame->interrupt_number] != NULL)
        handlers[frame->interrupt_number](frame);
}
