/* main.c -- the PicoCalc FTP server: the card, over Wi-Fi, to any FTP client.
 *
 * Wi-Fi comes from /ntp/ntp.cfg (picocalc-extras' net/); the login from
 * /ftp/ftp.cfg:
 *
 *     user = picocalc
 *     password = something
 *
 * Without that file, it's made with the earlier PicoCalc FTP server's login:
 * the ftp_user and ftp_password in an old /config_ftp.conf if there is one,
 * or else that server's default, picocalc and picocalc, so FTP clients set
 * up for it carry on working.
 *
 * The screen shows where to connect and what's happening; Esc quits, leaving
 * the network properly so the next program joins at once.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "drivers/picocalc.h"
#include "drivers/keyboard.h"
#include "drivers/lcd.h"
#include "ff.h"
#include "picocalc_shell.h"
#include "net.h"
#include "ftpd.h"

#define CFG_DIR "/ftp"
#define CFG_PATH "/ftp/ftp.cfg"
#define OLD_CFG "/config_ftp.conf"

/* the text starter's keyboard driver sets this on Brk */
volatile bool user_interrupt = false;

static char user[33], password[65];

/* "key = value" (or key=value) lines; # starts a comment */
static bool cfg_value(const char *text, const char *key, char *out, size_t size)
{
	size_t kl = strlen(key);
	for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
		while (*l == ' ' || *l == '\t') l++;
		if (strncmp(l, key, kl)) continue;
		const char *v = l + kl;
		while (*v == ' ' || *v == '\t') v++;
		if (*v != '=') continue;
		v++;
		while (*v == ' ' || *v == '\t') v++;
		size_t n = 0;
		while (v[n] && v[n] != '\n' && v[n] != '\r' && v[n] != '#') n++;
		while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
		snprintf(out, size, "%.*s", (int)n, v);
		return true;
	}
	return false;
}

static bool read_file(const char *path, char *text, size_t size)
{
	FIL f;
	if (f_open(&f, path, FA_READ) != FR_OK) return false;
	UINT got = 0;
	f_read(&f, text, (UINT)size - 1, &got);
	f_close(&f);
	text[got] = 0;
	return true;
}

/* the login: /ftp/ftp.cfg, or the old server's file, or a new one made now */
static const char *login(void)
{
	static char text[1024];
	if (read_file(CFG_PATH, text, sizeof text)) {
		cfg_value(text, "user", user, sizeof user);
		cfg_value(text, "password", password, sizeof password);
		return NULL;
	}
	const char *note;
	snprintf(user, sizeof user, "picocalc");		/* the earlier server's default */
	snprintf(password, sizeof password, "picocalc");
	if (read_file(OLD_CFG, text, sizeof text) && cfg_value(text, "ftp_user", user, sizeof user)) {
		cfg_value(text, "ftp_password", password, sizeof password);
		note = "Login from " OLD_CFG " is now in " CFG_PATH;
	} else {
		note = "Made " CFG_PATH ": change the password there if you like";
	}
	f_mkdir(CFG_DIR);
	FIL f;
	if (f_open(&f, CFG_PATH, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
		char b[256];
		int n = snprintf(b, sizeof b, "# the PicoCalc FTP server's login\nuser = %s\npassword = %s\n", user, password);
		UINT put;
		f_write(&f, b, (UINT)n, &put);
		f_close(&f);
	}
	return note;
}

/* ------------------------------------------------------------ the screen */

#define LOG_TOP 9
#define LOG_ROWS 20

static void at(int row, int col) { printf("\033[%d;%dH", row, col); }

static int width(void)
{
	int w = lcd_get_columns();
	return w > 1 && w <= 80 ? w - 1 : 39;	/* (the last column wraps) */
}

static void status(const char *s)
{
	at(LOG_TOP - 2, 1);
	printf("\033[K%.*s", width(), s);
}

/* a line in the log, wrapped to the screen; the log scrolls round */
static void log_line(const char *s)
{
	static int next;
	int w = width();
	bool first = true;
	do {
		at(LOG_TOP + next, 1);
		printf("\033[K%s%.*s", first ? "" : "  ", first ? w : w - 2, s);
		size_t took = strlen(s) < (size_t)(first ? w : w - 2) ? strlen(s) : (size_t)(first ? w : w - 2);
		s += took;
		first = false;
		next = (next + 1) % LOG_ROWS;
	} while (*s);
	at(LOG_TOP + next, 1);
	printf("\033[K");			/* a blank line after the newest */
}

static bool dots(const char *what, void *ctx)
{
	(void)ctx;
	status(what);
	return true;
}

static void on_event(ftpd_event e, const char *text, uint32_t bytes, uint32_t total, void *ctx)
{
	(void)ctx;
	char l[96];
	static absolute_time_t next;
	switch (e) {
	case FTPD_CONNECTED: snprintf(l, sizeof l, "%s connected", text); log_line(l); status("Connected"); break;
	case FTPD_LOGGED_IN: snprintf(l, sizeof l, "  logged in as %s", text); log_line(l); break;
	case FTPD_BAD_LOGIN: snprintf(l, sizeof l, "  wrong password for \"%s\"", text); log_line(l); break;
	case FTPD_SENDING:
	case FTPD_RECEIVING:
		if (!time_reached(next)) break;		/* (a few times a second is plenty) */
		next = make_timeout_time_ms(250);
		if (total) snprintf(l, sizeof l, "%s %s: %lu of %lu KB", e == FTPD_SENDING ? "Sending" : "Receiving",
				    text, (unsigned long)(bytes / 1024), (unsigned long)(total / 1024));
		else snprintf(l, sizeof l, "Receiving %s: %lu KB", text, (unsigned long)(bytes / 1024));
		status(l);
		break;
	case FTPD_SENT: snprintf(l, sizeof l, "  sent %s (%lu bytes)", text, (unsigned long)bytes); log_line(l); status("Connected"); break;
	case FTPD_RECEIVED: snprintf(l, sizeof l, "  received %s (%lu bytes)", text, (unsigned long)bytes); log_line(l); status("Connected"); break;
	case FTPD_FAILED: snprintf(l, sizeof l, "  %s", text); log_line(l); status("Connected"); break;
	case FTPD_DISCONNECTED: log_line("  disconnected"); status("Waiting for a connection"); break;
	}
}

static void quit(void)
{
	ftpd_stop();
	net_leave();			/* so the next program joins at once */
	picocalc_shell_exit();
}

int main(void)
{
	picocalc_init();
	printf("\033[2J\033[H\033[1mPicoCalc FTP server\033[0m");
	const char *note = login();

	char why[96];
	if (!net_join(dots, NULL, why, sizeof why)) {
		status(why);
		at(LOG_TOP, 1);
		printf("Press a key to go back.");
		getchar();
		picocalc_shell_exit();
		return 0;
	}
	if (!ftpd_start(21, user, password, on_event, NULL, why, sizeof why)) {
		status(why);
		at(LOG_TOP, 1);
		printf("Press a key to go back.");
		getchar();
		quit();
		return 0;
	}
	at(3, 1);
	printf("Connect to \033[1mftp://%s\033[0m", net_address());
	at(4, 1);
	printf("User       %s", user);
	at(5, 1);
	printf("Password   %s", password);
	status("Waiting for a connection");
	if (note) log_line(note);
	at(LOG_TOP + LOG_ROWS + 2, 1);
	printf("Esc: quit");

	for (;;) {
		ftpd_poll();
		if (keyboard_key_available()) {
			char k = keyboard_get_key();
			if (k == 0x1b) break;
		}
		sleep_ms(1);
	}
	quit();
	return 0;
}
