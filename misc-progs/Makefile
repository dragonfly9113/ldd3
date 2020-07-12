
#FILES = nbtest load50 mapcmp polltest mapper setlevel setconsole inp outp \
	datasize dataalign netifdebug

FILES = nbtest

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
INCLUDEDIR = $(KERNELDIR)/include
#CFLAGS = -O2 -fomit-frame-pointer -Wall -I$(INCLUDEDIR)

CFLAGS = -O2 -fomit-frame-pointer -Wall

all: $(FILES)

clean:
	rm -f $(FILES) *~ core

