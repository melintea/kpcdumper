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
static atomic_bool g_dumpdone = ATOMIC_VAR_INIT(false);
static mtx_t       g_dumping;

void dumpdone_handler(int sig)
{
    //printf("Signal %d\n", sig);
    bool old = atomic_load(&g_dumpdone);
    assert(old == false);
    atomic_store(&g_dumpdone, true);
}

void dump_core(const char* corefile, const char* devname)
{

    int mret = mtx_lock(&g_dumping);
    assert(mret == thrd_success);

    
    while (true == atomic_load(&g_dumpdone)) {
        //printf("Waiting in %s...\n", corefile);
        usleep(1000*10L);
    } 
    //printf("Dumping %s\n", corefile);
   
    struct sigaction satrap = { 
        .sa_handler = dumpdone_handler,
        .sa_flags   = SA_RESETHAND
    };
    sigaction(SIGDUMPDONE, &satrap, NULL);
    
    int fd = open(devname, O_RDWR);
    if (fd < 0) {
        //printf("open failed\n");
        abort(); // We still get the core...
    }
    
    ioctl(fd, IOCTL_SET_MSG, corefile);
    
    close(fd);
    

    while (false == atomic_load(&g_dumpdone)) {
        //printf("Waiting out %s...\n", corefile);
        usleep(1000*10L);
    } 
    atomic_store(&g_dumpdone, false);

    mret = mtx_unlock(&g_dumping);
    assert(mret == thrd_success);

}

