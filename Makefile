obj-m += niko_panic.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all: niko_image.h
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=1 modules

niko_image.h:
	@echo "Run ./gen_image.sh first to generate niko_image.h"
	@exit 1

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
