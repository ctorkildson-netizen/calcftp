/* ftpd.c -- an FTP server for the PicoCalc's SD card: see ftpd.h.
 *
 * The protocol is RFC 959's, with RFC 2428's EPSV and RFC 3659's SIZE and
 * MDTM; listings are in the "ls -l" form every client understands. Paths
 * are kept as absolute FatFs paths ("/docs/a.txt"); the client's are
 * resolved against the current folder, with "." and ".." handled here.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "ff.h"
#include "net.h"
#include "ftpd.h"

#define LINE 512
#define PATH 256
#define CHUNK 4096
#define DATA_WAIT_MS 10000

static net_listener *ctrl_l;		/* port 21 */
static net_conn *cli;			/* the client's commands */
static net_listener *pasv_l;		/* waiting for a passive data connection */
static char port_host[16];		/* or, after PORT, where to connect */
static uint16_t port_port;
static char user_need[33], pass_need[65], user_given[33];
static bool logged_in;
static char cwd[PATH] = "/";
static char rnfr[PATH];
static char line[LINE];
static unsigned line_n;
static int iac;				/* skipping a telnet command in the control stream */
static ftpd_report report;
static void *report_ctx;
static uint8_t buf[CHUNK];

static void tell(ftpd_event e, const char *text, uint32_t bytes, uint32_t total)
{
	if (report) report(e, text, bytes, total, report_ctx);
}

static void reply(const char *fmt, ...)
{
	char r[PATH + 64];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(r, sizeof r - 2, fmt, ap);
	va_end(ap);
	if (n < 0) return;
	if (n > (int)sizeof r - 3) n = sizeof r - 3;
	r[n++] = '\r';
	r[n++] = '\n';
	net_conn_write(cli, (const uint8_t *)r, (size_t)n);
}

/* ------------------------------------------------------------ paths */

/* arg against cwd into out: an absolute path, no ".", "..", or doubled or
   trailing slashes; "/" for the root */
static void resolve(const char *arg, char *out)
{
	char tmp[2 * PATH];
	if (!arg || !*arg) arg = ".";
	if (arg[0] == '/') snprintf(tmp, sizeof tmp, "%s", arg);
	else snprintf(tmp, sizeof tmp, "%s/%s", cwd, arg);
	char *parts[64];
	int np = 0;
	for (char *t = strtok(tmp, "/"); t && np < 64; t = strtok(NULL, "/")) {
		if (!strcmp(t, ".")) continue;
		if (!strcmp(t, "..")) { if (np) np--; continue; }
		parts[np++] = t;
	}
	size_t k = 0;
	out[0] = 0;
	for (int i = 0; i < np && k < PATH - 2; i++) {
		int w = snprintf(out + k, PATH - k, "/%s", parts[i]);
		if (w < 0 || (size_t)w >= PATH - k) break;
		k += (size_t)w;
	}
	if (!np) snprintf(out, PATH, "/");
}

static bool is_dir(const char *p)
{
	if (!strcmp(p, "/")) return true;
	FILINFO fi;
	return f_stat(p, &fi) == FR_OK && (fi.fattrib & AM_DIR);
}

