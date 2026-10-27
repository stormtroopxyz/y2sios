/*
 * mali2metal's log (m2m): one channel for the Metal driver's evidence, apart from the console
 * (sys/redwood_m2m.h for the record format, PLANS/10-mali2metal.md for the categories and the
 * workflow around it).
 *
 * Why a channel of its own: the console carries every daemon's chatter (a run's log is two thirds
 * launchd lookups), the loader keeps only its beginning and its end, and the Metal driver's lines
 * there were found afterwards with a grep. Here a record is written whole, with the time and the
 * process the kernel knows rather than one the writer claims, kept in a 1 MB ring, and read whole
 * or from a point (kern.m2m_log, kern.m2m_total; rwlogd serves it as GET /m2m).
 *
 * Writers:
 *   user space   write(2) on /dev/m2m, one record per write: "<sev> <CAT> <message>" (userland/src/m2m.h).
 *                A process that says it is a client (ioctl M2M_IOC_CLIENT) is reported when it ends:
 *                "F CRASH" if a signal ended it, "I DEV ... exited" otherwise.
 *   the kernel   m2m_klog() / m2m_klog_for(): GPU faults and timeouts (redwood_mali.c, RedwoodMali.cpp),
 *                the GPU's time every 5 s.
 *
 * A writer that floods is held to 400 records a second; what it lost is said once the second is over.
 * When the kernel returns to the loader (a panic included), the ring's last 128 KB are appended to the
 * log the loader saves, so a run that ends without a network still leaves them.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <miscfs/devfs/devfs.h>
#include <stdarg.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <sys/redwood_m2m.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/wait.h>

extern void loader_log_putc(char c);

#define M2M_RING                (1024 * 1024)
#define M2M_TO_LOADER           (128 * 1024)
#define M2M_PER_SECOND          400
#define M2M_CLIENTS             32
#define M2M_WRITERS             16

static LCK_GRP_DECLARE(m2m_lck_grp, "redwood-m2m");
static LCK_SPIN_DECLARE(m2m_lock, &m2m_lck_grp);

static char *ring;
static uint64_t ring_total;             /* bytes ever written; the ring holds the last M2M_RING of them */
static uint64_t record_seq;

static struct {
	int pid;
	uint64_t second;
	uint32_t count, dropped;
	char comm[MAXCOMLEN + 1];
} writers[M2M_WRITERS];

static struct {
	int pid;
	char comm[MAXCOMLEN + 1];
	char role[16];
	uint64_t held_at;               /* ms: when it was suspended, 0 while it runs */
} clients[M2M_CLIENTS];
static unsigned client_count;

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

/* with the lock held */
static void
ring_put(const char *s, size_t n)
{
	size_t at = (size_t)(ring_total % M2M_RING), first = n < M2M_RING - at ? n : M2M_RING - at;

	memcpy(ring + at, s, first);
	memcpy(ring, s + first, n - first);
	ring_total += n;
}

/* with the lock held: one whole record */
static void
record_put(uint64_t ms, char sev, const char *cat, const char *proc, int pid, const char *msg, size_t len)
{
	char head[96];
	int h;

	h = snprintf(head, sizeof(head), "%llu %llu.%03llu %c %-5s %s[%d] ", ++record_seq, ms / 1000, ms % 1000, sev, cat, proc, pid);
	if (h < 0) {
		return;
	}
	ring_put(head, (size_t)h < sizeof(head) ? (size_t)h : sizeof(head) - 1);
	ring_put(msg, len);
	ring_put("\n", 1);
}

static bool
valid_sev(char c)
{
	return c == 'I' || c == 'W' || c == 'E' || c == 'F';
}

static void
klog_v(int pid, const char *proc, char sev, const char *cat, const char *fmt, va_list ap)
{
	char msg[M2M_RECORD_MAX];
	int n;

	if (ring == NULL) {
		return;
	}
	n = vsnprintf(msg, sizeof(msg), fmt, ap);
	if (n < 0) {
		return;
	}
	if ((size_t)n >= sizeof(msg)) {
		n = sizeof(msg) - 1;
	}
	while (n > 0 && msg[n - 1] == '\n') {
		n--;
	}
	lck_spin_lock(&m2m_lock);
	record_put(now_ms(), sev, cat, proc, pid, msg, (size_t)n);
	lck_spin_unlock(&m2m_lock);
}

