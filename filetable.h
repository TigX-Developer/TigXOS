/* filetable.h — shared open-file objects (TigxOS 1.0.79) */
#ifndef FILETABLE_H
#define FILETABLE_H

#include <stdint.h>
#include "fs.h"

#define MAX_OPEN_FILES 64

/* Types stored on the shared object (mirrors old fd_type for FILE/PIPE/CONSOLE). */
enum of_type {
    OF_NONE = 0,
    OF_CONSOLE,
    OF_FILE,
    OF_PIPE
};

struct open_file {
    int      used;
    int      refs;
    int      type;                  /* OF_* */
    int      flags;
    uint32_t offset;                /* shared for OF_FILE */
    char     name[TIGFS_NAME_LEN];  /* OF_FILE path */
    int      pipe_id;               /* OF_PIPE */
    int      deferred_unlink;       /* unlink while open → remove on last close */
};

void filetable_init(void);
int  of_alloc(void);                /* new object, refs=1, or -1 */
void of_incref(int id);
void of_decref(int id);             /* last ref: pipe close / deferred unlink */
struct open_file *of_get(int id);   /* NULL if invalid */

/* Mark matching open files for deferred unlink; returns 1 if any had refs. */
int  of_mark_unlink(const char *name);

#endif
