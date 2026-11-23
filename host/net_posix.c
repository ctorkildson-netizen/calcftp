/* net_posix.c -- net.h's TCP calls over a PC's sockets, so ftpd.c can be
 * tried with real FTP clients. Joining is a no-op; the address is 127.0.0.1. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include "pico/stdlib.h"
#include "net.h"

struct net_conn { int fd; bool used, eof; char peer[16]; uint8_t peek[4096]; size_t pn; };
struct net_listener { int fd; uint16_t port; bool used; };
static net_conn conns[3];
static net_listener ls[4];

const char *net_address(void) { return "127.0.0.1"; }
bool net_join(net_progress p, void *c, char *w, size_t n) { (void)p; (void)c; (void)w; (void)n; return true; }

static net_conn *wrap(int fd, const char *peer)
{
	for (int i = 0; i < 3; i++) if (!conns[i].used) {
		net_conn *c = &conns[i];
		memset(c, 0, sizeof *c);
		c->used = true; c->fd = fd;
		snprintf(c->peer, sizeof c->peer, "%s", peer);
		fcntl(fd, F_SETFL, O_NONBLOCK);
		int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		return c;
	}
	close(fd);
	return NULL;
}

net_conn *net_dial(const char *host, uint16_t port, net_progress p, void *ctx, char *why, size_t n)
{
	(void)p; (void)ctx;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
	inet_pton(AF_INET, host, &a.sin_addr);
	if (connect(fd, (struct sockaddr *)&a, sizeof a)) { snprintf(why, n, "connect: %s", strerror(errno)); close(fd); return NULL; }
	net_conn *c = wrap(fd, host);
	if (!c) snprintf(why, n, "No connection free");
	return c;
}

static void fill(net_conn *c)
{
	if (c->pn == sizeof c->peek || c->eof) return;
	ssize_t r = recv(c->fd, c->peek + c->pn, sizeof c->peek - c->pn, 0);
	if (r > 0) c->pn += (size_t)r;
	else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) c->eof = true;
}

size_t net_conn_waiting(net_conn *c) { if (!c) return 0; fill(c); return c->pn; }
size_t net_conn_read(net_conn *c, uint8_t *b, size_t max)
{
	if (!c || !c->used) return 0;
	fill(c);
	size_t k = c->pn < max ? c->pn : max;
	memcpy(b, c->peek, k);
	memmove(c->peek, c->peek + k, c->pn - k);
	c->pn -= k;
	return k;
}
bool net_conn_write(net_conn *c, const uint8_t *s, size_t n)
{
	if (!c || !c->used) return false;
	while (n) {
		ssize_t w = send(c->fd, s, n, MSG_NOSIGNAL);
		if (w > 0) { s += w; n -= (size_t)w; continue; }
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { usleep(500); continue; }
		return false;
	}
	return true;
}
bool net_conn_flush(net_conn *c, unsigned ms) { (void)ms; return c && c->used; }
bool net_conn_open(net_conn *c) { if (!c || !c->used) return false; fill(c); return !c->eof || c->pn; }
bool net_conn_closed_by_host(net_conn *c) { if (!c || !c->used) return false; fill(c); return c->eof && !c->pn; }
const char *net_conn_peer(net_conn *c) { return c ? c->peer : ""; }
void net_conn_close(net_conn *c) { if (!c || !c->used) return; close(c->fd); c->used = false; }

net_listener *net_listen(uint16_t port, net_progress p, void *ctx, char *why, size_t n)
{
	(void)p; (void)ctx;
	net_listener *l = NULL;
	for (int i = 0; i < 4; i++) if (!ls[i].used) { l = &ls[i]; break; }
	if (!l) { snprintf(why, n, "Too many"); return NULL; }
	int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 2)) { snprintf(why, n, "Port %u is already in use", port); close(fd); return NULL; }
	socklen_t al = sizeof a;
	getsockname(fd, (struct sockaddr *)&a, &al);
	fcntl(fd, F_SETFL, O_NONBLOCK);
	l->fd = fd; l->used = true; l->port = ntohs(a.sin_port);
	return l;
}
uint16_t net_listener_port(net_listener *l) { return l ? l->port : 0; }
net_conn *net_accept(net_listener *l)
{
	if (!l || !l->used) return NULL;
	struct sockaddr_in a; socklen_t al = sizeof a;
	int fd = accept(l->fd, (struct sockaddr *)&a, &al);
	if (fd < 0) return NULL;
	char peer[16]; inet_ntop(AF_INET, &a.sin_addr, peer, sizeof peer);
	return wrap(fd, peer);
}
void net_unlisten(net_listener *l) { if (l && l->used) { close(l->fd); l->used = false; } }
