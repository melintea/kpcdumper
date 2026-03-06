#
#
#

# keep in sync with header
MODULE = kpcdumper
DEVNUM = 137

obj-m += $(MODULE).o
   
PWD := $(CURDIR)  
   
all: module libkpcdumper.a appdump

module: 
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules 
	modinfo $(MODULE).ko

libkpcdumper.a: libkpcdumper.o
	ar rcs $@ $^
	
libkpcdumper.o: libkpcdumper.c
	gcc -c -Wall -ggdb $<

#TODO: dep on libkpcdumper.a
appdump: appdump.cpp
	g++ -c -Wall -ggdb $<
	g++ appdump.o -L. -Wl,-Bstatic -lkpcdumper -Wl,-Bdynamic -Wall -O0 -o $@

test: all
	-sudo rmmod $(MODULE).ko
	sudo insmod $(MODULE).ko
	lsmod | grep $(MODULE)
	sudo mknod -m 666 $(MODULE) c $(DEVNUM) 1
	./appdump
	sudo rm $(MODULE) 
	test -f /tmp/kpc1.core && test -f /tmp/kpc2.core && test -f /tmp/kpc3.core && test -f /tmp/kpc4.core && test -f /tmp/kpc5.core
	sudo dmesg --time-format delta | grep $(MODULE) 

.PHONY: clean  
clean: 
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	-rm *.o a.out 
	-rm appdump
	-rm libkpcdumper.a 
	-sudo rmmod $(MODULE).ko
	-sudo rm $(MODULE)
	-sudo rm /tmp/gdb.log /tmp/*.core
