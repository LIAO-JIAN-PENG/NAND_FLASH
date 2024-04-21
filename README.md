# Nand Flash Memory Controller LAB
* Fuse ssd: ssd_fuse.c
* Common header: ssd_fuse_header.h
* Dut: ssd_fuse_dut.c

## Package
```bash=
apt-cache search fuse
sudo apt-get update
sudo apt-get install fuse3
sudo apt-get install libfuse3-dev
reboot
```

## Create device
```bash=
mkdir /tmp/ssd
```

## Compile and Run
```bash=
make
./ssd_fuse –d /tmp/ssd
```

## Read/Write API
```bash=
./ssd_fuse_dut 

# Usage: ssd_fuse SSD_FILE COMMAND

# COMMANDS
#  l    : get logic size 
#  p    : get physical size 
#  r SIZE [OFF] : read SIZE bytes @ OFF (dfl 0) and output to stdout
#  w SIZE [OFF] : write SIZE bytes @ OFF (dfl 0) from random
#  W    : write amplification factor
```

## Testing script
```bash=
sh test.sh test1
sh test.sh test2
```