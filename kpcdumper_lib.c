/*   
 * Static lib
 */

#include "kpcdumper.h"

#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>


static atomic_int _dumpdone = ATOMIC_VAR_INIT(0);

void dumpdone_handler(int sig)
{
    //printf("Signal %d\n", sig);
    int old = atomic_fetch_add(&_dumpdone, 1);
    assert(old == 0);
}

void dump_core(const char* corefile, const char* devname)
{
    int fd = open(devname, O_RDWR);
    if (fd < 0) {
        printf("open failed\n");
        abort(); // We still get the core...
    }
    
    struct sigaction satrap = { 
        .sa_handler = dumpdone_handler,
	//.sa_flags   = SA_RESETHAND
    };
    sigaction(SIGDUMPDONE, &satrap, NULL);
    
    int dump = atomic_load(&_dumpdone);
    assert(dump == 0);
    dump += 1;
    
    ioctl(fd, IOCTL_SET_MSG, corefile);
    
    while ( ! atomic_compare_exchange_weak(&_dumpdone, &dump, 0)) {
        printf("Waiting...\n");
	
	struct timespec ts = {
	    .tv_sec  = 0,
	    .tv_nsec = 1000*100L
	};
	nanosleep(&ts, NULL);
    } 
    assert(dump == 0);

    // Let another dump proceed
    close(fd);
}

