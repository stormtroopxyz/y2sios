/*
 * The Galaxy S20+'s Mali-G77 (bsd/dev/redwood_mali.c) for the rest of the kernel: what
 * iokit/Kernel/RedwoodMali.cpp (Linux's panfrost interface for programs) is built on.
 *
 * One GPU address space (AS 0) for everybody, 4 KB pages, addresses from 32 MB to 4 GB. Jobs
 * are run one at a time in the order they were given, so a job that depends on an earlier one
 * (Mesa's fragment job on its vertex/tiler job) never needs to be held back; a job's sequence
 * number is done once every job before it is.
 */
#ifndef _SYS_REDWOOD_MALI_H_
#define _SYS_REDWOOD_MALI_H_

#ifdef KERNEL
#include <stdint.h>
#include <stdbool.h>

#define REDWOOD_MALI_MAP_NOEXEC         0x1     /* the GPU may not run code from it */
#define REDWOOD_MALI_MAP_READ_ONLY      0x2

#define REDWOOD_MALI_JOB_FS             0x1     /* PANFROST_JD_REQ_FS: a fragment job chain (slot 0) */

__BEGIN_DECLS
bool redwood_mali_ready(void);
/* Linux's DRM_PANFROST_PARAM_*: false if there is no such parameter */
bool redwood_mali_param(uint32_t param, uint64_t *value);

/* GPU addresses: `exec` ones are aligned so as never to cross a 16 MB line (the GPU's rule for code) */
uint64_t redwood_mali_va_alloc(uint64_t size, bool exec);
void redwood_mali_va_free(uint64_t va, uint64_t size);
/* one 4 KB page; then redwood_mali_mapped() once for the whole range, before the GPU uses it */
bool redwood_mali_map(uint64_t va, uint64_t pa, uint32_t flags);
void redwood_mali_mapped(uint64_t va, uint64_t size);
void redwood_mali_unmap(uint64_t va, uint64_t size);

/* a job chain: its sequence number, 0 if the GPU is not there */
uint64_t redwood_mali_submit(uint64_t jc, uint32_t requirements);
bool redwood_mali_done(uint64_t seqno);
/* true: done; false: not within `milliseconds` */
bool redwood_mali_wait(uint64_t seqno, uint64_t milliseconds);
uint64_t redwood_mali_last(void);
/* jobs that ended in a fault, and resets of the GPU, since boot */
void redwood_mali_counts(uint64_t *jobs, uint64_t *faults, uint64_t *resets);

/* processor caches: written back and dropped, for memory the GPU reads or has written (IOSurfaces) */
void redwood_mali_to_memory(const void *start, uint64_t size);
__END_DECLS
#endif /* KERNEL */

#endif /* _SYS_REDWOOD_MALI_H_ */
