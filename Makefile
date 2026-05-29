obj-m += niko_panic.o

KDIR ?= /lib/modules/$(shell uname -r)/build

# Match the kernel's compiler. Clang-built kernels (e.g. CachyOS) need LLVM=1;
# GCC-built kernels (most distros) must NOT set it, or the build fails. Detect
# from the target kernel's config instead of hardcoding.
LLVM_FLAG := $(shell cat $(KDIR)/.config $(KDIR)/include/config/auto.conf 2>/dev/null \
	| grep -q "CONFIG_CC_IS_CLANG=y" && echo LLVM=1)

all: niko_image.h
	$(MAKE) -C $(KDIR) M=$(PWD) $(LLVM_FLAG) modules

niko_image.h:
	@echo "Run ./gen_image.sh first to generate niko_image.h"
	@exit 1

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
