/*   
 * On-demand core dumping
 */

#include "kpcdumper.h"

#include <asm/signal.h>
#include <linux/binfmts.h>
#include <linux/cdev.h>
#include <linux/coredump.h>
#include <linux/device.h>
#include <linux/elf.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/kprobes.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aurelian Melinte <ame01_at_gmx_dot_net>");
MODULE_DESCRIPTION("On-demand process core dumping device");
MODULE_VERSION("0.1");

#define KPCDUMPER_DEVCLASS  KPCDUMPER_DEVNAME


#define STR(x)   #x
#define TOSTR(x) STR(x)

#ifdef BUFLEN
#  error BUFLEN already defined
#endif
#define BUFLEN (PATH_MAX)


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
   ;
static char *g_envp[] = { 
    "HOME="KPCDUMPER_HOME, 
    "PWD="KPCDUMPER_HOME, 
    "TERM=linux", 
    "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 
    NULL 
};

static dev_t g_majnum;
static struct class*  g_class  = NULL;
static struct device* g_device = NULL;
static struct cdev    g_cdev;

static DEFINE_MUTEX(g_kpcdumper_lock); // open/close
static char g_cmds[BUFLEN+1]     = {0};
static char g_dumpfile[BUFLEN+1] = {0};

static int gcore(pid_t procpid, const char* dumpfile)
{
    int tlen = snprintf(g_cmds, sizeof(g_cmds), 
                        g_cmdsfmt, 
                        procpid,     // attach
                        g_dumpfile,  // core file
                        procpid);    // SIGCONT
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
    
    // UMH_WAIT_PROC will deadlock, SIGSTOP/CONT or not
    int gret = call_usermodehelper(argv[0], argv, g_envp, UMH_WAIT_EXEC);
    printk(KERN_INFO KPCDUMPER_DEVNAME ": gcore: %d\n", gret);
    return gret;
}

// Cannot use __attribute__((cleanup(filp_close_guard))) : will keep mod refs
static inline void filp_close_guard(void* pp)
{
    __auto_type ppf = (struct file**)pp;
    if (ppf && *ppf) {
        __auto_type p = *ppf;
        filp_close(p, NULL);
    }
}

#if ! defined(USE_GCORE)
typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
static struct linux_binfmt *get_elf_binfmt(void)
{
    struct kprobe kp = {
        .symbol_name = "kallsyms_lookup_name",
    };
    if (register_kprobe(&kp) < 0) {
        pr_err("Failed to register kprobe for symbol lookup\n");
        return NULL;
    }

    kallsyms_lookup_name_t lookupname = (kallsyms_lookup_name_t)kp.addr;
    unregister_kprobe(&kp);

    if (!lookupname) {
        pr_err("No address for kallsyms_lookup_name\n");
        return NULL;
    }

    struct linux_binfmt* elffmt = (struct linux_binfmt *)lookupname("elf_format");
    return elffmt;
}

// Placeholder dump_size, fixed up after dropping the mmap lock, for a VMA that
// may be the start of an ELF mapping. Mirrors fs/coredump.c.
#define DUMP_SIZE_MAYBE_ELFHDR_PLACEHOLDER 1

