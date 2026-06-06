/*   
 * On-demand core dumping
 */

#include "kpcdumper.h"

#include <asm/signal.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
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
#include <linux/uaccess.h>

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


static const char  g_cmdsfmt[] = 
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

static dev_t g_majnum;
//static int g_majnum = 0; 
static struct class*  g_class  = NULL;
static struct device* g_device = NULL;
static struct cdev    g_cdev;

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
    
    switch(ioctl_num) {
    case IOCTL_SET_MSG:
    // return or g_cmds must deliver a SIGCONT
        send_sig(SIGSTOP, current, 0);
    
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
        
        if (length + sizeof(g_cmdsfmt) + 2*10/*pid*/ > BUFLEN) {
            printk(KERN_ERR KPCDUMPER_DEVNAME ": %d bytes\n", length);
            send_sig(SIGCONT, current, 0);
            return -E2BIG;
        }
        
        int ldf = copy_from_user(g_dumpfile, (char *)ioctl_param, length); 
        printk(KERN_INFO KPCDUMPER_DEVNAME ": device_write: '%s'/%d from %d\n", 
               g_dumpfile, ldf, procpid);

        // gcore 
        int tlen = snprintf(g_cmds, sizeof(g_cmds), 
                            g_cmdsfmt, 
                            procpid,     // attach
                            g_dumpfile,  // core file
                            procpid,     // SIGCONT
                            procpid);    // SIGDUMPDONE
        printk(KERN_INFO KPCDUMPER_DEVNAME ": %d: %s\n", tlen, g_cmds);
        if (tlen >= BUFLEN) {
            printk(KERN_ERR KPCDUMPER_DEVNAME ": %d bytes\n", tlen);
            send_sig(SIGCONT, current, 0);
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

// Set access permissions
static char* kpcdumper_devnode(const struct device *dev, umode_t *mode)
{
    if (mode != NULL) {
        *mode = 0666;
    }
    return kasprintf(GFP_KERNEL, "%s", dev_name(dev));
}

static int __init kpcdumper_init(void)
{
    int ret = -1;
    
    // Dynamically allocate a major number
    ret = alloc_chrdev_region(&g_majnum, 0, 1, KPCDUMPER_DEVNAME);
    if (ret < 0) {
        printk(KERN_ALERT KPCDUMPER_DEVNAME ": Failed to register a major number %d\n", ret);
        return ret;
    }
    pr_info(KPCDUMPER_DEVNAME ": number %d : %d\n", MAJOR(g_majnum), MINOR(g_majnum));

    // Register the device class
    g_class = class_create(/*THIS_MODULE,*/ KPCDUMPER_DEVNAME/*CLASS_NAME*/);
    if (IS_ERR(g_class)) {
        unregister_chrdev_region(g_majnum, 1);
        printk(KERN_ALERT KPCDUMPER_DEVNAME ": Failed to register device class\n");
        return PTR_ERR(g_class);
    }
    g_class->devnode = kpcdumper_devnode;
    pr_info(KPCDUMPER_DEVNAME ": device class registered correctly\n");

    // Register the device driver
    g_device = device_create(g_class, NULL, g_majnum, NULL, KPCDUMPER_DEVNAME);
    if (IS_ERR(g_device)) {
        class_destroy(g_class);
        unregister_chrdev_region(g_majnum, 1);
        printk(KERN_ALERT KPCDUMPER_DEVNAME ": Failed to create the device\n");
        return PTR_ERR(g_device);
    }
    pr_info(KPCDUMPER_DEVNAME ": device class created correctly\n");

    // Initialize the cdev structure and add it to the kernel
    cdev_init(&g_cdev, &fops);
    g_cdev.owner = THIS_MODULE;
    ret = cdev_add(&g_cdev, g_majnum, 1);
    if (ret < 0) {
        device_destroy(g_class, g_majnum);
        class_destroy(g_class);
        unregister_chrdev_region(g_majnum, 1);
        printk(KERN_ALERT KPCDUMPER_DEVNAME ": Failed to add cdev %d\n", ret);
        return ret;
    }
 
    pr_info(KPCDUMPER_DEVNAME ": On-demand core dumping: init.\n");  
    return SUCCESS;  
}  
  
static void __exit kpcdumper_exit(void)
{  
    pr_info(KPCDUMPER_DEVNAME ": On-demand core dumping: cleanup.\n");  
    
    cdev_del(&g_cdev);
    device_destroy(g_class, g_majnum);
    class_destroy(g_class);
    unregister_chrdev_region(g_majnum, 1);
}
 
 
module_init(kpcdumper_init);
module_exit(kpcdumper_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aurelian Melinte <ame01_at_gmx_dot_net>");
MODULE_DESCRIPTION("On-demand process core dumping device");
MODULE_VERSION("0.1");
