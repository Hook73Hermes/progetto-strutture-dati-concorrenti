obj-m += pubsub.o
ccflags-$(RCU) += -DPUBSUB_USE_RCU

PWD := $(CURDIR)
BUILDDIR := $(PWD)/build

.PHONY: all kernel user clean

all: kernel user

# Modulo kernel: "make kernel" (rwlock) oppure "make kernel RCU=y" (RCU)
kernel:
	mkdir -p $(BUILDDIR)
	cp -f $(PWD)/*.c $(PWD)/*.h $(BUILDDIR)/ 2>/dev/null || true
	cp -f $(PWD)/Makefile $(BUILDDIR)/Makefile.orig 2>/dev/null || true
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(BUILDDIR) src=$(PWD) modules

# Programma userspace di benchmark
user:
	mkdir -p $(BUILDDIR)
	gcc -Wall -O2 -pthread -o $(BUILDDIR)/pubsub_benchmark $(PWD)/pubsub_benchmark.c

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(BUILDDIR) src=$(PWD) clean
	rm -rf $(BUILDDIR)