void
m2m_klog(char sev, const char *cat, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	klog_v(0, "kernel", sev, cat, fmt, ap);
	va_end(ap);
}

void
m2m_klog_for(int pid, const char *proc, char sev, const char *cat, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	klog_v(pid, proc, sev, cat, fmt, ap);
	va_end(ap);
}

void
m2m_klog_self(char sev, const char *cat, const char *fmt, ...)
{
	char comm[MAXCOMLEN + 1];
	va_list ap;

	proc_selfname(comm, sizeof(comm));
	va_start(ap, fmt);
	klog_v(proc_selfpid(), comm, sev, cat, fmt, ap);
	va_end(ap);
}

/*
 * One line a user wrote: "<sev> <CAT> <message>". A line without that head is kept as "I LOG", so
 * nothing written is lost for being badly made; the reader's report counts those (they are a bug in
 * the writer). With the lock held.
 */
static void
user_line(uint64_t ms, const char *proc, int pid, const char *line, size_t len)
{
	char sev = 'I', cat[6] = "LOG";
	size_t c = 0;

	if (len >= 4 && valid_sev(line[0]) && line[1] == ' ') {
		while (c < 5 && 2 + c < len && line[2 + c] >= 'A' && line[2 + c] <= 'Z') {
			c++;
		}
		if (c >= 2 && 2 + c < len && line[2 + c] == ' ') {
			sev = line[0];
			memcpy(cat, line + 2, c);
			cat[c] = '\0';
			line += 3 + c;
			len -= 3 + c;
		}
	}
	record_put(ms, sev, cat, proc, pid, line, len);
}

/* With the lock held: may this writer have another record this second? Says what a writer lost. */
static bool
writer_allowed(int pid, const char *comm, uint64_t ms)
{
	unsigned i, free_slot = M2M_WRITERS, oldest = 0;
	uint64_t second = ms / 1000;

	for (i = 0; i < M2M_WRITERS; i++) {
		if (writers[i].pid == pid) {
			break;
		}
		if (writers[i].pid == 0 && free_slot == M2M_WRITERS) {
			free_slot = i;
		}
		if (writers[i].second < writers[oldest].second) {
			oldest = i;
		}
	}
	if (i == M2M_WRITERS) {
		i = free_slot != M2M_WRITERS ? free_slot : oldest;
		writers[i].pid = pid;
		writers[i].second = second;
		writers[i].count = 0;
		writers[i].dropped = 0;
		strlcpy(writers[i].comm, comm, sizeof(writers[i].comm));
	}
	if (writers[i].second != second) {
		if (writers[i].dropped != 0) {
			char msg[96];
			int n = snprintf(msg, sizeof(msg), "flood: %u records dropped in second %llu (over %u a second)",
			    writers[i].dropped, writers[i].second, M2M_PER_SECOND);

			record_put(ms, 'W', "M2M", writers[i].comm, pid, msg, (size_t)n);
		}
		writers[i].second = second;
		writers[i].count = 0;
		writers[i].dropped = 0;
	}
	if (writers[i].count >= M2M_PER_SECOND) {
		writers[i].dropped++;
		return false;
	}
	writers[i].count++;
	return true;
}

static int
m2m_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
m2m_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
m2m_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	char buf[M2M_RECORD_MAX + 16], comm[MAXCOMLEN + 1];
	int pid = proc_selfpid(), error;
	size_t len, start = 0;
	uint64_t ms = now_ms();

	if (ring == NULL) {
		return ENXIO;
	}
	len = uio_resid(uio) < (user_ssize_t)sizeof(buf) ? (size_t)uio_resid(uio) : sizeof(buf);
	error = uiomove(buf, (int)len, uio);
	if (error != 0) {
		return error;
	}
	uio_setresid(uio, 0);           /* the rest of an over-long record is cut, not left for another write */
	proc_selfname(comm, sizeof(comm));
	lck_spin_lock(&m2m_lock);
	for (size_t i = 0; i <= len; i++) {
		if (i == len || buf[i] == '\n') {
			size_t n = i - start;

			if (n > M2M_RECORD_MAX) {
				n = M2M_RECORD_MAX;
			}
			if (n != 0 && writer_allowed(pid, comm, ms)) {
				user_line(ms, comm, pid, buf + start, n);
			}
			start = i + 1;
		}
	}
	lck_spin_unlock(&m2m_lock);
	return 0;
}

