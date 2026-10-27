/*
 * The kernel's logs for a reader while the system runs (PLANS/09-standalone.md, P4), so that a daemon
 * (userland/src/rwlogd.c) can serve them over the network -- the same two the loader saves when the
 * kernel returns (osfmk/arm64/loader_return.c):
 *
 *   sysctl kern.redwood_log           the console's log so far, oldest first (at most ~6.5 MB), root only
 *   sysctl kern.redwood_log_total     characters ever written to it (a reader keeps its place with it)
 *   sysctl kern.redwood_dmesg         the kernel's message buffer, oldest first (every printf), root only
 *   sysctl kern.redwood_dmesg_total   characters ever written to that
 */
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/msgbuf.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <kern/thread_call.h>
#include <sys/systm.h>

extern uint64_t loader_log_total;
extern size_t loader_log_snapshot(char *out, size_t cap);
extern uint64_t redwood_msgbuf_total;
extern struct msgbuf *msgbufp;
extern void bsd_log_lock_safe(void);
extern void bsd_log_unlock(void);

#define REDWOOD_LOG_MAX (6656 * 1024)

static int
redwood_log_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char *copy;
	size_t n;
	int error;

	if (req->oldptr == USER_ADDR_NULL) {
		return SYSCTL_OUT(req, NULL, REDWOOD_LOG_MAX);
	}
	if (!kauth_cred_issuser(kauth_cred_get())) {
		return EPERM;
	}
	copy = kalloc_data(REDWOOD_LOG_MAX, Z_WAITOK);
	if (copy == NULL) {
		return ENOMEM;
	}
	n = loader_log_snapshot(copy, REDWOOD_LOG_MAX);
	error = SYSCTL_OUT(req, copy, n);
	kfree_data(copy, REDWOOD_LOG_MAX);
	return error;
}

/* the message buffer, oldest first, with the total as it was at the copy (so the two agree) */
static int
redwood_dmesg_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	size_t size = msgbufp != NULL && msgbufp->msg_size > 0 ? (size_t)msgbufp->msg_size : 0, n = 0;
	char *copy;
	int error;

	if (req->oldptr == USER_ADDR_NULL) {
		return SYSCTL_OUT(req, NULL, size + sizeof(uint64_t));
	}
	if (!kauth_cred_issuser(kauth_cred_get())) {
		return EPERM;
	}
	copy = kalloc_data(size + sizeof(uint64_t), Z_WAITOK);
	if (copy == NULL) {
		return ENOMEM;
	}
	bsd_log_lock_safe();
	if (msgbufp->msg_size == (int)size && msgbufp->msg_bufx >= 0 && msgbufp->msg_bufx < (int)size) {
		uint64_t total = redwood_msgbuf_total;

		memcpy(copy, &total, sizeof(total));
		for (int pass = 0; pass < 2; pass++) {
			int from = pass == 0 ? msgbufp->msg_bufx : 0, to = pass == 0 ? (int)size : msgbufp->msg_bufx;

			for (int i = from; i < to; i++) {
				if (msgbufp->msg_bufc[i] != 0) {
					copy[sizeof(uint64_t) + n++] = msgbufp->msg_bufc[i];
				}
			}
		}
	}
	bsd_log_unlock();
	/* the first 8 bytes: the total at the copy; then the text */
	error = SYSCTL_OUT(req, copy, n + sizeof(uint64_t));
	kfree_data(copy, size + sizeof(uint64_t));
	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, redwood_log, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
    redwood_log_sysctl, "A", "the kernel's console log, as the loader saves it");
SYSCTL_QUAD(_kern, OID_AUTO, redwood_log_total, CTLFLAG_RD | CTLFLAG_LOCKED, &loader_log_total,
    "characters ever written to the kernel's console log");
SYSCTL_PROC(_kern, OID_AUTO, redwood_dmesg, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
    redwood_dmesg_sysctl, "A", "the kernel's message buffer: 8 bytes of total, then the text oldest first");
SYSCTL_QUAD(_kern, OID_AUTO, redwood_dmesg_total, CTLFLAG_RD | CTLFLAG_LOCKED, &redwood_msgbuf_total,
    "characters ever written to the kernel's message buffer");

/*
 * kern.redwood_menu: hand the phone back to the loader on purpose, and say what for.
 *
 *   value  what                                            loader reason (CTX_REASON, xnuloader/loader.c)
 *   1      Log access (USB fastboot: read the log, flash)  3
 *   2      the menu, waiting for a key                     0 (an ordinary return)
 *   3      Download Mode                                   4
 *
 * The loader's reasons 1 (early exception) and 2 (checkpoint) are its own and must never be sent
 * from here: a value was passed through unmapped once, and "Log access" (1) arrived as "early
 * exception" and left the phone in the menu. Anything else is refused with EINVAL.
 *
 * Writing it does not return: the machine leaves this kernel for the loader (loader_return_asked),
 * which is the point -- it is how a host takes the phone over with no key press. The caller (the
 * log daemon) sends its answer before writing.
 */
#define LOADER_REASON_RETURNED          0
#define LOADER_REASON_LOG_ACCESS        3
#define LOADER_REASON_DOWNLOAD          4
extern void loader_return_asked(uint64_t reason);

/*
 * The return itself is done on a thread call and not in the sysctl's own context: loader_return()
 * stops the other processors and leaves this kernel for good, which is not something a sysctl
 * handler may do -- called there it panics, and the panic path's own loader_return() is the one
 * that runs, with no reason attached (the menu then shows instead of Log access).
 */
static uint64_t        redwood_menu_reason;
static thread_call_t   redwood_menu_call;

static void
redwood_menu_fire(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	/* (Download Mode is the loader's to do: it has reboot_download(); the same writes from this
	 * kernel's mapping reset the phone but the bootloader did not take the INFORM registers.) */
	loader_return_asked(redwood_menu_reason);
}

static int
redwood_menu_sysctl SYSCTL_HANDLER_ARGS
{
	int value = 0, error;

	error = sysctl_handle_int(oidp, &value, 0, req);
	if (error != 0 || req->newptr == NULL) {
		return error;
	}
	switch (value) {
	case 1:
		redwood_menu_reason = LOADER_REASON_LOG_ACCESS;
		break;
	case 2:
		redwood_menu_reason = LOADER_REASON_RETURNED;
		break;
	case 3:
		redwood_menu_reason = LOADER_REASON_DOWNLOAD;
		break;
	default:
		return EINVAL;
	}
	printf("redwood_menu: %d asked for: returning to the loader with reason %llu\n", value, redwood_menu_reason);
	if (redwood_menu_call == NULL) {
		redwood_menu_call = thread_call_allocate(redwood_menu_fire, NULL);
	}
	if (redwood_menu_call == NULL) {
		return ENOMEM;
	}
	thread_call_enter(redwood_menu_call);
	return 0;
}

SYSCTL_PROC(_kern, OID_AUTO, redwood_menu, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
    redwood_menu_sysctl, "I", "return to the loader: 1 Log access, 2 the menu, 3 Download Mode");
