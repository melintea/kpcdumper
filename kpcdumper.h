/*   
 * On-demand core dumping
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif


#define KPCDUMPER_DEVNAME "kpcdumper"      // keep in sync with the Makefile
#define KPCDUMPER_DEVNUM  (137)            // ditto   //TODO: dynamic
#define SIGDUMPDONE       SIGUSR1          // signal.h
#define KPCDUMPER_HOME    "/tmp"
#define GDB               "/usr/bin/gdb"   // "/opt/gdb163/bin/gdb"

#define IOCTL_SET_MSG   _IOW(KPCDUMPER_DEVNUM, 1, char*) // Userspace writes to kernel
#define IOCTL_GET_MSG   _IOR(KPCDUMPER_DEVNUM, 2, char*) // Userspace reads from kernel


/*
 * Example: dump_core( "foo.core", "/dev/"KPCDUMPER_DEVNAME );
 * If corefile is a relative path, it is relative to KPCDUMPER_HOME.
 */
void dump_core(const char* corefile, const char* devname);


#ifdef __cplusplus
} // extern "C"
#endif

