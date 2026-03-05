/*   
 * Tests
 */

#include "kpcdumper.h"

#include <chrono>
#include <thread>

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>


int main(int argc, char** argv) 
{
    ::printf("%s: PID %d\n", argv[0], getpid());

    char spid[20] = {0};
    snprintf(spid, sizeof(spid), "attach %s", argv[1]);
    char *xargv[] = { 
        "/usr/bin/gdb", 
        "--nx", "--batch", "--readnever",
        "-ex", "set logging enabled on", 
        "-ex", "set pagination off", "-ex", "set height 0", "-ex", "set width 0",
        "-ex", "set startup-with-shell off",
        "-ex", "set use-coredump-filter off",
        "-ex", "set dump-excluded-mappings on",
        "-ex", spid, "-ex", "gcore ./_gdb.core", 
        "-ex", "detach", 
        "-ex", "quit",
        NULL
    };
    char *xenvp[] = { 
        "HOME=/tmp", 
        "PWD=/tmp", 
        "TERM=linux", 
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 
        NULL 
    };
    
    for (int i=0; xargv[i] != NULL; ++i) {
        ::printf(":    %s\n", xargv[i]);
    }

    if (::execve(xargv[0], xargv, xenvp) == -1) {
        ::perror("execve");
        return 1;
    }

    return 0;
}

