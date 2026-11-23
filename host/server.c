/* server.c -- ftpd.c on a PC: port 2121, user "chris", password "calc" */
#include <stdio.h>
#include "ff.h"
#include "pico/stdlib.h"
#include "ftpd.h"
static void rep(ftpd_event e, const char *t, uint32_t b, uint32_t tot, void *c)
{ (void)c; (void)tot; if (e != FTPD_SENDING && e != FTPD_RECEIVING) { printf("event %d %s %u\n", e, t, b); fflush(stdout); } }
int main(void)
{
	static FATFS fs;
	if (f_mount(&fs, "", 1) != FR_OK) { puts("no card.img"); return 1; }
	char why[96];
	if (!ftpd_start(2121, "chris", "calc", rep, NULL, why, sizeof why)) { puts(why); return 1; }
	puts("listening"); fflush(stdout);
	for (;;) { ftpd_poll(); sleep_ms(1); }
}
