/*
 * rwlogd: the kernel's log over the network while iOS runs (PLANS/09-standalone.md, P4).
 *
 *   GET /log                  both logs so far, oldest first, as the loader saves them on return: the
 *                             console's (kern.redwood_log), then the message buffer (kern.redwood_dmesg)
 *   GET /tail?log=N&dmesg=M   what each has had written since its character N / M (X-Log-Total and
 *                             X-Dmesg-Total: where to ask from next time); all of one that lost N/M
 *   GET /m2m[?from=N]         mali2metal's channel (kern.m2m_log): all of it, or what came after byte N
 *                             (X-M2M-Total: where to ask from next time)
 *   GET /tour                 start mali2metal's standard tour (/redwood/etc/m2m.tour, run by
 *                             /redwood/sbin/init) unless it is running; nothing else can be run
 *   GET /notify?name=N        post the notification N, which must be com.redwood.metal.<something> (the Metal
 *                             driver's own: summary, prof, trace, snap, dump); nothing else is posted
 *   GET /menu[?to=N]          hand back to the loader: 1 Log access (the default), 2 the menu,
 *                             3 Download Mode (kern.redwood_menu)
 *   GET /               a line saying what this is
 *
 * Port 8080, advertised over Bonjour as "HackiOS" (_http._tcp): `dns-sd -B _http._tcp` finds it, and
 * ../y2s.sh wlog / wtail use it. One connection at a time; a slow client cannot hold it for long.
 */
#include <arpa/inet.h>
#include <dns_sd.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <notify.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define PORT    8080

