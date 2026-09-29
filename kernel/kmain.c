/* kmain.c -- boot the N-process kernel with a fixed set of user programs
 * embedded as images (kernel/build.sh generates the headers). See
 * KERNEL_NOTES.md. */
#include "proc.h"
#include "kio.h"
#include "../examples/pmm.h"
#include "img_worker.h"
#include "img_badptr.h"
#include "img_escape.h"
#include "img_badread.h"
#ifdef BIGMEM
#include "img_bigmem.h"
#include "img_winfault.h"
#endif

#define STACKWORDS 1024

static void spawn(const char *name, const unsigned int *w, unsigned int n,
                  unsigned int base, unsigned int stacktop) {
    int pid = proc_create(w, n, base, stacktop, STACKWORDS);
    kputs("kernel: spawned ");
    kputs(name);
    kputs(" pid=");
    if (pid < 0) { kputs("FAILED\r\n"); return; }
    kputu((unsigned int)pid);
    kputs(" free_frames=");
    kputu((unsigned int)pmm_frames_free());
    kputs("\r\n");
    proc_dump(pid);
}

int main(void) {
    int i;
    pmm_init();
    kputs("kernel: boot, free_frames=");
    kputu((unsigned int)pmm_frames_free());
    kputs("\r\n");
    for (i = 0; i < NWORKERS; i++)
        spawn("worker", img_worker_words, img_worker_nwords,
              img_worker_base, img_worker_stacktop);
#ifdef BIGMEM
    /* KERNEL_NOTES.md section 9: >32KW in one process, next to workers */
    spawn("bigmem", img_bigmem_words, img_bigmem_nwords,
          img_bigmem_base, img_bigmem_stacktop);
    spawn("winfault", img_winfault_words, img_winfault_nwords,
          img_winfault_base, img_winfault_stacktop);
#else
    spawn("badptr", img_badptr_words, img_badptr_nwords,
          img_badptr_base, img_badptr_stacktop);
    spawn("escape", img_escape_words, img_escape_nwords,
          img_escape_base, img_escape_stacktop);
    spawn("badread", img_badread_words, img_badread_nwords,
          img_badread_base, img_badread_stacktop);
#endif
    k_start();
    return 0;
}