/*
 * The processors: at the first client how many are online so far, and once every core has been started
 * (iokit/Kernel/GenericARMPE.cpp starts the slower ones 28 s after boot: redwood_m2m_cores) the count and
 * each core's identity line (osfmk/arm64/cpu.c, cpu-ident) -- the evidence for anything that happens on one
 * kind of core only.
 */
extern void redwood_cpu_idents(void (*each)(const char *line));
extern uint32_t processor_avail_count;
extern unsigned int ml_get_cpu_count(void);

static void
m2m_core_line(const char *line)
{
	char trimmed[320];
	size_t n;

	strlcpy(trimmed, line, sizeof(trimmed));
	n = strlen(trimmed);
	while (n > 0 && trimmed[n - 1] == '\n') {
		trimmed[--n] = '\0';
	}
	m2m_klog('I', "DEV", "%s", trimmed);
}

static void
m2m_cores_once(void)
{
	static int said;

	if (__atomic_exchange_n(&said, 1, __ATOMIC_RELAXED)) {
		return;
	}
	m2m_klog('I', "DEV", "processors: %u online of %u so far", processor_avail_count, ml_get_cpu_count());
}

void
redwood_m2m_cores(void)
{
	m2m_klog('I', "DEV", "processors: %u online of %u, every core started", processor_avail_count, ml_get_cpu_count());
	redwood_cpu_idents(m2m_core_line);
}

static int
m2m_ioctl(__unused dev_t dev, u_long command, caddr_t data, __unused int flags, __unused struct proc *p)
{
	struct m2m_client *c = (struct m2m_client *)data;
	char comm[MAXCOMLEN + 1], role[sizeof(c->role)];
	int pid = proc_selfpid();
	unsigned i, slot = M2M_CLIENTS;

	if (command != M2M_IOC_CLIENT) {
		return ENOTTY;
	}
	strlcpy(role, c->role, sizeof(role));
	proc_selfname(comm, sizeof(comm));
	lck_spin_lock(&m2m_lock);
	for (i = 0; i < M2M_CLIENTS; i++) {
		if (clients[i].pid == pid) {
			slot = i;
			break;
		}
		if (clients[i].pid == 0 && slot == M2M_CLIENTS) {
			slot = i;
		}
	}
	if (slot != M2M_CLIENTS) {
		if (clients[slot].pid != pid) {
			client_count++;
		}
		clients[slot].pid = pid;
		strlcpy(clients[slot].comm, comm, sizeof(clients[slot].comm));
		strlcpy(clients[slot].role, role, sizeof(clients[slot].role));
	}
	lck_spin_unlock(&m2m_lock);
	if (slot == M2M_CLIENTS) {
		m2m_klog_for(pid, comm, 'W', "M2M", "client table full (%u): this process's end will not be reported", M2M_CLIENTS);
	} else {
		m2m_klog_for(pid, comm, 'I', "DEV", "client: %s", role);
	}
	m2m_cores_once();
	return 0;
}

void
redwood_m2m_proc_exit(int pid, const char *comm, int status)
{
	char role[16] = "";
	bool found = false;

	if (client_count == 0) {
		return;
	}
	lck_spin_lock(&m2m_lock);
	for (unsigned i = 0; i < M2M_CLIENTS; i++) {
		if (clients[i].pid == pid) {
			strlcpy(role, clients[i].role, sizeof(role));
			clients[i].pid = 0;
			client_count--;
			found = true;
			break;
		}
	}
	lck_spin_unlock(&m2m_lock);
	if (!found) {
		return;
	}
	if (WIFSIGNALED(status)) {
		m2m_klog_for(pid, comm, 'F', "CRASH", "%s client ended by signal %d%s", role, WTERMSIG(status),
		    WCOREDUMP(status) ? " (core)" : "");
	} else {
		m2m_klog_for(pid, comm, 'I', "DEV", "%s client exited with status %d", role, WEXITSTATUS(status));
	}
}

/*
 * From osfmk/kern/task.c: a Metal client suspended or resumed (RunningBoard parks a background daemon such as
 * chronod between its jobs). A client's completion cannot run while it is held, so this is what tells "the
 * process was suspended" apart from "its threads were starved" when a completion waits seconds with nothing
 * ahead of it on its queue (PLANS/10-mali2metal.md, item 3).
 */
