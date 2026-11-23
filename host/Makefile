# host/ -- the FTP server on a PC, over a FAT disk image, for testing with
# real FTP clients:  make check PICO_SDK_PATH=/path/to/pico-sdk
# (needs mkfs.fat and mtools for the test card, and python3)
FF = $(PICO_SDK_PATH)/lib/tinyusb/lib/fatfs/source
FFCONF = ../modules/picocalc-text-starter/drivers/fatfs/ffconf.h

server: server.c ../src/ftpd.c net_posix.c diskio_img.c
	mkdir -p ff && cp $(FF)/ff.c $(FF)/ff.h $(FF)/ffunicode.c $(FF)/diskio.h $(FFCONF) ff/
	cp ../modules/picocalc-extras/net/net.h .
	$(CC) -O1 -Wall -Wno-unused-parameter -I. -Iff -I../src -o $@ server.c ../src/ftpd.c net_posix.c diskio_img.c ff/ff.c ff/ffunicode.c

card.img:
	rm -f $@ && mkfs.fat -C -n PICOCALC $@ 65536 >/dev/null
	printf 'hello card\n' > t.txt && mcopy -i $@ t.txt ::readme.txt && mmd -i $@ ::docs && rm t.txt

check: server
	rm -f card.img && $(MAKE) card.img && python3 test_ftp.py

clean:
	rm -rf server card.img ff net.h
