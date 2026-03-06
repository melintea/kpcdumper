/*   
 * On-demand core dumping
 */

#include "kpcdumper.h"

#include <asm/uaccess.h>
#include <asm/signal.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#define STR(x)   #x
#define TOSTR(x) STR(x)

#ifdef BUFLEN
#  error BUFLEN already defined
#endif
#define BUFLEN (4096) // PATH_MAX?


#ifdef SUCCESS
#  error SUCCESS already defined
#endif
#define SUCCESS (0) 


static const char  g_cmdsf[] = 
    GDB" " 
       "--cd="KPCDUMPER_HOME" "
       "--nx --batch --readnever "
       "-ex 'set logging enabled on' " 
       "-ex 'set startup-with-shell off' "
       "-ex 'set pagination off' -ex 'set height 0' -ex 'set width 0' "
       "-ex 'set use-coredump-filter off' "
       "-ex 'set dump-excluded-mappings on' "
       "-ex 'set debug infrun 1' "
       "-ex 'set debug lin-lwp 1' "
       "-ex 'attach %d' -ex 'gcore %s' " 
       "-ex detach -ex quit "
   "; /usr/bin/kill -CONT %d "
   "; /usr/bin/kill -"TOSTR(SIGDUMPDONE) " %d "
   ;
static char *g_envp[] = { 
    "HOME="KPCDUMPER_HOME, 
    "PWD="KPCDUMPER_HOME, 
    "TERM=linux", 
    "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 
    NULL 
};

static DEFINE_MUTEX(g_kpcdumper_lock);
static char g_cmds[BUFLEN+1]     = {0};
static char g_dumpfile[BUFLEN+1] = {0};

/* 
 * This is called whenever a process attempts to open the device file 
 */
static int 
kpcdumper_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO KPCDUMPER_DEVNAME ": device_open(%p)\n", file);

    mutex_lock(&g_kpcdumper_lock);
    
    try_module_get(THIS_MODULE);
    return SUCCESS;
}

static int 
kpcdumper_release(struct inode *inode, struct file *file)
{
    printk(KERN_INFO KPCDUMPER_DEVNAME ": device_release(%p,%p)\n", inode, file);

    mutex_unlock(&g_kpcdumper_lock);

    module_put(THIS_MODULE);
    return SUCCESS;
}

static long 
kpcdumper_ioctl(//struct inode *inode,    /* see include/linux/fs.h */
        struct file *file,    /* ditto */
        unsigned int ioctl_num,    /* number and param for ioctl */
        unsigned long ioctl_param)
{
    printk(KERN_INFO KPCDUMPER_DEVNAME ": kpcdumper_ioctl\n");
    
    pid_t procpid  = current->tgid;
    pid_t threadid = current->pid;
    
    switch(ioctl_num) {
    case IOCTL_SET_MSG:
        int  length = 0;
        char ch;
        char *pch   = NULL;
        /* 
         * Receive a pointer to a message (in user space) and set that
         * to be the device's message.  Get the parameter given to 
         * ioctl by the process. 
         */
        pch = (char *)ioctl_param;

        /* 
         * Find the length of the message 
         */
        get_user(ch, pch);
        for (length = 0; ch && length < BUFLEN; ++length, ++pch) {
            get_user(ch, pch);
        }
        printk(KERN_INFO KPCDUMPER_DEVNAME ": %d bytes\n", length);
        
        if (length + sizeof(g_cmdsf) + 2*10/*pid*/ > BUFLEN) {
            printk(KERN_ERR KPCDUMPER_DEVNAME ": %d bytes\n", length);
            return -E2BIG;
        }
        
        int ldf = copy_from_user(g_dumpfile, (char *)ioctl_param, length); 
        printk(KERN_INFO KPCDUMPER_DEVNAME ": device_write: '%s'/%d from %d\n", 
               g_dumpfile, ldf, procpid);

        // gcore                                             attach   core        SIGCONT  SIGDUMPDONE
        int tlen = snprintf(g_cmds, sizeof(g_cmds), g_cmdsf, procpid, g_dumpfile, procpid, threadid);
        printk(KERN_INFO KPCDUMPER_DEVNAME ": %d: %s\n", tlen, g_cmds);
        if (tlen >= BUFLEN) {
            printk(KERN_ERR KPCDUMPER_DEVNAME ": %d bytes\n", tlen);
            return -E2BIG;
        }
    
        char *argv[] = { 
        "/bin/sh", "-c", 
            g_cmds,
            NULL
        };
        for (int i=0; argv[i] != NULL; ++i) {
            printk(KERN_INFO KPCDUMPER_DEVNAME ":    %s\n", argv[i]);
        }
	
	// _cmds must deliver a SIGCONT
        send_sig(SIGSTOP, current, 0);
	
        // UMH_WAIT_PROC will deadlock, SIGSTOP/CONT or not
        int gret = call_usermodehelper(argv[0], argv, g_envp, UMH_WAIT_EXEC);
        printk(KERN_INFO KPCDUMPER_DEVNAME ": gcore: %d\n", gret);
        
        break;
    
    default:
        return -ENOTTY;
    
    } // switch
    
    return SUCCESS;
}

struct file_operations fops = 
{
    .owner          = THIS_MODULE,
    .open           = kpcdumper_open,
    .release        = kpcdumper_release,
    .unlocked_ioctl = kpcdumper_ioctl,
    .compat_ioctl   = kpcdumper_ioctl,
};

int init_module(void)  
{
    int ret = -1;
    
    ret = register_chrdev(KPCDUMPER_DEVNUM, KPCDUMPER_DEVNAME, &fops);
    if (ret < 0) {
        printk(KERN_ALERT KPCDUMPER_DEVNAME ": register_chrdev failed %d\n", ret);
        return ret;
    }
 
    pr_info(KPCDUMPER_DEVNAME ": On-demand core dumping: init.\n");  
    return SUCCESS;  
}  
  
void cleanup_module(void)  
{  
    pr_info(KPCDUMPER_DEVNAME ": On-demand core dumping: cleanup.\n");  
    unregister_chrdev(KPCDUMPER_DEVNUM, KPCDUMPER_DEVNAME);
}
 
 
MODULE_LICENSE("GPL");
