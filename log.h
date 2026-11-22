/* log.h — dmesg ring buffer + leveled logging (TigxOS 1.2.5) */
#ifndef TIGX_LOG_H
#define TIGX_LOG_H

#include <stddef.h>
#include <stdint.h>

enum log_level {
    LOG_EMERG  = 0,
    LOG_ALERT  = 1,
    LOG_CRIT   = 2,
    LOG_ERR    = 3,
    LOG_WARN   = 4,
    LOG_NOTICE = 5,
    LOG_INFO   = 6,
    LOG_DEBUG  = 7
};

#define LOG_LEVEL_DEFAULT  LOG_INFO
#define LOG_BUF_SIZE       16384

void log_init(void);
void klog(int level, const char *fmt, ...);
void log_append(int level, const char *msg);
void log_ring_raw(int level, const char *msg, size_t len);

/* off is absolute sequence number (never resets). Returns bytes copied. */
size_t log_read(char *buf, size_t len, size_t off);
size_t log_seq(void);
size_t log_size(void);

void log_dump_recent(unsigned max_lines);
void log_stack_trace(void *frame_ptr, unsigned max_frames);

void log_set_console_level(int level);
int  log_get_console_level(void);

/* Set by panic_render; second fault just halts. */
extern volatile int panicking;

#define pr_emerg(...)  klog(LOG_EMERG,  __VA_ARGS__)
#define pr_alert(...)  klog(LOG_ALERT,  __VA_ARGS__)
#define pr_crit(...)   klog(LOG_CRIT,   __VA_ARGS__)
#define pr_err(...)    klog(LOG_ERR,    __VA_ARGS__)
#define pr_warn(...)   klog(LOG_WARN,   __VA_ARGS__)
#define pr_notice(...) klog(LOG_NOTICE, __VA_ARGS__)
#define pr_info(...)   klog(LOG_INFO,   __VA_ARGS__)
#define pr_debug(...)  klog(LOG_DEBUG,  __VA_ARGS__)

#endif