// Decide how much of a VMA's contents should be included in the core dump.
// @see fs/coredump.c:vma_dump_size() honouring the MMF_DUMP_* filter
// bits snapshotted in mm_flags. The ELF-header case returns a placeholder that
// snapshot_vmas() resolves once the mmap lock has been dropped.
static unsigned long vma_dump_size(struct vm_area_struct* vma,
                                   unsigned long mm_flags)
{
#define FILTER(type) (mm_flags & (1UL << MMF_DUMP_##type))
    if (vma->vm_flags & VM_DONTDUMP) {
        return 0;
    }
    if (is_vm_hugetlb_page(vma)) {
        if ((vma->vm_flags & VM_SHARED) && FILTER(HUGETLB_SHARED)) {
            goto whole;
        }
        if (!(vma->vm_flags & VM_SHARED) && FILTER(HUGETLB_PRIVATE)) {
            goto whole;
        }
        return 0;
    }
    // Do not dump I/O mapped devices or special mappings
    if (vma->vm_flags & VM_IO) {
        return 0;
    }
    // By default, dump shared memory if mapped from an anonymous file.
    if (vma->vm_flags & VM_SHARED) {
        if (file_inode(vma->vm_file)->i_nlink == 0
                ? FILTER(ANON_SHARED) : FILTER(MAPPED_SHARED)) {
            goto whole;
        }
        return 0;
    }
    // Dump segments that have been written to.
    if ((!IS_ENABLED(CONFIG_MMU) || vma->anon_vma) && FILTER(ANON_PRIVATE)) {
        goto whole;
    }
    if (vma->vm_file == NULL) {
        return 0;
    }
    if (FILTER(MAPPED_PRIVATE)) {
        goto whole;
    }
    // If this is the beginning of an executable file mapping, dump the first
    // page to aid in determining what was mapped here.
    if (FILTER(ELF_HEADERS) && vma->vm_pgoff == 0
            && (vma->vm_flags & VM_READ)) {
        if ((READ_ONCE(file_inode(vma->vm_file)->i_mode) & 0111) != 0) {
            return PAGE_SIZE;
        }
        // ELF libraries aren't always executable; the mmap lock is held here so
        // copy_from_user() can't run. Defer the magic check to snapshot_vmas().
        return DUMP_SIZE_MAYBE_ELFHDR_PLACEHOLDER;
    }
#undef FILTER
    return 0;
whole:
    return vma->vm_end - vma->vm_start;
}

// @see fs/coredump.c:free_vma_snapshot(). 
static void free_vma_snapshot(struct coredump_params* cprm)
{
    if (cprm->vma_meta) {
        for (int i = 0; i < cprm->vma_count; ++i) {
            struct file* file = cprm->vma_meta[i].file;
            if (file) {
                fput(file);
            }
        }
        kvfree(cprm->vma_meta);
        cprm->vma_meta = NULL;
    }
}

// Under the mmap lock, take a snapshot of the task's VMAs into cprm->vma_meta /
// vma_count / vma_data_size. elf_core_dump() reads these, so the dump oopses if
// they are left NULL. @see fs/coredump.c:dump_vma_snapshot(). 
// free_vma_snapshot()
static int snapshot_vmas(struct coredump_params* cprm)
{
    struct mm_struct* mm = current->mm;
    struct vm_area_struct* vma = NULL;
    VMA_ITERATOR(vmi, mm, 0);
    int i = 0;

    if (!mm) {
        return -EINVAL;
    }

    // Write mode matches the kernel: the stack-expansion code can still change
    // VMA bounds under a read-mode mmap lock.
    if (mmap_write_lock_killable(mm)) {
        return -EINTR;
    }

    cprm->vma_data_size = 0;
    cprm->vma_count = mm->map_count;
    cprm->vma_meta = kvmalloc_array(cprm->vma_count, sizeof(*cprm->vma_meta),
                                    GFP_KERNEL);
    if (!cprm->vma_meta) {
        mmap_write_unlock(mm);
        return -ENOMEM;
    }

    for_each_vma(vmi, vma) {
        struct core_vma_metadata* m = cprm->vma_meta + i;
        if (i >= cprm->vma_count) {
            break; // map_count is the authoritative bound
        }
        m->start     = vma->vm_start;
        m->end       = vma->vm_end;
        m->flags     = vma->vm_flags;
        m->dump_size = vma_dump_size(vma, cprm->mm_flags);
        m->pgoff     = vma->vm_pgoff;
        m->file      = vma->vm_file;
        if (m->file) {
            get_file(m->file);
        }
        ++i;
    }
    cprm->vma_count = i; // actual VMAs captured

    mmap_write_unlock(mm);

    // mmap lock is dropped: resolve the deferred ELF-header placeholders and
    // accumulate the total dumpable size.
    for (i = 0; i < cprm->vma_count; ++i) {
        struct core_vma_metadata* m = cprm->vma_meta + i;
        if (m->dump_size == DUMP_SIZE_MAYBE_ELFHDR_PLACEHOLDER) {
            char elfmag[SELFMAG];
            if (copy_from_user(elfmag, (void __user *)m->start, SELFMAG)
                    || memcmp(elfmag, ELFMAG, SELFMAG) != 0) {
                m->dump_size = 0;
            } else {
                m->dump_size = PAGE_SIZE;
            }
        }
        cprm->vma_data_size += m->dump_size;
    }

    return 0;
}

