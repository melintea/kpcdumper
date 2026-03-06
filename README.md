# kpcdumper - a mechanism to dump core files from within an application without killing it

It consist of a kernel module which will run ```gdb gcore``` on demand from the application.

Usage:
- make
- insert the kernel module
- create the dump device
- link the app with the static library
- call ```dump_core()```



## Build

- change the makefile and the header as needed
- make

## Caveats

- The thread calling ```dump_core``` will wait until the dump completes.
  Other threads in the application will continue running until gdb attaches. 
- YMMV with a different kernel.
- Secure Boot, ```selinux``` and such might interfere with.

## Similar tools & various links

- https://code.google.com/archive/p/google-coredumper/

