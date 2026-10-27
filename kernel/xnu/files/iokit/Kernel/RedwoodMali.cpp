/*
 * RedwoodMali: the phone's Mali-G77 for programs to draw with (bsd/dev/redwood_mali.c is the
 * hardware). A program that opens it speaks the interface of Linux's panfrost DRM driver, the one
 * Mesa's panfrost knows (25.0.7: driver version 1.3): every request of that interface is a call of
 * the user client with the request's number as the selector and its structure passed in and back
 * (userland: tools/mesa/redwood-drm, which stands in for libdrm inside Mesa), the same arrangement
 * as RedwoodGPU.cpp has for the Adreno of the phone this port came from.
 *
 *   panfrost   SUBMIT, WAIT_BO, CREATE_BO, MMAP_BO, GET_PARAM, GET_BO_OFFSET, MADVISE, SET_LABEL_BO
 *   drm        GEM_CLOSE, SYNCOBJ_CREATE / DESTROY / WAIT / RESET / SIGNAL
 *   ours       0x7b / 0x7c a syncobj's fence set / read (what redwood-drm makes sync files of),
 *              0x7e an IOSurface as a buffer, 0x7f pixels onto the panel, 0x7d counters
 *
 * A buffer's memory is the client memory of the type that is its handle (what Linux does with
 * mmap() on the device). Buffers are wired, zeroed, and write-combined: what the program writes
 * goes to memory, where the GPU reads it (the GPU's mappings are non-cacheable, its own caches are
 * cleaned and invalidated around every job). IOSurfaces are ordinary cached memory: pushed out of
 * this processor's cache before a job that names them.
 *
 * All clients share the GPU's one address space; a fence is a job's sequence number, and jobs run
 * in the order they were submitted, so a job's in-fences are always done before it starts.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <sys/proc.h>
#include <sys/systm.h>

#include "MiniIOSurface.h"

extern "C" {
/* bsd/sys/redwood_m2m.h (not seen from here): mali2metal's channel */
void m2m_klog_self(char sev, const char *cat, const char *fmt, ...) __printflike(3, 4);
/* bsd/sys/redwood_mali.h (not seen from here) */
#define REDWOOD_MALI_MAP_NOEXEC         0x1
bool redwood_mali_ready(void);
void redwood_mali_share_completed(volatile uint64_t *where);
bool redwood_mali_param(uint32_t param, uint64_t *value);
uint64_t redwood_mali_va_alloc(uint64_t size, bool exec);
void redwood_mali_va_free(uint64_t va, uint64_t size);
bool redwood_mali_map(uint64_t va, uint64_t pa, uint32_t flags);
void redwood_mali_mapped(uint64_t va, uint64_t size);
void redwood_mali_unmap(uint64_t va, uint64_t size);
uint64_t redwood_mali_submit(uint64_t jc, uint32_t requirements);
bool redwood_mali_done(uint64_t seqno);
bool redwood_mali_wait(uint64_t seqno, uint64_t milliseconds);
uint64_t redwood_mali_last(void);
uint64_t redwood_mali_last_fault(void);
void loader_log_line(const char *line);
void redwood_mali_counts(uint64_t *jobs, uint64_t *faults, uint64_t *resets);
void redwood_mali_to_memory(const void *start, uint64_t size);

void redwood_mali_buffer_report(char * out, size_t length);

void RedwoodMaliStart(void);
bool RedwoodMaliSurfaceDrawn(uint32_t fence, uint64_t milliseconds);
void MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes);
}

/* Linux's include/uapi/drm/panfrost_drm.h and drm.h: request numbers (DRM_COMMAND_BASE 0x40) */
enum {
	kDrmGemClose            = 0x09,
	kDrmSyncobjCreate       = 0xbf,
	kDrmSyncobjDestroy      = 0xc0,
	kDrmSyncobjWait         = 0xc3,
	kDrmSyncobjReset        = 0xc4,
	kDrmSyncobjSignal       = 0xc5,
	kPanfrostSubmit         = 0x40,
	kPanfrostWaitBo         = 0x41,
	kPanfrostCreateBo       = 0x42,
	kPanfrostMmapBo         = 0x43,
	kPanfrostGetParam       = 0x44,
	kPanfrostGetBoOffset    = 0x45,
	kPanfrostMadvise        = 0x48,
	kPanfrostSetLabelBo     = 0x49,
	kRedwoodSyncobjSet      = 0x7b,
	kRedwoodSyncobjGet      = 0x7c,
	kRedwoodMaliCounts      = 0x7d,
	kRedwoodSurface         = 0x7e,
	kRedwoodShow            = 0x7f,
};

