/* filetable.h — shared open-file objects (TigxOS 1.2.1) */
#ifndef FILETABLE_H
#define FILETABLE_H

#include <stdint.h>
#include "fs.h"

#define MAX_OPEN_FILES 64

enum of_type {
    OF_NONE = 0,
    OF_CONSOLE,
    OF_FILE,
    OF_PIPE
};

struct open_file {
    int      used;
    int      refs;
    int      type;
    int      flags;
    uint32_t offset;
    char     name[TIGFS_NAME_LEN];
    int      pipe_id;
    int      deferred_unlink;
    int      ino;   /* TigFS inode (1.2.1) */
    int      fsid;  /* VFS_FS_* */
};

void filetable_init(void);
int  of_alloc(void);
void of_incref(int id);
void of_decref(int id);
struct open_file *of_get(int id);
int  of_mark_unlink(const char *name);

#endif
