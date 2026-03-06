/*   
 * Core dumping tests
 */

#include "kpcdumper.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>


std::mutex               g_mtxStart;
std::condition_variable  g_cvStart;
bool                     g_startFlag = false;


void func2(const char* corefile, const char* devname)
{
    {
        std::unique_lock<std::mutex> lock(g_mtxStart);
        g_cvStart.wait(lock, []{return g_startFlag;});
    }
    
    dump_core(corefile, devname);
    //::printf("Dumped '%s'?\n", corefile);
}

void func1(const char* corefile, const char* devname)
{
    //std::this_thread::sleep_for(std::chrono::milliseconds(200));
    func2(corefile, devname);
    //std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

int main(int argc, char** argv) 
{
    ::printf("%s: PID %d\n", argv[0], getpid());

    std::vector<std::thread> threads;
    threads.emplace_back(func1, "./kpc1.core",  "./" KPCDUMPER_DEVNAME);
    threads.emplace_back(func1, "./kpc2.core",  "./" KPCDUMPER_DEVNAME);
    threads.emplace_back(func1, "./kpc3.core",  "./" KPCDUMPER_DEVNAME);
    threads.emplace_back(func1, "./kpc4.core",  "./" KPCDUMPER_DEVNAME);
    threads.emplace_back(func1, "./kpc5.core",  "./" KPCDUMPER_DEVNAME);

    {
        std::unique_lock<std::mutex> lock(g_mtxStart);
	g_startFlag = true;
        g_cvStart.notify_all();
    }
    
    for (auto& t: threads) {
        t.join();
    }

    ::printf("Done: %s: PID %d\n", argv[0], getpid());
    return 0;
}

