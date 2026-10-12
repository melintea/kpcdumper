/*   
 * Static lib
 */

#include "kpcdumper.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
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
static mtx_t   g_dumping;
static char    g_dumpfile[PATH_MAX]  = {0};

// -std=gnu11 for nested functions
static inline void open_guard(int* pfd)
{
    if (pfd && *pfd >= 0) { 
        close(*pfd); 
    };
}

static inline void mtx_unlock_guard(void* pp)
{
    if ((mtx_t**)pp && *(mtx_t**)pp) {
        __auto_type p = *((mtx_t**)pp);
        int mret = mtx_unlock(p);
        assert(mret == thrd_success);
    }
}

void dump_core(const char* corefile)
{

    int mret = mtx_lock(&g_dumping);
    assert(mret == thrd_success);
    mtx_t* mtxGuard __attribute__((cleanup(mtx_unlock_guard))) = &g_dumping;
    
    assert(corefile);
    const char* filep = corefile;
    if (corefile[0] != '/') {
        snprintf(g_dumpfile, sizeof(g_dumpfile), "%s/%s", KPCDUMPER_HOME, corefile);
        filep = g_dumpfile;
    }

    //printf("Dumping %s\n", corefile);

    {
        static const char* kpcddev = "/dev/"KPCDUMPER_DEVNAME;
        int fd __attribute__((cleanup(open_guard))) = open(kpcddev, O_RDWR);
        if (fd < 0) {
            //printf("%s open failed %s\n", kpcddev, strerror(errno));
            
            //mret = mtx_unlock(&g_dumping);
            //assert(mret == thrd_success);
           
            abort(); // We still get the core...
        }
            
        ioctl(fd, IOCTL_SET_MSG, filep);
    } // open_guard(fd)

    // mtx_guard(g_dumping)
}