static const char *base(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

/* ------------------------------------------------------------ data connections */

static void data_reset(void)
{
	if (pasv_l) { net_unlisten(pasv_l); pasv_l = NULL; }
	port_host[0] = 0;
}

/* the data connection the client asked for (PASV/EPSV: it connects to us;
   PORT: we connect to it); NULL, with a reply sent, if it can't be had */
static net_conn *data_open(void)
{
	char why[96];
	net_conn *d = NULL;
	if (pasv_l) {
		absolute_time_t until = make_timeout_time_ms(DATA_WAIT_MS);
		while (!(d = net_accept(pasv_l)) && !time_reached(until)) sleep_ms(2);
	} else if (port_host[0]) {
		d = net_dial(port_host, port_port, NULL, NULL, why, sizeof why);
	} else {
		reply("425 Use PASV or PORT first");
		return NULL;
	}
	data_reset();
	if (!d) reply("425 Can't open the data connection");
	return d;
}

static void data_close(net_conn *d)
{
	net_conn_flush(d, 10000);
	net_conn_close(d);
}

/* ------------------------------------------------------------ the commands */

static const char *const months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
					"Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* an MLSD/MLST fact line (RFC 3659): type, size, modify; then the name */
static int facts(char *l, size_t size, const FILINFO *fi, const char *name)
{
	return snprintf(l, size, "type=%s;size=%lu;modify=%04u%02u%02u%02u%02u%02u; %s\r\n",
			(fi->fattrib & AM_DIR) ? "dir" : "file", (unsigned long)fi->fsize,
			1980 + (fi->fdate >> 9), (fi->fdate >> 5) & 15, fi->fdate & 31,
			fi->ftime >> 11, (fi->ftime >> 5) & 63, (fi->ftime & 31) * 2, name);
}

enum { LS_LONG, LS_NAMES, LS_FACTS };

static void cmd_list(const char *arg, int how)
{
	/* "LIST -la" and the like: the options aren't ours to honour */
	while (arg && *arg == '-') { arg = strchr(arg, ' '); if (arg) while (*arg == ' ') arg++; }
	char p[PATH];
	resolve(arg, p);
	FILINFO fi;
	bool single = !is_dir(p);
	if (single && f_stat(p, &fi) != FR_OK) { reply("550 No such file or folder"); return; }
	net_conn *d = data_open();
	if (!d) return;
	reply("150 Here comes the listing");
	DIR dir;
	bool ok = true;
	if (!single && f_opendir(&dir, p) != FR_OK) ok = false;
	for (;;) {
		if (!single) {
			if (!ok || f_readdir(&dir, &fi) != FR_OK || !fi.fname[0]) break;
			if (fi.fattrib & (AM_HID | AM_SYS)) continue;
		}
		char l[PATH + 80];
		int n;
		if (how == LS_NAMES) n = snprintf(l, sizeof l, "%s\r\n", fi.fname);
		else if (how == LS_FACTS) n = facts(l, sizeof l, &fi, fi.fname);
		else {
			unsigned mon = (fi.fdate >> 5) & 15, day = fi.fdate & 31, year = 1980 + (fi.fdate >> 9);
			n = snprintf(l, sizeof l, "%s 1 picocalc picocalc %10lu %s %2u  %4u %s\r\n",
				     (fi.fattrib & AM_DIR) ? "drwxr-xr-x" : "-rw-r--r--",
				     (unsigned long)fi.fsize, months[(mon >= 1 && mon <= 12) ? mon - 1 : 0],
				     day ? day : 1, year, fi.fname);
		}
		if (n > 0) net_conn_write(d, (const uint8_t *)l, (size_t)n);
		if (single) break;
	}
	if (!single && ok) f_closedir(&dir);
	data_close(d);
	reply(ok ? "226 Done" : "451 Couldn't read the folder");
}

static void cmd_retr(const char *arg)
{
	char p[PATH];
	resolve(arg, p);
	FIL f;
	if (is_dir(p) || f_open(&f, p, FA_READ) != FR_OK) { reply("550 No such file"); return; }
	uint32_t total = (uint32_t)f_size(&f), done = 0;
	net_conn *d = data_open();
	if (!d) { f_close(&f); return; }
	reply("150 Sending %s (%lu bytes)", base(p), (unsigned long)total);
	bool ok = true;
	for (;;) {
		UINT got = 0;
		if (f_read(&f, buf, sizeof buf, &got) != FR_OK) { ok = false; break; }
		if (!got) break;
		if (!net_conn_write(d, buf, got)) { ok = false; break; }
		done += got;
		tell(FTPD_SENDING, base(p), done, total);
	}
	f_close(&f);
	if (ok) ok = net_conn_flush(d, 10000);
	net_conn_close(d);
	if (ok) { reply("226 Sent"); tell(FTPD_SENT, p, done, total); }
	else { reply("426 The transfer broke off"); tell(FTPD_FAILED, "a transfer broke off", done, total); }
}

static void cmd_stor(const char *arg, bool append)
{
	char p[PATH];
	resolve(arg, p);
	if (!strcmp(p, "/") || is_dir(p)) { reply("553 That's a folder"); return; }
	FIL f;
	FRESULT fr = f_open(&f, p, append ? (FA_WRITE | FA_OPEN_APPEND) : (FA_WRITE | FA_CREATE_ALWAYS));
	if (fr != FR_OK) { reply(fr == FR_NO_PATH ? "553 No such folder" : "553 Can't create the file"); return; }
	net_conn *d = data_open();
	if (!d) { f_close(&f); f_unlink(p); return; }
	reply("150 Ready for %s", base(p));
	uint32_t done = 0;
	bool ok = true;
	absolute_time_t quiet = make_timeout_time_ms(30000);
	for (;;) {
		size_t got = net_conn_read(d, buf, sizeof buf);
		if (got) {
			UINT put = 0;
			if (f_write(&f, buf, (UINT)got, &put) != FR_OK || put != got) { ok = false; break; }
			done += (uint32_t)got;
			tell(FTPD_RECEIVING, base(p), done, 0);
			quiet = make_timeout_time_ms(30000);
			continue;
		}
		if (net_conn_closed_by_host(d) || !net_conn_open(d)) break;	/* all of it */
		if (time_reached(quiet)) { ok = false; break; }
		sleep_ms(1);
	}
	f_close(&f);
	net_conn_close(d);
	if (ok) { reply("226 Received"); tell(FTPD_RECEIVED, p, done, 0); }
	else { reply("451 Couldn't write it all (is the card full?)"); tell(FTPD_FAILED, "a file couldn't be written", done, 0); }
}

static void cmd_pasv(bool extended)
{
	char why[96];
	data_reset();
	pasv_l = net_listen(0, NULL, NULL, why, sizeof why);
	if (!pasv_l) { reply("425 %s", why); return; }
	uint16_t port = net_listener_port(pasv_l);
	if (extended) { reply("229 Entering Extended Passive Mode (|||%u|)", port); return; }
	unsigned a, b, c, d;
	if (sscanf(net_address(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) { reply("425 No address"); data_reset(); return; }
	reply("227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)", a, b, c, d, port >> 8, port & 255);
}

static void cmd_port(const char *arg)
{
	unsigned h[4], p[2];
	data_reset();
	if (!arg || sscanf(arg, "%u,%u,%u,%u,%u,%u", &h[0], &h[1], &h[2], &h[3], &p[0], &p[1]) != 6) {
		reply("501 PORT h1,h2,h3,h4,p1,p2");
		return;
	}
	snprintf(port_host, sizeof port_host, "%u.%u.%u.%u", h[0], h[1], h[2], h[3]);
	port_port = (uint16_t)(p[0] * 256 + p[1]);
	reply("200 PORT taken");
}

static void command(char *l)
{
	char *arg = strchr(l, ' ');
	if (arg) { *arg++ = 0; while (*arg == ' ') arg++; if (!*arg) arg = NULL; }
	for (char *c = l; *c; c++) *c = (char)toupper((unsigned char)*c);
	char p[PATH];
	FILINFO fi;

	if (!strcmp(l, "USER")) {
		snprintf(user_given, sizeof user_given, "%s", arg ? arg : "");
		logged_in = false;
		reply("331 Password, please");
		return;
	}
	if (!strcmp(l, "PASS")) {
		if (!user_need[0] || (!strcmp(user_given, user_need) && !strcmp(arg ? arg : "", pass_need))) {
			logged_in = true;
			reply("230 Logged in");
			tell(FTPD_LOGGED_IN, user_given, 0, 0);
		} else {
			reply("530 Wrong user name or password");
			tell(FTPD_BAD_LOGIN, user_given, 0, 0);
		}
		return;
	}
	if (!strcmp(l, "QUIT")) {
		reply("221 Goodbye");
		net_conn_flush(cli, 2000);
		net_conn_close(cli);
		cli = NULL;
		data_reset();
		tell(FTPD_DISCONNECTED, "", 0, 0);
		return;
	}
	if (!strcmp(l, "SYST")) { reply("215 UNIX Type: L8"); return; }
	if (!strcmp(l, "FEAT")) {
		static const char f[] = "211-Features:\r\n SIZE\r\n MDTM\r\n MLST type*;size*;modify*;\r\n UTF8\r\n EPSV\r\n PASV\r\n211 End\r\n";
		net_conn_write(cli, (const uint8_t *)f, sizeof f - 1);
		return;
	}
	if (!strcmp(l, "NOOP")) { reply("200 OK"); return; }
	if (!strcmp(l, "OPTS")) { reply("200 OK"); return; }		/* UTF8 ON: names are UTF-8 already */
	if (!logged_in) { reply("530 Log in first"); return; }

	if (!strcmp(l, "PWD") || !strcmp(l, "XPWD")) { reply("257 \"%s\" is the current folder", cwd); return; }
	if (!strcmp(l, "CWD") || !strcmp(l, "XCWD") || !strcmp(l, "CDUP") || !strcmp(l, "XCUP")) {
		bool up = !strcmp(l, "CDUP") || !strcmp(l, "XCUP");
		resolve(up ? ".." : arg, p);
		if (!is_dir(p)) { reply("550 No such folder"); return; }
		snprintf(cwd, sizeof cwd, "%s", p);
		reply("250 Now in %s", cwd);
		return;
	}
	if (!strcmp(l, "TYPE")) { reply("200 Binary it is"); return; }	/* A or I: sent as stored */
	if (!strcmp(l, "MODE") || !strcmp(l, "STRU")) { reply("200 OK"); return; }
	if (!strcmp(l, "PASV")) { cmd_pasv(false); return; }
	if (!strcmp(l, "EPSV")) { cmd_pasv(true); return; }
	if (!strcmp(l, "PORT")) { cmd_port(arg); return; }
	if (!strcmp(l, "LIST")) { cmd_list(arg, LS_LONG); return; }
	if (!strcmp(l, "NLST")) { cmd_list(arg, LS_NAMES); return; }
	if (!strcmp(l, "MLSD")) {
		resolve(arg, p);
		if (!is_dir(p)) { reply("501 Not a folder"); return; }
		cmd_list(arg, LS_FACTS);
		return;
	}
	if (!strcmp(l, "MLST")) {
		resolve(arg, p);
		if (!strcmp(p, "/")) { memset(&fi, 0, sizeof fi); fi.fattrib = AM_DIR; }
		else if (f_stat(p, &fi) != FR_OK) { reply("550 No such file or folder"); return; }
		char f[PATH + 96];
		facts(f, sizeof f, &fi, p);
		reply("250-Listing %s", p);
		net_conn_write(cli, (const uint8_t *)" ", 1);
		net_conn_write(cli, (const uint8_t *)f, strlen(f));
		reply("250 End");
		return;
	}
	if (!strcmp(l, "RETR")) { cmd_retr(arg); return; }
	if (!strcmp(l, "STOR")) { cmd_stor(arg, false); return; }
	if (!strcmp(l, "APPE")) { cmd_stor(arg, true); return; }
	if (!strcmp(l, "SIZE") || !strcmp(l, "MDTM")) {
		resolve(arg, p);
		if (!strcmp(p, "/") || f_stat(p, &fi) != FR_OK || (fi.fattrib & AM_DIR)) { reply("550 No such file"); return; }
		if (l[0] == 'S') reply("213 %lu", (unsigned long)fi.fsize);
		else reply("213 %04u%02u%02u%02u%02u%02u", 1980 + (fi.fdate >> 9), (fi.fdate >> 5) & 15, fi.fdate & 31,
			   fi.ftime >> 11, (fi.ftime >> 5) & 63, (fi.ftime & 31) * 2);
		return;
	}
	if (!strcmp(l, "DELE")) {
		resolve(arg, p);
		if (is_dir(p) || f_unlink(p) != FR_OK) { reply("550 Couldn't delete it"); return; }
		reply("250 Deleted");
		return;
	}
	if (!strcmp(l, "MKD") || !strcmp(l, "XMKD")) {
		resolve(arg, p);
		if (f_mkdir(p) != FR_OK) { reply("550 Couldn't make it"); return; }
		reply("257 \"%s\" made", p);
		return;
	}
	if (!strcmp(l, "RMD") || !strcmp(l, "XRMD")) {
		resolve(arg, p);
		if (!strcmp(p, "/") || !is_dir(p) || f_unlink(p) != FR_OK) { reply("550 Couldn't remove it (is it empty?)"); return; }
		reply("250 Removed");
		return;
	}
	if (!strcmp(l, "RNFR")) {
		resolve(arg, rnfr);
		if (!strcmp(rnfr, "/") || f_stat(rnfr, &fi) != FR_OK) { rnfr[0] = 0; reply("550 No such file"); return; }
		reply("350 And its new name?");
		return;
	}
	if (!strcmp(l, "RNTO")) {
		if (!rnfr[0]) { reply("503 RNFR first"); return; }
		resolve(arg, p);
		FRESULT fr = f_rename(rnfr, p);
		rnfr[0] = 0;
		reply(fr == FR_OK ? "250 Renamed" : "550 Couldn't rename it");
		return;
	}
	if (!strcmp(l, "ABOR")) { data_reset(); reply("226 Nothing to abort"); return; }
	reply("502 %s isn't something this server does", l);
}

/* ------------------------------------------------------------ the server */

bool ftpd_start(uint16_t port, const char *user, const char *password,
		ftpd_report rep, void *ctx, char *why, unsigned n)
{
	snprintf(user_need, sizeof user_need, "%s", user ? user : "");
	snprintf(pass_need, sizeof pass_need, "%s", password ? password : "");
	report = rep;
	report_ctx = ctx;
	ctrl_l = net_listen(port, NULL, NULL, why, n);
	return ctrl_l != NULL;
}

bool ftpd_busy(void) { return cli != NULL; }

void ftpd_poll(void)
{
	net_conn *c;
	while ((c = net_accept(ctrl_l))) {
		if (cli) {			/* one at a time */
			static const char busy[] = "421 Someone else is connected; try again later\r\n";
			net_conn_write(c, (const uint8_t *)busy, sizeof busy - 1);
			net_conn_flush(c, 1000);
			net_conn_close(c);
			continue;
		}
		cli = c;
		logged_in = false;
		snprintf(cwd, sizeof cwd, "/");
		line_n = 0;
		iac = 0;
		rnfr[0] = 0;
		data_reset();
		tell(FTPD_CONNECTED, net_conn_peer(c), 0, 0);
		reply("220 PicoCalc FTP server ready");
	}
	if (!cli) return;
	uint8_t in[128];
	size_t got;
	while (cli && (got = net_conn_read(cli, in, sizeof in))) {
		for (size_t i = 0; i < got && cli; i++) {
			uint8_t ch = in[i];
			if (iac) { iac = (iac == 1 && ch >= 251 && ch <= 254) ? 2 : 0; continue; }	/* telnet IAC x [y] */
			if (ch == 255) { iac = 1; continue; }
			if (ch == '\r') continue;
			if (ch == '\n') {
				line[line_n] = 0;
				line_n = 0;
				if (line[0]) command(line);
				continue;
			}
			if (line_n < LINE - 1) line[line_n++] = (char)ch;
		}
	}
	if (cli && net_conn_closed_by_host(cli)) {
		net_conn_close(cli);
		cli = NULL;
		data_reset();
		tell(FTPD_DISCONNECTED, "", 0, 0);
	}
}

void ftpd_stop(void)
{
	if (cli) { net_conn_close(cli); cli = NULL; }
	data_reset();
	if (ctrl_l) { net_unlisten(ctrl_l); ctrl_l = NULL; }
}
