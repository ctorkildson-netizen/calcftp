/* ftpd.h -- an FTP server for the PicoCalc's SD card, on picocalc-extras'
 * net/ (Wi-Fi from /ntp/ntp.cfg) and FatFs.
 *
 * One client at a time (a second is told to come back later), with a user
 * name and password; passive mode (PASV, EPSV) and active (PORT); the usual
 * commands of FileZilla, WinSCP, Windows' ftp and Linux's ftp and lftp:
 * listing, get, put, delete, rename, make and remove folders.
 *
 * The program calls ftpd_poll() often; a transfer runs inside one call, with
 * the progress callback (if any) called as it goes.
 */
#ifndef FTPD_H
#define FTPD_H

#include <stdbool.h>
#include <stdint.h>

/* what the server tells the program, for its screen */
typedef enum {
	FTPD_CONNECTED,		/* text: the client's address */
	FTPD_LOGGED_IN,		/* text: the user name */
	FTPD_BAD_LOGIN,		/* text: the user name tried */
	FTPD_SENDING,		/* text: the file; bytes: done so far; total: its size */
	FTPD_RECEIVING,		/* text: the file; bytes: done so far */
	FTPD_SENT,		/* text: the file's full path; bytes: its size */
	FTPD_RECEIVED,		/* text: the file's full path; bytes: its size */
	FTPD_FAILED,		/* text: what went wrong */
	FTPD_DISCONNECTED,
} ftpd_event;

typedef void (*ftpd_report)(ftpd_event e, const char *text, uint32_t bytes, uint32_t total, void *ctx);

/* listen on port (21 usually); user and password are what the client must
   give (an empty user: anyone, any password). false, with why, if not. */
bool ftpd_start(uint16_t port, const char *user, const char *password,
		ftpd_report report, void *ctx, char *why, unsigned why_size);
void ftpd_poll(void);
bool ftpd_busy(void);		/* a client is connected */
void ftpd_stop(void);

#endif