static int kcore(const char* dumpfile)
{
    // Notes: 
    // - vfs_coredump: v6.17+; uses on /proc/sys/kernel/core_pattern
    // - do_coredump before: same core_pattern dep
    // - alternative: filp_open(struct file *) && elf_core_dump()

    struct linux_binfmt* elffmt = get_elf_binfmt();
    if (!elffmt || !elffmt->core_dump) {
        pr_err("No elf_format\n");
        return -ENOENT;
    }
    
    struct file* file = 
        filp_open(dumpfile, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (IS_ERR(file)) {
        pr_err("Failed to open %s: %ld\n", dumpfile, PTR_ERR(file));
        return PTR_ERR(file);
    }
    file->f_mode |= FMODE_CAN_WRITE;
    
    struct coredump_params cprm;
    memset(&cprm, 0, sizeof(cprm));
    cprm.siginfo = NULL;          // Or fill out a custom struct kernel_siginfo
    //  Architecture specific CPU registers. task_pt_regs(current) contains the 
    //  register state of the ioctl syscall itself, not the state of the app; 
    //  must parse and look up its thread references using find_vpid() so its 
    //  stack frame does not change while dumping.
    //cprm.regs = task_pt_regs(current); 
    cprm.file = file;             // Directing output to our open VFS descriptor
    cprm.limit = RLIM_INFINITY;   // Overriding standard user shell limits
    cprm.mm_flags = current->mm->flags;

    int sret = snapshot_vmas(&cprm);
    if (sret != 0) {
        pr_err("Failed to snapshot VMAs for %s: %d\n", dumpfile, sret);
        filp_close(file, NULL);
        return sret;
    }

    pr_info("Dumping %s (%d vmas, %zu bytes)\n",
            dumpfile, cprm.vma_count, cprm.vma_data_size);
    file_start_write(cprm.file);
    int dret = elffmt->core_dump(&cprm);
    file_end_write(cprm.file);
    pr_info("Dump %s ret %d\n", dumpfile, dret);

    free_vma_snapshot(&cprm);
    filp_close(file, NULL);
    return dret;

    //return gcore(current->tgid, dumpfile);
}
#endif // USE_GCORE

// g_kpcdumper_lock/open/close protected
static long 
kpcdumper_ioctl(
        //struct inode  *inode,     /* see include/linux/fs.h */
        struct file   *file,      /* ditto */
        unsigned int  ioctl_num,  /* number and param for ioctl */
        unsigned long ioctl_param)
{
    printk(KERN_INFO KPCDUMPER_DEVNAME ": kpcdumper_ioctl\n");
    
    pid_t procpid  = current->tgid;
    
    switch(ioctl_num) {
    case IOCTL_SET_MSG:
        // return or gcore must deliver a SIGCONT
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
        WARN_ON(g_dumpfile[0] != '/'); // absolute path

#if defined(CONFIG_COREDUMP) && ! defined(USE_GCORE)
        int gret = kcore(g_dumpfile);
        printk(KERN_INFO KPCDUMPER_DEVNAME ": kcore: %d\n", gret);
#else
        int gret = gcore(procpid, g_dumpfile);
        printk(KERN_INFO KPCDUMPER_DEVNAME ": gcore: %d\n", gret);
#endif // CONFIG_COREDUMP
        if (gret != 0) {
            send_sig(SIGCONT, current, 0);
	    return gret; 
	}
        
        break;
    
    default:
        printk(KERN_WARNING KPCDUMPER_DEVNAME ": kpcdumper_ioctl: unsupported: %d\n", ioctl_num);
        return -ENOTTY;
    
    } // switch
    
    return SUCCESS;
}

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
    g_class = class_create(/*THIS_MODULE,*/ KPCDUMPER_DEVCLASS);
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
