/*   
 * Static lib
 */

#include "kpcdumper.h"

#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <threads.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>


// TODO: sig_atomic_t?
static atomic_bool _dumpdone = ATOMIC_VAR_INIT(false);
static mtx_t       _dumping;

void dumpdone_handler(int sig)
{
    //printf("Signal %d\n", sig);
    bool old = atomic_load(&_dumpdone);
    assert(old == false);
    atomic_store(&_dumpdone, true);
}

void dump_core(const char* corefile, const char* devname)
{

    int mret = mtx_lock(&_dumping);
    assert(mret == thrd_success);

    struct sigaction satrap = { 
        .sa_handler = dumpdone_handler,
        //.sa_flags   = SA_RESETHAND
    };
    sigaction(SIGDUMPDONE, &satrap, NULL);
    
    
    while (true == atomic_load(&_dumpdone)) {
        //printf("Waiting in %s...\n", corefile);
        usleep(1000*10L);
    } 
    //printf("Dumping %s\n", corefile);
    
    
    int fd = open(devname, O_RDWR);
    if (fd < 0) {
        //printf("open failed\n");
        abort(); // We still get the core...
    }
    
    ioctl(fd, IOCTL_SET_MSG, corefile);
    
    close(fd);
    

    while (false == atomic_load(&_dumpdone)) {
        //printf("Waiting out %s...\n", corefile);
        usleep(1000*10L);
    } 
    atomic_store(&_dumpdone, false);

    mret = mtx_unlock(&_dumping);
    assert(mret == thrd_success);

}