struct drm_panfrost_submit {
	uint64_t jc, in_syncs;
	uint32_t in_sync_count, out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count, requirements;
	uint32_t jm_ctx_handle, pad;            /* (newer than Mesa 25.0.7, which passes 48 bytes) */
};
/* SUBMIT as the port's libdrm sends it (tools/mesa/redwood-drm): Linux's request and, after it, where the
 * job's sequence number is written back, so that the library can answer an idle query itself */
struct redwood_mali_submit_ex {
	struct drm_panfrost_submit submit;
	uint64_t seqno;
};
struct drm_panfrost_wait_bo {
	uint32_t handle, pad;
	int64_t timeout_ns;
};
struct drm_panfrost_create_bo {
	uint32_t size, flags, handle, pad;
	uint64_t offset;
};
struct drm_panfrost_mmap_bo {
	uint32_t handle, flags;
	uint64_t offset;
};
struct drm_panfrost_get_param {
	uint32_t param, pad;
	uint64_t value;
};
struct drm_panfrost_get_bo_offset {
	uint32_t handle, pad;
	uint64_t offset;
};
struct drm_panfrost_madvise {
	uint32_t handle, madv, retained;
};
struct drm_gem_close {
	uint32_t handle, pad;
};
struct drm_syncobj_create {
	uint32_t handle, flags;
};
struct drm_syncobj_destroy {
	uint32_t handle, pad;
};
struct drm_syncobj_wait {
	uint64_t handles;
	int64_t timeout_nsec;
	uint32_t count_handles, flags, first_signaled, pad;
	uint64_t deadline_nsec;
};
struct drm_syncobj_array {
	uint64_t handles;
	uint32_t count_handles, pad;
};
struct redwood_syncobj_fence {
	uint32_t handle, pad;
	uint64_t fence;
};
struct redwood_mali_counts {
	uint64_t jobs, faults, resets, last;
};
struct redwood_surface {
	uint32_t surface, handle;
	uint64_t size;
	uint32_t offset, rowBytes, width, height, pixelFormat, planes;
	/* (appended: an older caller asks with the struct above and gets it) each plane's start and row */
	uint32_t planeOffset[2], planeRowBytes[2];
};
struct redwood_show {
	uint64_t pixels;
	uint32_t width, height, rowBytes, pad;
};

#define PANFROST_BO_NOEXEC              1
#define PANFROST_BO_HEAP                2
#define DRM_SYNCOBJ_CREATE_SIGNALED     1
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL 1
#define kRedwoodDescribe                0xffffffffu

#define MALI_BUFFERS            8192            /* a client's, at most */
#define MALI_BUFFER_MAX         (512ull << 20)
#define MALI_SYNCOBJS           1024
#define MALI_SUBMIT_BOS         16384
#define MALI_PATIENCE_MS        5000            /* for a wait without a limit */
#define MALI_LEFT               256

static IOLock * gMaliLock;

