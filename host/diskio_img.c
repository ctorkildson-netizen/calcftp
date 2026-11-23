/* diskio_img.c -- FatFs's disk calls over a disk image file (card.img), for
 * trying ftpd.c on a PC with the same FatFs the PicoCalc uses */
#include <stdio.h>
#include <time.h>
#include "ff.h"
#include "diskio.h"
static FILE *img;
DSTATUS disk_initialize(BYTE d) { (void)d; if (!img) img = fopen("card.img", "r+b"); return img ? 0 : STA_NOINIT; }
DSTATUS disk_status(BYTE d) { (void)d; return img ? 0 : STA_NOINIT; }
DRESULT disk_read(BYTE d, BYTE *b, LBA_t s, UINT n) { (void)d; fseek(img, (long)s * 512, SEEK_SET); return fread(b, 512, n, img) == n ? RES_OK : RES_ERROR; }
DRESULT disk_write(BYTE d, const BYTE *b, LBA_t s, UINT n) { (void)d; fseek(img, (long)s * 512, SEEK_SET); return fwrite(b, 512, n, img) == n ? RES_OK : RES_ERROR; }
DRESULT disk_ioctl(BYTE d, BYTE c, void *b) { (void)d; if (c == CTRL_SYNC) { fflush(img); return RES_OK; } if (c == GET_SECTOR_SIZE) { *(WORD *)b = 512; return RES_OK; } return RES_PARERR; }
DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | (11u << 21) | (23u << 16) | (12u << 11); }
