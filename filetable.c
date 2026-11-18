/* filetable.c — shared open-file table with refcounts (TigxOS 1.1.12) */

#include "filetable.h"
#include "pipe.h"
#include "string.h"
#include "console.h"
#include "spinlock.h"
#include <stddef.h>

static struct open_file oftab[MAX_OPEN_FILES];

void filetable_init(void) {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        oftab[i].used = 0;
        oftab[i].refs = 0;
        oftab[i].type = OF_NONE;
        oftab[i].deferred_unlink = 0;
        oftab[i].pipe_id = -1;
            oftab[i].ino = -1;
            oftab[i].fsid = 0;
        oftab[i].name[0] = 0;
        oftab[i].ino = -1;
        oftab[i].fsid = 0;
    }
    kprintf("Filetable: %d open-file slots (shared offset + refcount)\n",
            MAX_OPEN_FILES);
}

int of_alloc(void) {
    unsigned long flags = irq_save();
    int id = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!oftab[i].used) {
            oftab[i].used = 1;
            oftab[i].refs = 1;
            oftab[i].type = OF_NONE;
            oftab[i].flags = 0;
            oftab[i].offset = 0;
            oftab[i].name[0] = 0;
            oftab[i].pipe_id = -1;
            oftab[i].ino = -1;
            oftab[i].fsid = 0;
            oftab[i].deferred_unlink = 0;
            id = i;
            break;
        }
    }
    irq_restore(flags);
    return id;
}

void of_incref(int id) {
    if (id < 0 || id >= MAX_OPEN_FILES)
        return;
    unsigned long flags = irq_save();
    if (oftab[id].used)
        oftab[id].refs++;
    irq_restore(flags);
}

void of_decref(int id) {
    if (id < 0 || id >= MAX_OPEN_FILES)
        return;
    unsigned long flags = irq_save();
    if (!oftab[id].used || oftab[id].refs <= 0) {
        irq_restore(flags);
        return;
    }
    oftab[id].refs--;
    if (oftab[id].refs > 0) {
        irq_restore(flags);
        return;
    }
    /* Last reference */
    struct open_file *o = &oftab[id];
    /* Never destroy the shared console object — keep a zombie slot. */
    if (o->type == OF_CONSOLE) {
        o->refs = 1; /* pin permanently */
        irq_restore(flags);
        return;
    }
    if (o->type == OF_PIPE && o->pipe_id >= 0) {
        int acc = o->flags & 3;
        extern void pipe_decref(int pipe_id, int is_write);
        if (acc == 1 /* O_WRONLY */ || acc == 2 /* O_RDWR */)
            pipe_decref(o->pipe_id, 1);
        if (acc == 0 /* O_RDONLY */ || acc == 2)
            pipe_decref(o->pipe_id, 0);
    }
    if (o->type == OF_FILE && o->deferred_unlink && o->name[0]) {
        extern int fs_remove(const char *name);
        fs_remove(o->name);
    }
    o->used = 0;
    o->type = OF_NONE;
    o->name[0] = 0;
    o->pipe_id = -1;
    o->deferred_unlink = 0;
    irq_restore(flags);
}

struct open_file *of_get(int id) {
    if (id < 0 || id >= MAX_OPEN_FILES)
        return 0;
    if (!oftab[id].used || oftab[id].refs <= 0)
        return 0;
    return &oftab[id];
}

int of_mark_unlink(const char *name) {
    if (!name || !name[0])
        return 0;
    int any = 0;
    unsigned long flags = irq_save();
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (oftab[i].used && oftab[i].type == OF_FILE &&
            oftab[i].name[0] && strcmp(oftab[i].name, name) == 0) {
            oftab[i].deferred_unlink = 1;
            any = 1;
        }
    }
    irq_restore(flags);
    if (any) {
        extern int fs_detach_name(const char *name);
        fs_detach_name(name);
    }
    return any;
}