/* How long from now an absolute time of the caller's (CLOCK_MONOTONIC: mach_continuous_time here) is, in ms. */
static uint64_t
millisecondsUntil(int64_t when)
{
	uint64_t now;
	int64_t left;

	if (when <= 0) {
		return 0;
	}
	if (when == INT64_MAX) {
		return MALI_PATIENCE_MS;
	}
	absolutetime_to_nanoseconds(mach_continuous_time(), &now);
	left = when - (int64_t) now;
	if (left <= 0) {
		return 0;
	}
	return (uint64_t) left / NSEC_PER_MSEC > MALI_PATIENCE_MS ? MALI_PATIENCE_MS : ((uint64_t) left + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class RedwoodMali : public IOService
{
	OSDeclareDefaultStructors(RedwoodMali);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(RedwoodMali, IOService);

bool
RedwoodMali::start(IOService * provider)
{
	uint64_t id = 0;

	if (!IOService::start(provider)) {
		return false;
	}
	redwood_mali_param(0, &id);
	setProperty(kIOUserClientClassKey, "RedwoodMaliUserClient");
	setProperty("GPUProductID", id, 32);
	setName("RedwoodMali");
	registerService();
	IOLog("RedwoodMali: ready (Mali product %#llx); programs speak panfrost 1.3 to it\n", id);
	return true;
}

void
RedwoodMaliStart(void)
{
	IOService * provider = IOService::getPlatform();
	RedwoodMali * gpu;

	gMaliLock = IOLockAlloc();
	gpu = OSTypeAlloc(RedwoodMali);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("RedwoodMali: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("RedwoodMali: the service did not start\n");
	}
	gpu->release();
}

/* For MiniIOSurfaceDrawn() (RedwoodGPU.cpp): the display waits for the GPU to be done with a surface. */
bool
RedwoodMaliSurfaceDrawn(uint32_t fence, uint64_t milliseconds)
{
	if (!redwood_mali_ready()) {
		return true;
	}
	/* (the surface's fence is the low 32 bits of a sequence number; the high ones are today's) */
	uint64_t last = redwood_mali_last(), seqno = (last & ~0xffffffffULL) | fence;

	if (seqno > last) {
		seqno -= 1ULL << 32;
	}
	return redwood_mali_wait(seqno, milliseconds);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct mali_buffer {
	IOMemoryDescriptor *    memory;         /* NULL: this handle is free */
	uint8_t *               kernel;
	MiniIOSurface *         surface;        /* whose memory it is, when not its own */
	uint64_t                address, size;
	uint32_t                flags;
	uint32_t                seed;           /* the surface's when it was last pushed out of the cache */
	uint64_t                fence;          /* the last job that names it */
};

/* A buffer given back while the GPU may still use it: kept until its job is done. */
struct mali_left {
	IOMemoryDescriptor *    memory;
	MiniIOSurface *         surface;
	uint64_t                address, size, fence;
};

/*
 * What each client's buffers hold, for the log. Changed with gMaliLock held, read without it (the
 * GPU's scheduler reports from under its own lock, which submit() takes inside gMaliLock): a figure.
 */
#define MALI_STAT_CLIENTS       16
static struct mali_stat {
	int32_t                 pid;            /* 0: a free slot */
	char                    name[20];
	uint64_t                own, surfaces, left;
} gMaliStats[MALI_STAT_CLIENTS];

class RedwoodMaliUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(RedwoodMaliUserClient);

	task_t                  fTask;
	struct mali_buffer *    fBuffers;       /* handle n is fBuffers[n - 1] */
	uint64_t *              fSyncobjs;      /* handle n: fSyncobjs[n - 1], the fence (0: signalled); ~0: free */
	uint32_t                fNextBuffer;
	uint32_t                fBufferCount;
	uint64_t                fBufferBytes;
	uint32_t                fSubmits;
	struct mali_left        fLeft[MALI_LEFT];
	uint32_t                fLeftCount;
	struct mali_stat *      fStat;          /* NULL: no slot was free */
	int32_t                 fPid;
	char                    fName[20];

	struct mali_buffer * buffer(uint32_t handle);
	struct mali_buffer * newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t mapFlags, uint32_t * handle);
	void dropMapping(uint64_t address, uint64_t size);
	void freeBuffer(struct mali_buffer * b);
	void reap(bool wait);
	uint64_t * syncobj(uint32_t handle);
	IOReturn createBo(struct drm_panfrost_create_bo * request);
	IOReturn surfaceBuffer(struct redwood_surface * request);
	IOReturn submit(struct drm_panfrost_submit * request, size_t size);
	IOReturn syncobjWait(struct drm_syncobj_wait * request);
	IOReturn syncobjArray(struct drm_syncobj_array * request, bool reset);
	IOReturn show(const struct redwood_show * request);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(RedwoodMaliUserClient, IOUserClient);

bool
RedwoodMaliUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	fPid = proc_selfpid();
	proc_selfname(fName, sizeof(fName));
	return true;
}

bool
RedwoodMaliUserClient::start(IOService * provider)
{
	if (!redwood_mali_ready() || !IOUserClient::start(provider)) {
		return false;
	}
	fBuffers = IONewZero(struct mali_buffer, MALI_BUFFERS);
	fSyncobjs = IONew(uint64_t, MALI_SYNCOBJS);
	if (fBuffers == NULL || fSyncobjs == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < MALI_SYNCOBJS; i++) {
		fSyncobjs[i] = ~0ULL;
	}
	IOLockLock(gMaliLock);
	for (uint32_t i = 0; i < MALI_STAT_CLIENTS && fStat == NULL; i++) {
		if (gMaliStats[i].pid == 0) {
			fStat = &gMaliStats[i];
			bzero(fStat, sizeof(*fStat));
			strlcpy(fStat->name, fName, sizeof(fStat->name));
			fStat->pid = fPid > 0 ? fPid : -1;
		}
	}
	IOLockUnlock(gMaliLock);
	return true;
}

/* "GPU buffers 412 MB in 5 programs (+38 MB of IOSurfaces, 6 MB waiting for the GPU); backboardd 230, SpringBoard 120" */
void
redwood_mali_buffer_report(char * out, size_t length)
{
	struct mali_stat copy[MALI_STAT_CLIENTS];
	uint64_t own = 0, surfaces = 0, left = 0;
	uint32_t clients = 0, used;

	memcpy(copy, (const void *) gMaliStats, sizeof(copy));
	for (uint32_t i = 0; i < MALI_STAT_CLIENTS; i++) {
		if (copy[i].pid != 0) {
			own += copy[i].own;
			surfaces += copy[i].surfaces;
			left += copy[i].left;
			clients++;
		}
	}
	used = (uint32_t) snprintf(out, length, "GPU buffers %llu MB in %u programs (+%llu MB of IOSurfaces, %llu MB waiting for the GPU)",
	    own >> 20, clients, surfaces >> 20, left >> 20);
	for (uint32_t n = 0; n < 4 && used < length; n++) {
		struct mali_stat * big = NULL;

		for (uint32_t i = 0; i < MALI_STAT_CLIENTS; i++) {
			if (copy[i].pid != 0 && (big == NULL || copy[i].own + copy[i].left > big->own + big->left)) {
				big = &copy[i];
			}
		}
		if (big == NULL || (big->own + big->left) < (1 << 20)) {
			break;
		}
		used += (uint32_t) snprintf(out + used, length - used, "%s %.12s[%d] %llu", n ? "," : ";", big->name, big->pid,
		    (big->own + big->left) >> 20);
		big->pid = 0;
	}
}

struct mali_buffer *
RedwoodMaliUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > MALI_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		return NULL;
	}
	return &fBuffers[handle - 1];
}

