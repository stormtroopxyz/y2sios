/*
 * mali2metal's log (m2m): the Metal driver's evidence in one channel, apart from the console.
 *
 * See bsd/dev/redwood_m2m.c and PLANS/10-mali2metal.md. User space writes records to /dev/m2m (one
 * record per write, "<sev> <CAT> <message>"), the kernel adds its own (GPU faults, the GPU's time,
 * Metal clients that die) through m2m_klog(), and a reader takes the whole channel through
 * sysctl kern.m2m_log (8 bytes of total, then the text oldest first) and kern.m2m_total.
 *
 * Each record, as stored and as read:
 *
 *   <seq> <seconds.millis> <sev> <CAT> <process>[<pid>] <message>
 *
 *   sev   I information, W something was declined or degraded, E an error, F fatal (a client died)
 *   CAT   2-5 capitals, a fixed set (PLANS/10-mali2metal.md "Categories")
 */
#ifndef _SYS_REDWOOD_M2M_H_
#define _SYS_REDWOOD_M2M_H_

#include <stdint.h>
#include <sys/ioccom.h>

#define M2M_DEVICE              "/dev/m2m"
#define M2M_RECORD_MAX          1024            /* the longest message kept; longer ones are cut */

/* the writer says what it is (a Metal driver, a test): the kernel then reports how it ends */
struct m2m_client {
	char role[16];
};
#define M2M_IOC_CLIENT          _IOW('m', 1, struct m2m_client)

#ifdef KERNEL
#include <sys/cdefs.h>

__BEGIN_DECLS
void redwood_m2m_init(void);
void m2m_klog(char sev, const char *cat, const char *fmt, ...) __printflike(3, 4);
/* a record on behalf of a process (the GPU job that faulted belongs to it) */
void m2m_klog_for(int pid, const char *proc, char sev, const char *cat, const char *fmt, ...) __printflike(5, 6);
/* a record on behalf of the calling process (a wait that gave up, an allocation it was refused) */
void m2m_klog_self(char sev, const char *cat, const char *fmt, ...) __printflike(3, 4);
/* from osfmk/kern/task.c: a registered client suspended (held 1) or resumed (0); bsd_info is its proc */
void redwood_m2m_task_hold(void *bsd_info, int held, const char *how);
/* from proc_exit: a registered client is gone; `status' is its wait status */
void redwood_m2m_proc_exit(int pid, const char *comm, int status);
/* from loader_return: the channel's last records into the loader's log, so a run that ends without
 * a network still leaves them */
void redwood_m2m_to_loader(void);
/* every core has been started: the count and each core's identity into the channel */
void redwood_m2m_cores(void);
__END_DECLS
#endif

#endif /* _SYS_REDWOOD_M2M_H_ */