static int
write_all(int fd, const char *p, size_t n)
{
	while (n > 0) {
		ssize_t w = write(fd, p, n);

		if (w <= 0) {
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

/* a sysctl's whole value, malloc'd */
static char *
fetch(const char *name, size_t *len)
{
	size_t size = 0;
	char *buf;

	if (sysctlbyname(name, NULL, &size, NULL, 0) != 0 || (buf = malloc(size + 1)) == NULL) {
		return NULL;
	}
	if (sysctlbyname(name, buf, &size, NULL, 0) != 0) {
		free(buf);
		return NULL;
	}
	*len = size;
	return buf;
}

/* both logs: the console's (kern.redwood_log) and the message buffer (kern.redwood_dmesg), with their totals */
struct logs {
	char *log, *dmesg;              /* dmesg: 8 bytes of total, then the text */
	size_t log_len, dmesg_len;
	uint64_t log_total, dmesg_total;
};

static int
snapshot(struct logs *l)
{
	size_t t = sizeof(l->log_total);

	memset(l, 0, sizeof(*l));
	sysctlbyname("kern.redwood_log_total", &l->log_total, &t, NULL, 0);
	l->log = fetch("kern.redwood_log", &l->log_len);
	l->dmesg = fetch("kern.redwood_dmesg", &l->dmesg_len);
	if (l->dmesg != NULL && l->dmesg_len >= 8) {
		memcpy(&l->dmesg_total, l->dmesg, 8);
		l->dmesg_len -= 8;
	} else {
		l->dmesg_len = 0;
	}
	return l->log != NULL || l->dmesg_len != 0 ? 0 : -1;
}

/* what came after character `from` of a stream whose snapshot of `len` bytes ends at `total`; all of it if lost */
static size_t
since(uint64_t from, uint64_t total, size_t len)
{
	return from <= total && total - from <= len ? (size_t)(total - from) : len;
}

static void
reply(int fd, const char *status, const char *body)
{
	char head[200];

	snprintf(head, sizeof(head), "HTTP/1.0 %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
	    status, strlen(body));
	if (write_all(fd, head, strlen(head)) == 0) {
		write_all(fd, body, strlen(body));
	}
}

/* GET /m2m[?from=N]: mali2metal's channel, whole or from byte N (sys/redwood_m2m.h) */
static void
serve_m2m(int fd, const char *req)
{
	char head[200], *q;
	size_t len = 0, n;
	uint64_t total = 0, from = 0;
	char *all = fetch("kern.m2m_log", &len);

	if (all == NULL || len < 8) {
		free(all);
		reply(fd, "500 No Channel", "kern.m2m_log: no mali2metal channel in this kernel\n");
		return;
	}
	memcpy(&total, all, 8);
	len -= 8;
	if ((q = strstr(req, "from=")) != NULL && q < strstr(req, " HTTP")) {
		from = strtoull(q + 5, NULL, 10);
	}
	n = q != NULL ? since(from, total, len) : len;
	snprintf(head, sizeof(head), "HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %zu\r\n"
	    "X-M2M-Total: %llu\r\nConnection: close\r\n\r\n", n, (unsigned long long)total);
	if (write_all(fd, head, strlen(head)) == 0) {
		write_all(fd, all + 8 + len - n, n);
	}
	free(all);
}

/* GET /notify?name=com.redwood.metal.X: one of the Metal driver's notifications, and only those */
static void
serve_notify(int fd, const char *req)
{
	static const char prefix[] = "com.redwood.metal.";
	char name[96] = "";
	const char *q = strstr(req, "name=");
	size_t n = 0;

	if (q != NULL) {
		q += 5;
		while (n + 1 < sizeof(name) && q[n] != ' ' && q[n] != '&' && q[n] != '\r' && q[n] != '\n' && q[n] != '\0') {
			name[n] = q[n];
			n++;
		}
		name[n] = '\0';
	}
	if (strncmp(name, prefix, sizeof(prefix) - 1) != 0 || n == sizeof(prefix) - 1) {
		reply(fd, "403 Forbidden", "only com.redwood.metal.* is posted\n");
		return;
	}
	if (notify_post(name) != NOTIFY_STATUS_OK) {
		reply(fd, "500 Not Posted", "notify_post failed\n");
		return;
	}
	reply(fd, "200 OK", "posted\n");
}

/* GET /tour: the standard tour, one at a time; the only thing this daemon runs */
static pid_t tour_pid;

static void
serve_tour(int fd)
{
	static char *const argv[] = { "init", "/redwood/etc/m2m.tour", NULL };
	extern char **environ;

	if (tour_pid > 0 && waitpid(tour_pid, NULL, WNOHANG) == 0) {
		reply(fd, "409 Busy", "the tour is running\n");
		return;
	}
	tour_pid = 0;
	if (posix_spawn(&tour_pid, "/redwood/sbin/init", NULL, NULL, argv, environ) != 0) {
		tour_pid = 0;
		reply(fd, "500 Not Started", "the tour could not be started\n");
		return;
	}
	reply(fd, "200 OK", "tour started\n");
}

static void
serve(int fd)
{
	static const char sep[] = "\n--- kernel message buffer ---\n";
	char req[1024], head[320], *q;
	ssize_t n = read(fd, req, sizeof(req) - 1);
	uint64_t from_log = 0, from_dmesg = 0;
	size_t a, b;
	struct logs l;
	bool tail;

	if (n <= 0) {
		return;
	}
	req[n] = 0;
	if (strncmp(req, "GET /m2m", 8) == 0) {
		serve_m2m(fd, req);
		return;
	}
	if (strncmp(req, "GET /notify", 11) == 0) {
		serve_notify(fd, req);
		return;
	}
	if (strncmp(req, "GET /tour", 9) == 0) {
		serve_tour(fd);
		return;
	}
	/*
	 * GET /menu (or /usb): hand the phone back to the loader's Log access, so a host can read
	 * the log and flash without anyone pressing a key. The answer is sent first; the write to
	 * kern.redwood_menu does not return (the machine leaves the kernel).
	 */
	if (strncmp(req, "GET /menu", 9) == 0 || strncmp(req, "GET /usb", 8) == 0) {
		static const char ok[] = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
		static const char body[] = "returning to the loader\n";
		int what = 1;

		/* /menu?to=N: 1 Log access, 2 the menu, 3 Download Mode (kern.redwood_menu) */
		if (strncmp(req, "GET /menu?to=", 13) == 0) {
			what = atoi(req + 13);
		}

		if (write_all(fd, ok, sizeof(ok) - 1) == 0) {
			write_all(fd, body, sizeof(body) - 1);
		}
		close(fd);
		sysctlbyname("kern.redwood_menu", NULL, NULL, &what, sizeof(what));
		return;
	}
	tail = strncmp(req, "GET /tail", 9) == 0;
	if (!tail && strncmp(req, "GET /log", 8) != 0) {
		static const char what[] = "rwlogd: GET /log (the kernel's logs), GET /tail?log=N&dmesg=M (what is new since), "
		    "GET /m2m[?from=N] (mali2metal's channel), GET /tour (the standard tour), GET /notify?name=com.redwood.metal.X, "
		    "GET /menu[?to=1|2|3] (hand back to the loader: Log access, the menu, Download Mode)\n";

		snprintf(head, sizeof(head), "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n"
		    "Connection: close\r\n\r\n", sizeof(what) - 1);
		if (write_all(fd, head, strlen(head)) == 0) {
			write_all(fd, what, sizeof(what) - 1);
		}
		return;
	}
	if (snapshot(&l) != 0) {
		static const char no[] = "HTTP/1.0 500 No Log\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

		write_all(fd, no, sizeof(no) - 1);
		return;
	}
	if (tail) {
		if ((q = strstr(req, "log=")) != NULL && q < strstr(req, " HTTP")) {
			from_log = strtoull(q + 4, NULL, 10);
		}
		if ((q = strstr(req, "dmesg=")) != NULL && q < strstr(req, " HTTP")) {
			from_dmesg = strtoull(q + 6, NULL, 10);
		}
	}
	a = tail ? since(from_log, l.log_total, l.log_len) : l.log_len;
	b = tail ? since(from_dmesg, l.dmesg_total, l.dmesg_len) : l.dmesg_len;
	snprintf(head, sizeof(head), "HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %zu\r\n"
	    "X-Log-Total: %llu\r\nX-Dmesg-Total: %llu\r\nConnection: close\r\n\r\n", a + (tail ? 0 : sizeof(sep) - 1) + b,
	    (unsigned long long)l.log_total, (unsigned long long)l.dmesg_total);
	if (write_all(fd, head, strlen(head)) == 0 && write_all(fd, l.log + l.log_len - a, a) == 0 &&
	    (tail || write_all(fd, sep, sizeof(sep) - 1) == 0)) {
		write_all(fd, l.dmesg + 8 + l.dmesg_len - b, b);
	}
	free(l.log);
	free(l.dmesg);
}

/* the addresses, said on the console (and so in the log) whenever they change */
static void
say_addresses(void)
{
	static char last[256];
	char now[256] = "";
	struct ifaddrs *list, *i;

	if (getifaddrs(&list) != 0) {
		return;
	}
	for (i = list; i != NULL; i = i->ifa_next) {
		if (i->ifa_addr != NULL && i->ifa_addr->sa_family == AF_INET && strcmp(i->ifa_name, "lo0") != 0) {
			char a[INET_ADDRSTRLEN];

			inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, a, sizeof(a));
			snprintf(now + strlen(now), sizeof(now) - strlen(now), " %s=%s", i->ifa_name, a);
		}
	}
	freeifaddrs(list);
	if (strcmp(now, last) != 0) {
		strlcpy(last, now, sizeof(last));
		printf("rwlogd: the log at http://<address>:%d/log:%s\n", PORT, now[0] ? now : " (no address yet)");
		fflush(stdout);
	}
}

int
main(void)
{
	struct sockaddr_in at = { .sin_len = sizeof(at), .sin_family = AF_INET, .sin_port = htons(PORT) };
	DNSServiceRef bonjour = NULL;
	int s, one = 1;

	signal(SIGPIPE, SIG_IGN);
	s = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (s < 0 || bind(s, (struct sockaddr *)&at, sizeof(at)) != 0 || listen(s, 4) != 0) {
		printf("rwlogd: port %d: %s\n", PORT, strerror(errno));
		return 1;
	}
	if (DNSServiceRegister(&bonjour, 0, 0, "HackiOS", "_http._tcp", NULL, NULL, htons(PORT), 0, NULL, NULL, NULL) !=
	    kDNSServiceErr_NoError) {
		printf("rwlogd: no Bonjour (mDNSResponder not running?): by address only\n");
	}
	for (;;) {
		struct timeval tv = { 2, 0 };
		fd_set fds;

		say_addresses();
		FD_ZERO(&fds);
		FD_SET(s, &fds);
		if (select(s + 1, &fds, NULL, NULL, &(struct timeval){ 30, 0 }) > 0) {
			int c = accept(s, NULL, NULL);

			if (c >= 0) {
				setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
				tv.tv_sec = 30;
				setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
				serve(c);
				close(c);
			}
		}
	}
}