uint64_t *
RedwoodMaliUserClient::syncobj(uint32_t handle)
{
	if (handle == 0 || handle > MALI_SYNCOBJS || fSyncobjs[handle - 1] == ~0ULL) {
		return NULL;
	}
	return &fSyncobjs[handle - 1];
}

/*
 * A handle and GPU addresses for `size` bytes of a memory descriptor from byte `from` on (wired;
 * whole pages). With the lock held; NULL: no room.
 */
struct mali_buffer *
RedwoodMaliUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t mapFlags, uint32_t * handle)
{
	struct mali_buffer * b = NULL;
	bool mapped = true;
	uint64_t address;

	for (uint32_t n = 0; n < MALI_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % MALI_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL || (address = redwood_mali_va_alloc(size, !(mapFlags & REDWOOD_MALI_MAP_NOEXEC))) == 0) {
		IOLog("RedwoodMali: no %s left for a buffer of %llu bytes\n", b ? "GPU addresses" : "handles", size);
		m2m_klog_self('E', "RES", "gpu buffer of %llu bytes refused: no %s left", size, b ? "GPU addresses" : "handles");
		return NULL;
	}
	for (uint64_t offset = 0; offset < size && mapped;) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);

		if (phys == 0 || length == 0) {
			mapped = false;
			break;
		}
		/* a physical run, a 4 KB page of the GPU's at a time */
		for (uint64_t done = 0; done < length && offset < size && mapped; done += 4096, offset += 4096) {
			mapped = redwood_mali_map(address + offset, phys + done, mapFlags);
		}
	}
	redwood_mali_mapped(address, size);
	if (!mapped) {
		redwood_mali_unmap(address, size);
		redwood_mali_va_free(address, size);
		IOLog("RedwoodMali: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		m2m_klog_self('E', "RES", "gpu buffer of %llu bytes refused: cannot be mapped into the GPU's address space", size);
		return NULL;
	}
	bzero(b, sizeof(*b));
	b->address = address;
	b->size = size;
	b->flags = mapFlags;
	fBufferCount++;
	fBufferBytes += size;
	return b;
}

void
RedwoodMaliUserClient::dropMapping(uint64_t address, uint64_t size)
{
	redwood_mali_unmap(address, size);
	redwood_mali_va_free(address, size);
}

/* With the lock held. A buffer the GPU may still be using is left until it is done. */
void
RedwoodMaliUserClient::freeBuffer(struct mali_buffer * b)
{
	bool busy = b->fence != 0 && !redwood_mali_done(b->fence);

	if (busy && fLeftCount == MALI_LEFT) {
		reap(true);
		busy = b->fence != 0 && !redwood_mali_done(b->fence);
	}
	if (busy && fLeftCount < MALI_LEFT) {
		struct mali_left * left = &fLeft[fLeftCount++];

		left->memory = b->memory;
		left->surface = b->surface;
		left->address = b->address;
		left->size = b->size;
		left->fence = b->fence;
		if (fStat != NULL) {
			fStat->left += b->size;
		}
	} else {
		if (busy) {
			/* (the GPU has stopped answering: its memory stays the GPU's for good) */
			b->memory->retain();
			IOLog("RedwoodMali: the GPU is not done with a buffer of %llu bytes: it stays the GPU's\n", b->size);
		} else {
			dropMapping(b->address, b->size);
		}
		b->memory->release();
		if (b->surface != NULL) {
			MiniIOSurfaceDrop(b->surface);
		}
	}
	if (fStat != NULL) {
		if (b->surface != NULL) {
			fStat->surfaces -= b->size;
		} else {
			fStat->own -= b->size;
		}
	}
	b->memory = NULL;
	b->surface = NULL;
	fBufferCount--;
	fBufferBytes -= b->size;
}

