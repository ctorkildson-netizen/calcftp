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
 * The screen shows where to connect and what's happening; Shift+F5 saves a
 * screenshot to /screenshots; Esc quits, leaving the network properly so the
 * next program joins at once.
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
#include "lcdshot.h"

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

/* rows: the title bar, where to connect, the login, a status pill, then the
   log (newest at the bottom, scrolling up), and "Esc: quit" */
#define ROW_TITLE 1
#define ROW_ADDR 3
#define ROW_USER 5
#define ROW_PASS 6
#define ROW_STATUS 8
#define LOG_TOP 10
#define LOG_ROWS 19
#define ROW_KEYS 31

#define PLAIN "\033[0m"
#define DIM "\033[90m"

static void at(int row, int col) { printf("\033[%d;%dH", row, col); }

static int width(void)
{
	int w = lcd_get_columns();
	return w > 1 && w <= 80 ? w - 1 : 39;	/* (the last column wraps) */
}

/* a whole-width bar in colour (the escape code), text on it */
static void bar(int row, const char *colour, const char *text)
{
	at(row, 1);
	printf("%s%-*.*s" PLAIN, colour, width(), width(), text);
}

typedef enum { ST_WAIT, ST_CONNECTED, ST_MOVING, ST_ERROR } st_kind;

/* the status: a coloured label, then what's happening */
static void status(st_kind k, const char *s)
{
	static const char *const label[] = { " WAITING ", " CONNECTED ", " BUSY ", " PROBLEM " };
	static const char *const colour[] = { "\033[30;103m", "\033[30;102m", "\033[30;106m", "\033[97;41m" };
	at(ROW_STATUS, 1);
	printf(PLAIN "\033[K%s%s" PLAIN " %.*s", colour[k], label[k], width() - (int)strlen(label[k]) - 1, s);
}

/* the log: each entry starts with a coloured "> " and a capital, and
   carries on indented, so entries stand apart when they wrap */
static char log_text[LOG_ROWS][81];
static const char *log_colour[LOG_ROWS];
static int log_n;

static void log_row(const char *colour, const char *lead, const char *text, int n)
{
	if (log_n == LOG_ROWS) {		/* scroll up one */
		memmove(log_text[0], log_text[1], sizeof log_text[0] * (LOG_ROWS - 1));
		memmove(&log_colour[0], &log_colour[1], sizeof log_colour[0] * (LOG_ROWS - 1));
		log_n--;
	}
	snprintf(log_text[log_n], sizeof log_text[0], "%s%.*s", lead, n, text);
	log_colour[log_n] = colour;
	log_n++;
}

static void log_entry(const char *colour, const char *s)
{
	int w = width() - 2;
	bool first = true;
	do {
		int n = (int)strlen(s) < w ? (int)strlen(s) : w;
		if (n == w) {				/* break at a space if there is one */
			int k = n;
			while (k > w / 2 && s[k] != ' ') k--;
			if (s[k] == ' ') n = k;
		}
		log_row(first ? colour : NULL, first ? "> " : "  ", s, n);
		s += n;
		while (*s == ' ') s++;
		first = false;
	} while (*s);
	for (int r = 0; r < LOG_ROWS; r++) {
		at(LOG_TOP + r, 1);
		printf(PLAIN "\033[K");
		if (r >= log_n) continue;
		if (log_colour[r]) printf("%s%.2s" PLAIN "%s", log_colour[r], log_text[r], log_text[r] + 2);
		else printf("%s", log_text[r]);
	}
}

#define GREEN "\033[92m"
#define YELLOW "\033[93m"
#define RED "\033[91m"
#define CYAN "\033[96m"
#define WHITE "\033[97m"

static bool dots(const char *what, void *ctx)
{
	(void)ctx;
	status(ST_WAIT, what);
	return true;
}

static char peer[16];

static void connected(void)
{
	char l[48];
	snprintf(l, sizeof l, "to %s", peer);
	status(ST_CONNECTED, l);
}

