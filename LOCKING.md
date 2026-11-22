# TigxOS locking rules (1.2.7)

## Principles
- Prefer `irq_save` / `irq_restore` or spinlocks over raw `cli`/`sti`.
- Raw `cli`/`sti` are only allowed in:
  - spinlock internals
  - idle/halt paths
  - panic (after `panicking = 1`)
  - the final `sti` that starts the scheduler
- Never take the same spinlock recursively.
- Always release locks in reverse acquisition order.

## Lock order (must not invert)

```
console_lock
  └─ log_lock
sched_lock / process table
  └─ (run queues are under sched_lock)
filetable / open-file slots
  └─ pipe locks
VFS / TigFS / ramfs
  └─ (directory and inode state)
PMM / frame bitmap
  └─ kmalloc / heap
```

When holding a lower lock you may **not** take a higher one.

## Shared structures that must be locked
| Structure            | Lock / mechanism          | Notes |
|----------------------|---------------------------|-------|
| Process table        | `sched_lock`              | create, exit, wait, kill |
| Run queues           | `sched_lock`              | pick_next, yield |
| Console / kprintf    | `console_lock`            | IRQ-safe |
| Log ring             | `log_lock`                | trylock in panic |
| Open-file table      | filetable spinlock        | refcount + offset |
| Pipes                | per-pipe wait queues + IF | R/W sides |
| PMM frame bitmap     | pmm lock                  | alloc/free frames |
| Kernel heap          | heap lock                 | kmalloc/kfree |
| VFS / TigFS          | fs lock                   | path walk, create |

## Spawn path (fixed in 1.2.7)
`sys_spawn` previously used a home-made `spawn_busy` flag with raw `cli`/`sti`
around static argument buffers. That is now a proper spinlock (`spawn_lock`)
so it is safe once multiple cores are online.

## NCPU_MAX
Raised to **16** in 1.2.7. Only CPU 0 is online until 1.2.8.

## Debug
A future `LOCK_DEBUG` build can record the holder and a timestamp; for now
the rules above are the contract.
