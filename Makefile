obj-m += pubsub.o

PWD := $(CURDIR)
BUILDDIR := $(PWD)/build

all:
	mkdir -p $(BUILDDIR)
	cp -f $(PWD)/*.c $(PWD)/*.h $(BUILDDIR)/ 2>/dev/null || true
	cp -f $(PWD)/Makefile $(BUILDDIR)/Makefile.orig 2>/dev/null || true
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(BUILDDIR) src=$(PWD) modules

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(BUILDDIR) src=$(PWD) clean
	rm -rf $(BUILDDIR)