static void on_event(ftpd_event e, const char *text, uint32_t bytes, uint32_t total, void *ctx)
{
	(void)ctx;
	char l[160];
	static absolute_time_t next;
	switch (e) {
	case FTPD_CONNECTED:
		snprintf(peer, sizeof peer, "%s", text);
		snprintf(l, sizeof l, "Connection from %s", text);
		log_entry(WHITE, l);
		connected();
		break;
	case FTPD_LOGGED_IN: snprintf(l, sizeof l, "Logged in as %s", text); log_entry(GREEN, l); break;
	case FTPD_BAD_LOGIN:
		snprintf(l, sizeof l, "Wrong user name or password (\"%s\")", text);
		log_entry(RED, l);
		status(ST_ERROR, "a login was refused");
		break;
	case FTPD_SENDING:
	case FTPD_RECEIVING:
		if (!time_reached(next)) break;		/* (a few times a second is plenty) */
		next = make_timeout_time_ms(250);
		if (total) snprintf(l, sizeof l, "%s %s: %lu of %lu KB", e == FTPD_SENDING ? "sending" : "receiving",
				    text, (unsigned long)(bytes / 1024), (unsigned long)(total / 1024));
		else snprintf(l, sizeof l, "receiving %s: %lu KB", text, (unsigned long)(bytes / 1024));
		status(ST_MOVING, l);
		break;
	case FTPD_SENT:
		snprintf(l, sizeof l, "Sent %s (%lu bytes)", text, (unsigned long)bytes);
		log_entry(CYAN, l);
		connected();
		break;
	case FTPD_RECEIVED:
		snprintf(l, sizeof l, "Received %s (%lu bytes)", text, (unsigned long)bytes);
		log_entry(CYAN, l);
		connected();
		break;
	case FTPD_FAILED:
		snprintf(l, sizeof l, "Failed: %s", text);
		log_entry(RED, l);
		status(ST_ERROR, text);
		break;
	case FTPD_DISCONNECTED:
		log_entry(YELLOW, "Disconnected");
		status(ST_WAIT, "for a connection");
		break;
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
	printf("\033[2J");
	bar(ROW_TITLE, "\033[97;44m", " PicoCalc FTP server");
	const char *note = login();

	char why[96];
	bool ok = net_join(dots, NULL, why, sizeof why) &&
		  ftpd_start(21, user, password, on_event, NULL, why, sizeof why);
	if (!ok) {
		status(ST_ERROR, why);
		at(ROW_KEYS, 1);
		printf(DIM "Press a key to go back." PLAIN);
		getchar();
		quit();
		return 0;
	}
	char addr[48];
	snprintf(addr, sizeof addr, "  ftp://%s", net_address());
	bar(ROW_ADDR, "\033[1;97;45m", addr);
	at(ROW_USER, 1);
	printf(DIM "User     " PLAIN "%s", user);
	at(ROW_PASS, 1);
	printf(DIM "Password " PLAIN "%s", password);
	status(ST_WAIT, "for a connection");
	if (note) log_entry(YELLOW, note);
	at(ROW_KEYS, 1);
	printf(DIM "Esc: quit   Shift+F5: screenshot" PLAIN);

	for (;;) {
		ftpd_poll();
		if (keyboard_key_available()) {
			uint8_t k = (uint8_t)keyboard_get_key();
			if (k == KEY_ESC || k == 0x1b) break;	/* the PicoCalc's Esc (or a Bluetooth keyboard's) */
			if (k == KEY_F10) {			/* Shift+F5, as the keyboard sends it */
				char saved[80], l[100];
				bool ok = lcd_screenshot("ftp", saved, sizeof saved);
				snprintf(l, sizeof l, ok ? "Screenshot %s" : "Couldn't save a screenshot", saved);
				log_entry(ok ? WHITE : RED, l);
			}
		}
		sleep_ms(1);
	}
	quit();
	return 0;
}
