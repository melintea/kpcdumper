/*   
 * Core dumping tests
 */

#include "kpcdumper.h"

#include <chrono>
#include <thread>

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>


void func2(const char* corefile, const char* devname)
{
    dump_core(corefile, devname);
    ::printf("Dumped '%s'?\n", corefile);
}

void func1(const char* corefile, const char* devname)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    func2(corefile, devname);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

int main(int argc, char** argv) 
{
    ::printf("%s: PID %d\n", argv[0], getpid());


    std::thread t1(func1, "./kpc1.core",  "./" KPCDUMPER_DEVNAME);
    std::thread t2(func1, "./kpc2.core",  "./" KPCDUMPER_DEVNAME);
    std::thread t3(func1, "./kpc3.core",  "./" KPCDUMPER_DEVNAME);
    std::thread t4(func1, "./kpc4.core",  "./" KPCDUMPER_DEVNAME);
    std::thread t5(func1, "./kpc5.core",  "./" KPCDUMPER_DEVNAME);
    
    t1.join();
    t2.join();
    t3.join();
    t4.join();
    t5.join();

    ::printf("Done: %s: PID %d\n", argv[0], getpid());
    return 0;
}

