#ifndef LINUX_HUNG_TASK_H
#define LINUX_HUNG_TASK_H

#ifdef CONFIG_GENERIC_HARDIRQS
void signal_start_coredump(void);
void signal_end_coredump(void);
#else
static inline void signal_start_coredump(void);
static inline void signal_end_coredump(void);
#endif

#endif