void
redwood_m2m_task_hold(void *bsd_info, int held, const char *how)
{
	proc_t p = (proc_t)bsd_info;
	char comm[MAXCOMLEN + 1] = "", role[16] = "";
	uint64_t now, was = 0;
	bool found = false;
	int pid;

	if (p == NULL || client_count == 0 || ring == NULL) {
		return;
	}
	pid = proc_pid(p);
	now = now_ms();
	lck_spin_lock(&m2m_lock);
	for (unsigned i = 0; i < M2M_CLIENTS; i++) {
		if (clients[i].pid == pid) {
			strlcpy(comm, clients[i].comm, sizeof(comm));
			strlcpy(role, clients[i].role, sizeof(role));
			was = clients[i].held_at;
			clients[i].held_at = held ? now : 0;
			found = true;
			break;
		}
	}
	lck_spin_unlock(&m2m_lock);
	if (!found) {
		return;
	}
	if (held) {
		m2m_klog_for(pid, comm, 'I', "DEV", "%s client suspended (%s)", role, how);
	} else {
		m2m_klog_for(pid, comm, 'I', "DEV", "%s client resumed (%s) after %llu ms", role, how, was ? now - was : 0);
	}
}

void
redwood_m2m_to_loader(void)
{
	uint64_t total = ring_total, size, start;
	static const char head[] = "\n==== m2m: the Metal channel's last records ====\n";
	static const char tail[] = "==== m2m: end ====\n";
	bool skipping;

	if (ring == NULL || total == 0) {
		return;
	}
	size = total < M2M_TO_LOADER ? total : M2M_TO_LOADER;
	start = total - size;
	skipping = start != 0;          /* from the first whole record */
	for (const char *c = head; *c; c++) {
		loader_log_putc(*c);
	}
	for (uint64_t i = start; i < total; i++) {
		char ch = ring[i % M2M_RING];

		if (skipping) {
			skipping = ch != '\n';
			continue;
		}
		loader_log_putc(ch);
	}
	for (const char *c = tail; *c; c++) {
		loader_log_putc(*c);
	}
}

/* the whole ring, oldest record first, after 8 bytes of the total at the copy */
static int
m2m_log_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char *copy;
	uint64_t total, size, start, skip = 0;
	int error;

	if (req->oldptr == USER_ADDR_NULL) {
		return SYSCTL_OUT(req, NULL, M2M_RING + sizeof(uint64_t));
	}
	if (ring == NULL) {
		return ENXIO;
	}
	copy = kalloc_data(M2M_RING + sizeof(uint64_t), Z_WAITOK);
	if (copy == NULL) {
		return ENOMEM;
	}
	lck_spin_lock(&m2m_lock);
	total = ring_total;
	size = total < M2M_RING ? total : M2M_RING;
	start = total - size;
	for (uint64_t i = 0; i < size; i++) {
		copy[sizeof(uint64_t) + i] = ring[(start + i) % M2M_RING];
	}
	lck_spin_unlock(&m2m_lock);
	if (start != 0) {
		while (skip < size && copy[sizeof(uint64_t) + skip] != '\n') {
			skip++;
		}
		skip = skip < size ? skip + 1 : size;
	}
	memcpy(copy + skip, &total, sizeof(total));
	error = SYSCTL_OUT(req, copy + skip, sizeof(uint64_t) + size - skip);
	kfree_data(copy, M2M_RING + sizeof(uint64_t));
	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, m2m_log, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
    m2m_log_sysctl, "A", "mali2metal's channel: 8 bytes of total, then the records oldest first");
SYSCTL_QUAD(_kern, OID_AUTO, m2m_total, CTLFLAG_RD | CTLFLAG_LOCKED, &ring_total,
    "bytes ever written to mali2metal's channel");

static const struct cdevsw m2m_cdevsw = {
	.d_open = m2m_open, .d_close = m2m_close, .d_read = eno_rdwrt, .d_write = m2m_write,
	.d_ioctl = m2m_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

void
redwood_m2m_init(void)
{
	int major;

	ring = kalloc_data(M2M_RING, Z_WAITOK | Z_ZERO);
	if (ring == NULL) {
		printf("m2m: no memory for the channel\n");
		return;
	}
	major = cdevsw_add(-1, &m2m_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "m2m") == NULL) {
		printf("m2m: cannot create %s\n", M2M_DEVICE);
	}
	m2m_klog('I', "M2M", "channel up: %u KB ring, %u records a second per writer", M2M_RING / 1024, M2M_PER_SECOND);
}