/* With the lock held: the buffers given back early that the GPU is done with now (all of them, if `wait`). */
void
RedwoodMaliUserClient::reap(bool wait)
{
	for (uint32_t i = 0; i < fLeftCount;) {
		struct mali_left * left = &fLeft[i];

		if (wait && !redwood_mali_done(left->fence)) {
			IOLockUnlock(gMaliLock);
			redwood_mali_wait(left->fence, 2000);
			IOLockLock(gMaliLock);
		}
		if (!redwood_mali_done(left->fence)) {
			i++;
			continue;
		}
		dropMapping(left->address, left->size);
		if (fStat != NULL) {
			fStat->left -= left->size;
		}
		left->memory->release();
		if (left->surface != NULL) {
			MiniIOSurfaceDrop(left->surface);
		}
		*left = fLeft[--fLeftCount];
	}
}

IOReturn
RedwoodMaliUserClient::clientClose(void)
{
	IOLockLock(gMaliLock);
	if (fBuffers != NULL) {
		redwood_mali_wait(redwood_mali_last(), 2000);
		for (uint32_t i = 0; i < MALI_BUFFERS; i++) {
			if (fBuffers[i].memory != NULL) {
				freeBuffer(&fBuffers[i]);
			}
		}
		reap(true);
	}
	if (fStat != NULL) {
		fStat->pid = 0;
		fStat = NULL;
	}
	IOLockUnlock(gMaliLock);
	terminate();
	return kIOReturnSuccess;
}

void
RedwoodMaliUserClient::free(void)
{
	if (fStat != NULL) {
		fStat->pid = 0;
	}
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct mali_buffer, MALI_BUFFERS);
	}
	if (fSyncobjs != NULL) {
		IODelete(fSyncobjs, uint64_t, MALI_SYNCOBJS);
	}
	IOUserClient::free();
}

