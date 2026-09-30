/* kmain.c -- boot: mount the filesystem on DSK and run /bin/init from it.
 * See KERNEL_NOTES.md and FS_NOTES.md. */
#include "proc.h"
#include "kio.h"
#include "fs.h"
#include "../examples/pmm.h"

int main(void) {
    int pid;
    pmm_init();
    kputs("kernel: boot, free_frames=");
    kputu((unsigned int)pmm_frames_free());
    kputs("\r\n");
    if (fs_init() < 0) {
        kputs("kernel: PANIC no filesystem on DSK unit 0\r\n");
        return 1;
    }
    kputs("kernel: fs mounted, free_blocks=");
    kputu(fs_freeblocks());
    kputs("\r\n");
    pid = proc_spawn("/bin/init", -1);
    if (pid < 0) {
        kputs("kernel: PANIC cannot run /bin/init\r\n");
        return 1;
    }
    kputs("kernel: /bin/init is pid ");
    kputu((unsigned int)pid);
    kputs("\r\n");
    k_start();
    return 0;
}
