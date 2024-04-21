CC=gcc
CFLAGS=-Wall -D_FILE_OFFSET_BITS=64
LIBS=`pkg-config fuse3 --cflags --libs`

all: ssd_fuse ssd_fuse_dut

ssd_fuse: ssd_fuse_one.c
	$(CC) $(CFLAGS) $< $(LIBS) -o $@

ssd_fuse_dut: ssd_fuse_dut.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f ssd_fuse ssd_fuse_dut