IOReturn
RedwoodMaliUserClient::createBo(struct drm_panfrost_create_bo * request)
{
	uint64_t size = round_page_64(request->size);
	IOBufferMemoryDescriptor * memory;
	struct mali_buffer * b;
	uint32_t mapFlags = (request->flags & PANFROST_BO_NOEXEC) ? REDWOOD_MALI_MAP_NOEXEC : 0;

	if (size == 0 || size > MALI_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	/*
	 * PANFROST_BO_HEAP (the tiler's heap) grows on GPU page faults under Linux; here it is all
	 * there from the start: the GPU never faults on it.
	 */
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	    kIOMapWriteCombineCache, (vm_size_t) size, PAGE_SIZE);
	if (memory == NULL) {
		IOLog("RedwoodMali: no memory for a buffer of %llu bytes (%u buffers, %llu MB with this client)\n", size, fBufferCount,
		    fBufferBytes >> 20);
		m2m_klog_self('E', "RES", "gpu buffer of %llu bytes refused: no memory (%u buffers, %llu MB with this client)", size,
		    fBufferCount, fBufferBytes >> 20);
		return kIOReturnNoMemory;
	}
	IOLockLock(gMaliLock);
	b = newBuffer(memory, 0, size, mapFlags, &request->handle);
	if (b != NULL) {
		b->memory = memory;
		b->kernel = (uint8_t *) memory->getBytesNoCopy();
		request->offset = b->address;
		if (fStat != NULL) {
			fStat->own += size;
		}
	}
	IOLockUnlock(gMaliLock);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

/* An IOSurface's pixels as a buffer (as RedwoodGPU.cpp's surfaceBuffer()): the same surface is the same buffer. */
IOReturn
RedwoodMaliUserClient::surfaceBuffer(struct redwood_surface * request)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	struct mali_buffer * b = NULL;
	IOMemoryDescriptor * memory;
	uint64_t size;

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (s->client != NULL) {
		MiniIOSurfaceDrop(s);
		return kIOReturnUnsupported;
	}
	size = round_page_64(s->info.allocSize);
	memory = IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, size, kIODirectionInOut);
	if (memory == NULL || size == 0 || size > MALI_BUFFER_MAX) {
		OSSafeReleaseNULL(memory);
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	for (uint32_t i = 0; i < 2; i++) {
		bool real = i < s->info.planeCount;

		request->planeOffset[i] = real ? s->planes[i].offset : (i == 0 ? s->info.offset : 0);
		request->planeRowBytes[i] = real ? s->planes[i].bytesPerRow : (i == 0 ? s->info.bytesPerRow : 0);
	}
	if (request->handle == kRedwoodDescribe) {
		request->handle = 0;
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}
	IOLockLock(gMaliLock);
	for (uint32_t i = 0; i < MALI_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
	} else if ((b = newBuffer(s->memory, PAGE_SIZE, size, REDWOOD_MALI_MAP_NOEXEC, &request->handle)) != NULL) {
		b->memory = memory;
		b->kernel = s->kbase + PAGE_SIZE;
		b->surface = s;
		if (fStat != NULL) {
			fStat->surfaces += size;
		}
	}
	IOLockUnlock(gMaliLock);
	if (b == NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

IOReturn
RedwoodMaliUserClient::submit(struct drm_panfrost_submit * request, size_t size)
{
	uint32_t * handles = NULL;
	uint32_t count = request->bo_handle_count;
	uint64_t fence;
	uint64_t * out;

	if (size < offsetof(struct drm_panfrost_submit, jm_ctx_handle) || request->jc == 0 || count > MALI_SUBMIT_BOS) {
		return kIOReturnBadArgument;
	}
	if (count != 0) {
		handles = IONew(uint32_t, count);
		if (handles == NULL) {
			return kIOReturnNoMemory;
		}
		if (copyin((user_addr_t) request->bo_handles, handles, count * sizeof(uint32_t)) != 0) {
			IODelete(handles, uint32_t, count);
			return kIOReturnVMError;
		}
	}
	IOLockLock(gMaliLock);
	/* IOSurfaces: out of this processor's cache when somebody has said they changed, and always the first time */
	for (uint32_t i = 0; i < count; i++) {
		struct mali_buffer * b = buffer(handles[i]);

		if (b != NULL && b->surface != NULL) {
			uint32_t seed = b->surface->shared()->seed;

			if (seed != b->seed || b->fence == 0) {
				b->seed = seed;
				redwood_mali_to_memory(b->kernel, b->size);
			}
		}
	}
	redwood_mali_to_memory(NULL, 0);                /* (and the program's writes to memory first: a barrier) */
	fence = redwood_mali_submit(request->jc, request->requirements);
	if (fence != 0) {
		for (uint32_t i = 0; i < count; i++) {
			struct mali_buffer * b = buffer(handles[i]);

			if (b != NULL) {
				b->fence = fence;
				if (b->surface != NULL) {
					b->surface->drawn = (uint32_t) fence;
				}
			}
		}
		if (request->out_sync != 0 && (out = syncobj(request->out_sync)) != NULL) {
			*out = fence;
		}
		if (size >= sizeof(struct redwood_mali_submit_ex)) {
			((struct redwood_mali_submit_ex *) request)->seqno = fence;
		}
	}
	reap(false);
	if (fSubmits++ < 4) {
		IOLog("RedwoodMali: job %llu: chain %#llx, %u buffers, requirements %#x\n", fence, request->jc, count,
		    request->requirements);
	}
	IOLockUnlock(gMaliLock);
	if (handles != NULL) {
		IODelete(handles, uint32_t, count);
	}
	return fence != 0 ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn
RedwoodMaliUserClient::syncobjWait(struct drm_syncobj_wait * request)
{
	uint32_t count = request->count_handles, handles[64];
	uint64_t fences[64], patience = millisecondsUntil(request->timeout_nsec), began = mach_absolute_time(), now;
	bool all = request->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;

	if (count == 0 || count > 64 || copyin((user_addr_t) request->handles, handles, count * sizeof(uint32_t)) != 0) {
		return kIOReturnBadArgument;
	}
	IOLockLock(gMaliLock);
	for (uint32_t i = 0; i < count; i++) {
		uint64_t * s = syncobj(handles[i]);

		if (s == NULL) {
			IOLockUnlock(gMaliLock);
			return kIOReturnBadArgument;
		}
		fences[i] = *s;
	}
	IOLockUnlock(gMaliLock);
	for (;;) {
		uint32_t done = 0;

		for (uint32_t i = 0; i < count; i++) {
			if (fences[i] == 0 || redwood_mali_done(fences[i])) {
				if (done++ == 0) {
					request->first_signaled = i;
				}
			}
		}
		if (all ? done == count : done != 0) {
			return kIOReturnSuccess;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &now);
		if (now / NSEC_PER_MSEC >= patience) {
			static uint32_t told;

			m2m_klog_self('W', "GPU", "fence wait gave up after %llu ms (patience %llu): %u fence(s), first %llu, last submitted %llu, "
			    "first done %d", now / NSEC_PER_MSEC, patience, count, fences[0], redwood_mali_last(),
			    (int) redwood_mali_done(fences[0]));

			if (told++ < 16) {
				char line[200];

				snprintf(line, sizeof(line), "RedwoodMali: a wait gave up after %llu ms (patience %llu, timeout %lld): "
				    "%u fence(s), the first %llu, the last submitted %llu, done %d\n", now / NSEC_PER_MSEC, patience,
				    (long long) request->timeout_nsec, count, fences[0], redwood_mali_last(),
				    (int) redwood_mali_done(fences[0]));
				IOLog("%s", line);
				loader_log_line(line);
			}
			return kIOReturnTimeout;
		}
		/* the last unsignalled one: the job queue is in order, so waiting for it covers the others */
		uint64_t latest = 0;

		for (uint32_t i = 0; i < count; i++) {
			latest = fences[i] > latest ? fences[i] : latest;
		}
		redwood_mali_wait(all ? latest : fences[0], patience - now / NSEC_PER_MSEC);
	}
}

IOReturn
RedwoodMaliUserClient::syncobjArray(struct drm_syncobj_array * request, bool reset)
{
	uint32_t handles[64], count = request->count_handles;

	(void) reset;           /* reset (no fence: unsignalled) and signal are the same here: nothing to wait for */
	if (count == 0 || count > 64 || copyin((user_addr_t) request->handles, handles, count * sizeof(uint32_t)) != 0) {
		return kIOReturnBadArgument;
	}
	IOLockLock(gMaliLock);
	for (uint32_t i = 0; i < count; i++) {
		uint64_t * s = syncobj(handles[i]);

		if (s != NULL) {
			*s = 0;
		}
	}
	IOLockUnlock(gMaliLock);
	return kIOReturnSuccess;
}

IOReturn
RedwoodMaliUserClient::show(const struct redwood_show * request)
{
	uint64_t size = (uint64_t) request->rowBytes * request->height;
	uint8_t * pixels;
	IOReturn ret = kIOReturnSuccess;

	if (request->width == 0 || request->width > 4096 || request->height == 0 || request->height > 4096 ||
	    request->rowBytes < 4 * request->width) {
		return kIOReturnBadArgument;
	}
	pixels = (uint8_t *) IOMallocData(size);
	if (pixels == NULL) {
		return kIOReturnNoMemory;
	}
	if (copyin((user_addr_t) request->pixels, pixels, size) != 0) {
		ret = kIOReturnVMError;
	} else {
		MiniIOMobileFramebufferShow(pixels, request->width, request->height, request->rowBytes);
	}
	IOFreeData(pixels, size);
	return ret;
}

/*
 * Client memory type 0 (never a buffer's handle): one page every client may read, with the GPU's completed
 * job count in its first 8 bytes (redwood_mali_share_completed). Made at the first request and kept.
 */
static IOBufferMemoryDescriptor * gMaliStatus;

static IOMemoryDescriptor *
statusPage(void)
{
	if (gMaliStatus == NULL) {
		IOBufferMemoryDescriptor * page = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
		    kIODirectionInOut | kIOMemoryKernelUserShared, PAGE_SIZE, PAGE_SIZE);

		if (page == NULL) {
			return NULL;
		}
		bzero(page->getBytesNoCopy(), PAGE_SIZE);
		gMaliStatus = page;
		redwood_mali_share_completed((volatile uint64_t *) page->getBytesNoCopy());
	}
	return gMaliStatus;
}

IOReturn
RedwoodMaliUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	struct mali_buffer * b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gMaliLock);
	if (type == 0) {
		IOMemoryDescriptor * page = statusPage();

		if (page != NULL) {
			page->retain();
			*memory = page;
			*options = kIOMapReadOnly;
			ret = kIOReturnSuccess;
		}
		IOLockUnlock(gMaliLock);
		return ret;
	}
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		*memory = b->memory;
		/* a surface in the display pool is memory outside XNU's: device memory unless asked otherwise (MiniIOSurface.cpp) */
		*options = b->surface != NULL && b->surface->poolSlot >= 0 ? kIOMapWriteCombineCache : 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gMaliLock);
	return ret;
}

IOReturn
RedwoodMaliUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct drm_panfrost_submit submit;
		struct redwood_mali_submit_ex submitEx;
		struct drm_panfrost_wait_bo waitBo;
		struct drm_panfrost_create_bo createBo;
		struct drm_panfrost_mmap_bo mmapBo;
		struct drm_panfrost_get_param param;
		struct drm_panfrost_get_bo_offset boOffset;
		struct drm_panfrost_madvise madvise;
		struct drm_gem_close close;
		struct drm_syncobj_create syncCreate;
		struct drm_syncobj_destroy syncDestroy;
		struct drm_syncobj_wait syncWait;
		struct drm_syncobj_array syncArray;
		struct redwood_syncobj_fence syncFence;
		struct redwood_mali_counts counts;
		struct redwood_surface surface;
		struct redwood_show show;
	} r;
	size_t size = args->structureInputSize;
	struct mali_buffer * b;
	uint64_t * s;
	IOReturn ret = kIOReturnSuccess;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!redwood_mali_ready() && selector != kRedwoodShow) {
		return kIOReturnNotReady;
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	switch (selector) {
	case kPanfrostGetParam:
		ret = redwood_mali_param(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case kPanfrostCreateBo:
		ret = createBo(&r.createBo);
		break;
	case kPanfrostMmapBo:                           /* (mapped by handle: redwood_drm_map()) */
	case kPanfrostGetBoOffset:
		IOLockLock(gMaliLock);
		b = buffer(r.boOffset.handle);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else if (selector == kPanfrostMmapBo) {
			r.mmapBo.offset = (uint64_t) r.mmapBo.handle << 32;
		} else {
			r.boOffset.offset = b->address;
		}
		IOLockUnlock(gMaliLock);
		break;
	case kPanfrostWaitBo: {
		uint64_t fence = 0;

		IOLockLock(gMaliLock);
		b = buffer(r.waitBo.handle);
		if (b != NULL) {
			fence = b->fence;
		}
		IOLockUnlock(gMaliLock);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else if (fence != 0 && !redwood_mali_wait(fence, millisecondsUntil(r.waitBo.timeout_ns))) {
			ret = kIOReturnTimeout;
		}
		break;
	}
	case kPanfrostSubmit:
		ret = submit(&r.submit, size);
		break;
	case kPanfrostMadvise:
		r.madvise.retained = 1;
		break;
	case kPanfrostSetLabelBo:
		break;
	case kDrmGemClose:
		IOLockLock(gMaliLock);
		b = buffer(r.close.handle);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else {
			freeBuffer(b);
			reap(false);
		}
		IOLockUnlock(gMaliLock);
		break;
	case kDrmSyncobjCreate:
		ret = kIOReturnNoResources;
		IOLockLock(gMaliLock);
		for (uint32_t i = 0; i < MALI_SYNCOBJS; i++) {
			if (fSyncobjs[i] == ~0ULL) {
				fSyncobjs[i] = 0;               /* signalled or not, there is nothing to wait for yet */
				r.syncCreate.handle = i + 1;
				ret = kIOReturnSuccess;
				break;
			}
		}
		IOLockUnlock(gMaliLock);
		break;
	case kDrmSyncobjDestroy:
		IOLockLock(gMaliLock);
		if ((s = syncobj(r.syncDestroy.handle)) != NULL) {
			*s = ~0ULL;
		} else {
			ret = kIOReturnBadArgument;
		}
		IOLockUnlock(gMaliLock);
		break;
	case kDrmSyncobjWait:
		ret = syncobjWait(&r.syncWait);
		break;
	case kDrmSyncobjReset:
	case kDrmSyncobjSignal:
		ret = syncobjArray(&r.syncArray, selector == kDrmSyncobjReset);
		break;
	case kRedwoodSyncobjSet:
	case kRedwoodSyncobjGet:
		IOLockLock(gMaliLock);
		if ((s = syncobj(r.syncFence.handle)) == NULL) {
			ret = kIOReturnBadArgument;
		} else if (selector == kRedwoodSyncobjSet) {
			*s = r.syncFence.fence;
		} else {
			r.syncFence.fence = *s;
		}
		IOLockUnlock(gMaliLock);
		break;
	case kRedwoodMaliCounts:
		redwood_mali_counts(&r.counts.jobs, &r.counts.faults, &r.counts.resets);
		r.counts.last = redwood_mali_last_fault();      /* (the chain that last faulted: Panfrost's fault dump finds its own) */
		break;
	case kRedwoodSurface:
		ret = surfaceBuffer(&r.surface);
		break;
	case kRedwoodShow:
		ret = show(&r.show);
		break;
	default:
		IOLog("RedwoodMali: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
		if (args->structureOutputSize > size) {
			args->structureOutputSize = (uint32_t) size;
		}
		memcpy(args->structureOutput, &r, args->structureOutputSize);
	}
	return ret;
}
