#include "proc.h"
#include "kmem.h"
#include "kio.h"
#include "ksyscall.h"
#include "../examples/pmm.h"
#include "../examples/console.h"
#include "fs.h"
#include "fsys.h"
#include "ksync.h"
#include "tty.h"
#include "tape.h"
#include "bdev.h"

/* ---- context exchange with kernel/kentry.s ------------------------------
 * kentry.s's entry paths write the interrupted user context here, and
 * k_resume enters user mode from here. Referenced only from asm, so
 * eclipse-cc's undefined-symbol retry loop is what keeps them alive. */
unsigned int k_ac0, k_ac1, k_ac2, k_ac3;
unsigned int k_pc;          /* bit 15 = carry */
unsigned int k_mapstat;     /* DIA readback at entry; DOA value at exit */
unsigned int k_devcode;
unsigned int k_reload;      /* nonzero: LMP k_ptbuf into map A first */
unsigned int k_ptbuf[NPAGES];

#define K_TICK 1
#define K_SYS  2
#define K_FLT  3
#define K_DEV  4

/* ---- process table (parallel arrays -- see proc.h) --------------------- */
int p_state[NPROC];
unsigned int p_ac0[NPROC], p_ac1[NPROC], p_ac2[NPROC], p_ac3[NPROC];
unsigned int p_pc[NPROC];
unsigned int p_pt[NPROC * NPAGES];
unsigned int p_nframes[NPROC];
int p_parent[NPROC];            /* -1: none (kernel-spawned) */
unsigned int p_status[NPROC];   /* exit status, valid in P_ZOMBIE */
int p_waitpid[NPROC];           /* child a P_WAIT process waits for */

/* Big-memory segments (KERNEL_NOTES.md section 9): extra physical frames a
 * process owns beyond its 32KW logical space, reachable only by mapping
 * them into a "window" of its logical pages with SYS_MAPWIN.
 * p_fixed marks the pages proc_create mapped (image + stack): those can
 * never be remapped, so a window can't pull the caller's own code or
 * stack out from under it. */
unsigned int p_fixed[NPROC * NPAGES];
unsigned int p_seg[NPROC * MAXSEG];
unsigned int p_nseg[NPROC];

int curproc = -1;           /* -1: the idle loop (or boot) */
int loaded = -1;            /* whose page table is in map A */

/* ---- kernel stacks and in-kernel sleep (IO_NOTES.md) --------------------
 * Every process has its own kernel stack (kstk slice n; slice NPROC is the
 * idle loop's). k_kbase/k_klim are the stack base/limit WORD addresses of
 * whatever context is current; kentry.s loads them into 040/042 on every
 * trap entry and before every return to user mode. A process asleep in
 * the kernel keeps its frames on its own slice, plus its saved hardware
 * stack registers and AC2/AC3/resume PC in p_k*. */
static unsigned int kstk[(NPROC + 1) * KSTK];
unsigned int k_kbase, k_klim;
unsigned int k_ks_sp, k_ks_fp, k_ks_lim, k_ks_ac2, k_ks_ac3, k_ks_pc;
volatile unsigned int k_ks_resumed;
unsigned int k_n32 = 32;
static unsigned int p_chan[NPROC], p_kin[NPROC];
static unsigned int p_killed[NPROC];    /* SYS_KILL / ^C (KERNEL_NOTES.md) */
#define PNAMEW 6                        /* name words (12 bytes) for ps */
static unsigned int p_name[NPROC * PNAMEW];
#define KILLSTAT 0177767                /* -9: exit status of a killed process */
static unsigned int p_ksp[NPROC], p_kfp[NPROC], p_klim[NPROC];
static unsigned int p_kac2[NPROC], p_kac3[NPROC], p_kpc[NPROC];
static unsigned int fs_locked;
unsigned int st_sleeps, st_idle;

#define KSTK_FILL 0152525

static unsigned int kstk_base(int slice) {
    return (unsigned int)(unsigned long)&kstk[(unsigned int)slice * KSTK] >> 1;
}

/* statistics, printed at shutdown */
unsigned int st_ticks, st_switches, st_syscalls, st_faults;
#ifdef FSLOCK_STATS
/* Measurement only (FS_NOTES.md section 8): how often a process waits for
 * the fs lock, and whether the disk was busy at that moment. */
extern unsigned int bdev_busy(void);
unsigned int st_lk_acq, st_lk_wait, st_lk_wait_dskbusy;
#endif


static void k_halt(void) {
    asm volatile("HALT\n\t" ::: "memory");
}

static int pte_valid(unsigned int pte) {
    return (pte & PTE_INVALID) != PTE_INVALID;
}

static void proc_free(int n);

/* Allocate a process slot and an address space for an image of nwords at
 * base with its stack at stacktop: fixed pages mapped and zeroed, the rest
 * invalid. Returns the slot (state still P_FREE) or -1. On failure every
 * frame taken is returned. */
static int proc_alloc(unsigned int nwords, unsigned int base,
                      unsigned int stacktop, unsigned int stackwords) {
    int n, s;
    unsigned int i, pfn, lastimg, sfirst, slast, pt0;

    n = -1;
    for (s = 0; s < NPROC; s++) {
        if (p_state[s] == P_FREE) { n = s; break; }
    }
    if (n < 0) return -1;
    p_state[n] = P_EMBRYO;      /* reserved while it's built: spawn sleeps */

    pt0 = (unsigned int)n * NPAGES;
    lastimg = (base + nwords - 1) >> 10;
    /* stacktop-1 too: main's second argument (argv) lives there
     * (SHELL_NOTES.md), and reorder_asm.py puts _STACKTOP 2 words past
     * the image (STACK_GAP), so that word is never program data */
    sfirst = (stacktop - 1) >> 10;
    slast = (stacktop + stackwords - 1) >> 10;
    if (slast > NPAGES - 1) slast = NPAGES - 1;

    p_nframes[n] = 0;
    p_killed[n] = 0;
    p_nseg[n] = 0;
    fsys_procinit(n);       /* before anything that could call proc_free */
#ifdef KDEBUG
    kputs("  dbg lastimg="); kputu(lastimg);
    kputs(" sfirst="); kputu(sfirst);
    kputs(" slast="); kputu(slast);
    kputs(" stacktop="); kputo(stacktop);
    kputs("\r\n");
#endif
    for (s = 0; s < NPAGES; s++) {
        tty_poll();             /* 32 PTEs + zero-fill: section 11 */
        p_pt[pt0 + s] = PTE_INVALID | ((unsigned int)s << 10);
        p_fixed[pt0 + s] = 0;
        if ((unsigned int)s <= lastimg ||
            ((unsigned int)s >= sfirst && (unsigned int)s <= slast)) {
            pfn = (unsigned int)pmm_alloc_page();
            if (pfn == (unsigned int)PMM_NONE) {
                while (++s < NPAGES) p_fixed[pt0 + s] = 0;
                proc_free(n);
                p_state[n] = P_FREE;
                return -1;
            }
            p_pt[pt0 + s] = ((unsigned int)s << 10) | pfn;
            p_fixed[pt0 + s] = 1;
            p_nframes[n]++;
            for (i = 0; i < 1024; i++) kfar_write(pfn, i, 0);
        }
    }
    p_ac0[n] = 0; p_ac1[n] = 0; p_ac2[n] = 0; p_ac3[n] = 0;
    p_pc[n] = base;         /* _start; carry clear */
    p_parent[n] = -1;
    p_status[n] = 0;
    return n;
}

int proc_create(const unsigned int *words, unsigned int nwords,
                unsigned int base, unsigned int stacktop,
                unsigned int stackwords) {
    int n;
    unsigned int i, a, pfn, pt0;
    n = proc_alloc(nwords, base, stacktop, stackwords);
    if (n < 0) return -1;
    pt0 = (unsigned int)n * NPAGES;
    for (i = 0; i < nwords; i++) {
        a = base + i;
        pfn = p_pt[pt0 + (a >> 10)] & 01777;
        kfar_write(pfn, a & 01777, words[i]);
    }
    p_state[n] = P_RUN;
    return n;
}

/* Executable file format (FS_NOTES.md; kernel/fstools/mkfs.py writes it):
 *   word 0 AOUT_MAGIC, 1 nwords, 2 base, 3 stacktop, then nwords words;
 *   AOUT_MAGIC2 adds word 4, the stack words the program was built for
 *   (eclipse-cc ECLIPSE_STACK_WORDS, recorded as _STACKWDS). */
#define AOUT_MAGIC 0407
#define AOUT_MAGIC2 0410
static unsigned int lbuf[256];

/* Arguments for the next proc_spawn: kargc strings packed NUL-terminated in
 * kargs, string i at byte kargoff[i]; kargn bytes in all. */
#define MAXARGS 16
#define ARGMAX  256
static char kargs[ARGMAX];
static unsigned int kargoff[MAXARGS], kargc, kargn;

static void args_one(const char *path) {
    unsigned int i;
    kargc = 1;
    kargoff[0] = 0;
    for (i = 0; path[i] && i < ARGMAX - 1; i++) {
        if ((i & 15) == 0) tty_poll();
        kargs[i] = path[i];
    }
    kargs[i] = 0;
    kargn = i + 1;
}

/* copy the caller's NULL-terminated argv (byte address of a char *[]) */
static int args_user(unsigned int uargv) {
    unsigned int p, len;
    kargc = 0;
    kargn = 0;
    if (uargv & 1) return -1;
    for (;;) {
        if (!uvalid(uargv + 2 * kargc, 2)) return -1;
        p = kcopyin_word((uargv >> 1) + kargc);
        if (p == 0) return 0;
        if (kargc == MAXARGS) return -1;
        if (ucopystr(p, kargs + kargn, ARGMAX - kargn) < 0) return -1;
        kargoff[kargc] = kargn;
        for (len = 0; kargs[kargn + len]; len++) ;
        kargn += len + 1;
        kargc++;
    }
}

static void wlog(unsigned int pt0, unsigned int a, unsigned int v) {
    kfar_write(p_pt[pt0 + (a >> 10)] & 01777, a & 01777, v);
}

/* Lay out argv at the top of the child's stack area and hand main() its
 * arguments: eclipse-cc's _start does 040 = _STACKTOP; EJSR main, so
 * main's frame pointer is _STACKTOP+5 and it reads its first argument at
 * FP-5 = _STACKTOP, its second at FP-6 = _STACKTOP-1 (the backend's
 * convention, checked on a compiled 2-argument function). */
static void args_place(unsigned int pt0, unsigned int stacktop,
                       unsigned int stackwords) {
    unsigned int slast, top, strw, sw, av, i, v;
    slast = (stacktop + stackwords - 1) >> 10;
    if (slast > NPAGES - 1) slast = NPAGES - 1;
    top = ((slast + 1) << 10) - 1;              /* last mapped word */
    strw = (kargn + 1) >> 1;
    sw = top + 1 - strw;                        /* strings */
    av = sw - (kargc + 1);                      /* char *argv[kargc+1] */
    for (i = 0; i < strw; i++) {
        v = ((unsigned int)kargs[2 * i] & 0377) << 8;
        if (2 * i + 1 < kargn) v |= (unsigned int)kargs[2 * i + 1] & 0377;
        wlog(pt0, sw + i, v);
    }
    for (i = 0; i < kargc; i++) wlog(pt0, av + i, 2 * sw + kargoff[i]);
    wlog(pt0, av + kargc, 0);
    wlog(pt0, stacktop, kargc);                 /* argc */
    wlog(pt0, stacktop - 1, 2 * av);            /* argv (byte address) */
}

int proc_spawn(const char *path, int parent) {
    unsigned int inum, hdr[5], off, hsz, sw, i, k, a, pfn, pt0;
    int n, got;
    inum = fs_namei(path);
    if (inum == 0) return -1;
    if (fs_readi(inum, 0, (char *)hdr, 10) != 10) return -1;
    if (hdr[0] == AOUT_MAGIC) { hsz = 8; sw = STACKWORDS; }
    else if (hdr[0] == AOUT_MAGIC2) { hsz = 10; sw = hdr[4]; }
    else return -1;
    if (hdr[1] == 0 || hdr[2] + hdr[1] < hdr[2] || hdr[2] + hdr[1] > 0100000)
        return -1;
    if (sw < STACKWORDS) sw = STACKWORDS;   /* the old minimum still holds */
    n = proc_alloc(hdr[1], hdr[2], hdr[3], sw);
    if (n < 0) return -1;
    pt0 = (unsigned int)n * NPAGES;
    /* the image's words, big-endian bytes on disk = exactly the byte
     * order of a word array viewed through a char pointer */
    for (i = 0; i < hdr[1]; i += 256) {
        off = hsz + 2 * i;
        got = fs_readi(inum, off, (char *)lbuf, 512);
        if (got < 2) break;
        for (k = 0; k < (unsigned int)got / 2 && i + k < hdr[1]; k++) {
            a = hdr[2] + i + k;
            pfn = p_pt[pt0 + (a >> 10)] & 01777;
            kfar_write(pfn, a & 01777, lbuf[k]);
        }
    }
    args_place(pt0, hdr[3], sw);
    {                                   /* last path component, for ps */
        const char *b = path;
        unsigned int j;
        for (j = 0; path[j]; j++)
            if (path[j] == '/') b = path + j + 1;
        for (j = 0; j < 2 * PNAMEW; j++) {
            unsigned int c = *b ? (unsigned int)*b++ & 0377 : 0;
            if (j & 1) p_name[n * PNAMEW + j / 2] |= c;
            else p_name[n * PNAMEW + j / 2] = c << 8;
        }
    }
    if (parent >= 0) fsys_inherit(n, parent);
    p_parent[n] = parent;
    p_state[n] = P_RUN;
    return n;
}

void proc_dump(int n) {
    int s;
    unsigned int pte;
    kputs("  map:");
    for (s = 0; s < NPAGES; s++) {
        pte = p_pt[(unsigned int)n * NPAGES + s];
        if (pte_valid(pte)) {
            kputs(" ");
            kputu((unsigned int)s);
            kputs("->");
            kputo(pte & 01777);
        }
    }
    kputs("\r\n");
}

static void proc_free(int n) {
    int s;
    unsigned int pte, pt0, sg0;
    pt0 = (unsigned int)n * NPAGES;
    sg0 = (unsigned int)n * MAXSEG;
    /* fixed pages only: a window page points at a segment frame, which is
     * freed exactly once below via p_seg */
    for (s = 0; s < NPAGES; s++) {
        if ((s & 3) == 0) tty_poll();
        pte = p_pt[pt0 + s];
        if (p_fixed[pt0 + s] && pte_valid(pte))
            pmm_free_page((int)(pte & 01777));
    }
    for (s = 0; (unsigned int)s < p_nseg[n]; s++) {
        if ((s & 3) == 0) tty_poll();
        pmm_free_page((int)p_seg[sg0 + s]);
    }
    p_nseg[n] = 0;
    fsys_procexit(n);
    if (loaded == n) loaded = -1;
}

static void sched(void);

/* Process n is gone with this status: release everything, hand the status
 * to a waiting parent or keep it as a zombie, orphan its children. */
static void proc_exit(int n, unsigned int status) {
    int i, par;
    if (tty_raw && tty_rawpid == n) tty_setraw(0, -1);   /* back to cooked */
    proc_free(n);
    for (i = 0; i < NPROC; i++) {
        if (p_state[i] != P_FREE && p_parent[i] == n) {
            p_parent[i] = -1;
            if (p_state[i] == P_ZOMBIE) p_state[i] = P_FREE;
        }
    }
    par = p_parent[n];
    if (par >= 0 && p_state[par] == P_WAIT && p_waitpid[par] == n) {
        p_ac0[par] = status;            /* wait()'s return value */
        p_state[par] = P_RUN;
        p_state[n] = P_FREE;
    } else if (par >= 0 && p_state[par] != P_FREE && p_state[par] != P_ZOMBIE) {
        /* any live parent -- including one asleep inside the kernel (e.g.
         * in spawn, on the disk): found when a child exited during its
         * parent's next spawn and its slot was reused (IO_NOTES.md) */
        p_status[n] = status;
        p_state[n] = P_ZOMBIE;
    } else {
        p_state[n] = P_FREE;
    }
}

/* The current process's page table changed: make k_resume reload map A. */
static void reload_cur(void) {
    int s;
    for (s = 0; s < NPAGES; s++)
        k_ptbuf[s] = p_pt[(unsigned int)curproc * NPAGES + s];
    k_reload = 1;
    loaded = curproc;
}

/* SYS_SEGALLOC: grow the caller's segment by npages zeroed frames.
 * All-or-nothing: on failure every frame taken by this call is returned. */
static unsigned int sys_segalloc(unsigned int npages) {
    unsigned int i, j, old, sg0;
    int pfn;
    sg0 = (unsigned int)curproc * MAXSEG;
    old = p_nseg[curproc];
    if (npages == 0 || npages > MAXSEG || old + npages > MAXSEG)
        return 0177777;
    for (i = 0; i < npages; i++) {
        pfn = pmm_alloc_page();
        if (pfn == PMM_NONE) {
            for (j = 0; j < i; j++) pmm_free_page((int)p_seg[sg0 + old + j]);
            return 0177777;
        }
        p_seg[sg0 + old + i] = (unsigned int)pfn;
        for (j = 0; j < 1024; j++) kfar_write((unsigned int)pfn, j, 0);
    }
    p_nseg[curproc] = old + npages;
    kputs("kernel: pid ");
    kputu((unsigned int)curproc);
    kputs(" segalloc +");
    kputu(npages);
    kputs(" -> ");
    kputu(p_nseg[curproc]);
    kputs(" pages, frames ");
    kputo(p_seg[sg0 + old]);
    kputs("..");
    kputo(p_seg[sg0 + old + npages - 1]);
    kputs(" free_frames=");
    kputu((unsigned int)pmm_frames_free());
    kputs("\r\n");
    return p_nseg[curproc];
}

/* SYS_MAPWIN: arg1 = window first logical page (bits 0-4) | npages << 5,
 * arg2 = first segment page, or 0177777 to unmap the window. Every page
 * of the window must be a non-fixed page; the segment range must exist.
 * Validates everything before changing anything. */
static unsigned int sys_mapwin(unsigned int a1, unsigned int segpage) {
    unsigned int win, n, i, pt0, sg0, unmap;
    win = a1 & 037;
    n = a1 >> 5;
    pt0 = (unsigned int)curproc * NPAGES;
    sg0 = (unsigned int)curproc * MAXSEG;
    unmap = (segpage == 0177777);
    if (n == 0 || win + n > NPAGES) return 0177777;
    if (!unmap && (segpage >= p_nseg[curproc] ||
                   segpage + n > p_nseg[curproc])) return 0177777;
    for (i = 0; i < n; i++)
        if (p_fixed[pt0 + win + i]) return 0177777;
    for (i = 0; i < n; i++) {
        if (unmap)
            p_pt[pt0 + win + i] = PTE_INVALID | ((win + i) << 10);
        else
            p_pt[pt0 + win + i] = ((win + i) << 10) | p_seg[sg0 + segpage + i];
    }
    reload_cur();
    return 0;
}

static void save_cur(void) {
    p_ac0[curproc] = k_ac0;
    p_ac1[curproc] = k_ac1;
    p_ac2[curproc] = k_ac2;
    p_ac3[curproc] = k_ac3;
    p_pc[curproc] = k_pc;
}

/* LMP k_ptbuf into user map A now (not deferred to k_resume): a process
 * woken inside the kernel copies to/from its user memory through map A
 * before it ever gets back to k_resume. */
/* Word address of the page-table row LMP loads (set by load()). */
unsigned int k_ptaddr;

static void mapA_load(void) {
    asm volatile(
        "ESTA 2,k_ks_ac2,0\n\t"          /* scratch save of AC2/AC3 */
        "ESTA 3,k_ks_ac3,0\n\t"
        "SUB 0,0\n\t"
        "DOA 0,MAP\n\t"                  /* select A, user mode off */
        "ELDA 1,k_n32,0\n\t"
        "ELDA 2,k_ptaddr,0\n\t"        /* the process's own p_pt row */
        "SUB 0,0\n\t"
        "LMP\n\t"
        "ELDA 2,k_ks_ac2,0\n\t"
        "ELDA 3,k_ks_ac3,0\n\t"
        ::: "memory"
    );
}

static void load(int n) {
    int s;
    if (n != loaded) {
        /* LMP straight from the process's row: copying it into k_ptbuf
         * first cost ~2000 instructions per switch with interrupts off
         * (KERNEL_NOTES.md section 11). Pointers are byte addresses
         * (DEBUGGING_NOTES.md #38); LMP wants the word address. */
        k_ptaddr = (unsigned int)(unsigned long)&p_pt[(unsigned int)n * NPAGES] >> 1;
        mapA_load();
        k_reload = 0;
        loaded = n;
    }
    k_kbase = kstk_base(n);
    k_klim = k_kbase + KSTK - 16;
    if (n != curproc) st_switches++;
    curproc = n;
    k_ac0 = p_ac0[n];
    k_ac1 = p_ac1[n];
    k_ac2 = p_ac2[n];
    k_ac3 = p_ac3[n];
    k_pc = p_pc[n];
    k_mapstat = P_MAPSTAT;
}

static void shutdown(void) {
    kputs("kernel: no runnable processes. ticks=");
    kputu(st_ticks);
    kputs(" switches=");
    kputu(st_switches);
    kputs(" syscalls=");
    kputu(st_syscalls);
    kputs(" faults=");
    kputu(st_faults);
    kputs(" free_frames=");
    kputu((unsigned int)pmm_frames_free());
    kputs("\r\nkernel: io: sleeps=");
    kputu(st_sleeps);
    kputs(" idle=");
    kputu(st_idle);
    kputs(" tty_intr=");
    kputu(tty_nintr);
    kputs(" tty_drop=");
    kputu(tty_ndrop);
#ifdef FSLOCK_STATS
    kputs(" fslock acq/wait/wait-while-disk-busy=");
    kputu(st_lk_acq); kputs("/"); kputu(st_lk_wait); kputs("/"); kputu(st_lk_wait_dskbusy);
#endif
    kputs(" tty_polls=");
    kputu(tty_ncalls);
    kputs("/");
    kputu(tty_npoll);
    kputs(" dsk_intr=");
    kputu(bdev_nintr);
    kputs(" dsk_sleeps=");
    kputu(bdev_nsleep);
    kputs("\r\nkernel: kernel stack high water (words of ");
    kputu(KSTK);
    kputs("):");
    {
        int sl;
        unsigned int w;
        for (sl = 0; sl <= NPROC; sl++) {
            for (w = KSTK; w > 0; w--, (w & 31) || (tty_poll(), 0))
                if (kstk[(unsigned int)sl * KSTK + w - 1] != KSTK_FILL) break;
            kputs(" ");
            kputu(w);
        }
    }
    kputs("\r\n");
    k_halt();
}

/* Switch to process n and never return here: either back into the kernel
 * context it saved in ksleep(), or out to user mode via k_resume. */
static void swtch_to(int n) {
    load(n);
    if (p_kin[n]) {
        p_kin[n] = 0;
        k_ks_sp = p_ksp[n];
        k_ks_fp = p_kfp[n];
        k_ks_lim = p_klim[n];
        k_ks_ac2 = p_kac2[n];
        k_ks_ac3 = p_kac3[n];
        k_ks_pc = p_kpc[n];
        k_ks_resumed = 1;
        asm volatile("EJMP k_kswitch,0\n\t" ::: "memory");
    }
    asm volatile("EJMP k_resume,0\n\t" ::: "memory");
}

/* Nothing runnable, something asleep: idle with ION on, on its own stack. */
static void idle(void) {
    st_idle++;
    curproc = -1;
    k_kbase = kstk_base(NPROC);
    k_klim = k_kbase + KSTK - 16;
    asm volatile("EJMP k_idle,0\n\t" ::: "memory");
}

/* Round-robin: the next P_RUN slot after curproc (possibly curproc).
 * Never returns: every caller has already saved whatever it needs. */
static void proc_kill(int n, int insched);
static void intr_fg(void);

static void sched(void) {
    if (tty_sigint) {
        tty_sigint = 0;
        intr_fg();
    }
    /* A chain of in-kernel sleeps/wakeups switches kernel-to-kernel
     * without ever re-enabling interrupts (only k_resume and the idle loop
     * do), so take a waiting keystroke here too (IO_NOTES.md overrun fix;
     * a trace showed 6767 instructions of sched/load/wakeup between a
     * keystroke and its interrupt). */
    tty_poll();
    int i, n, sleepers;
    n = curproc < 0 ? NPROC - 1 : curproc;
    for (i = 0; i < NPROC; i++) {
        n = n + 1;
        if (n >= NPROC) n = 0;
        if (p_state[n] == P_RUN) swtch_to(n);
    }
    sleepers = 0;
    for (i = 0; i < NPROC; i++)
        if (p_state[i] == P_SLEEP) sleepers++;
    if (sleepers) idle();
    shutdown();
}

/* ---- sleep / wakeup (ksync.h) ------------------------------------------ */

int kcansleep(void) {
    return curproc >= 0;
}

int kkilled(void) {
    return curproc >= 0 && p_killed[curproc];
}

/* Kill process n. One stopped at the user boundary (preempted, or blocked
 * in wait) exits now. One inside the kernel -- asleep, woken but not yet
 * back out, or the caller itself in a syscall -- is only marked: it exits
 * when its syscall returns (do_syscall), and a tty or pipe wait is woken so
 * that happens soon. insched: called from sched(), where curproc's state
 * is already saved, so curproc may exit directly too. -1: no such process,
 * or the login process tree's root (parent -1). */
static void proc_kill(int n, int insched) {
    int s = p_state[n];
    if (s == P_FREE || s == P_ZOMBIE || s == P_EMBRYO) return;
    if ((s == P_RUN && !p_kin[n] && (n != curproc || insched)) ||
        s == P_WAIT) {
        proc_exit(n, KILLSTAT);
        return;
    }
    p_killed[n] = 1;
    if (s == P_SLEEP && (p_chan[n] == CHAN_TTY || p_chan[n] >= CHAN_PIPE))
        p_state[n] = P_RUN;
}

static unsigned int sys_kill(unsigned int pid) {
    if (pid >= NPROC || p_parent[pid] < 0) return 0177777;
    if (p_state[pid] == P_FREE || p_state[pid] == P_ZOMBIE ||
        p_state[pid] == P_EMBRYO) return 0177777;
    proc_kill((int)pid, 0);
    return 0;
}

/* ^C: kill every process its parent is wait()ing for -- the foreground
 * job -- except a child of a parentless process (init's login shell).
 * Targets are chosen first: killing a parent would orphan its child. */
static void intr_fg(void) {
    int i, par, hit[NPROC];
    for (i = 0; i < NPROC; i++) {
        par = p_parent[i];
        hit[i] = p_state[i] != P_FREE && p_state[i] != P_ZOMBIE &&
                 par >= 0 && p_parent[par] >= 0 &&
                 p_state[par] == P_WAIT && p_waitpid[par] == i;
    }
    for (i = 0; i < NPROC; i++)
        if (hit[i]) proc_kill(i, 1);
}

/* SYS_PSINFO: w[0] state, w[1] parent, w[2] pages, w[3..8] name */
static unsigned int sys_psinfo(unsigned int pid, unsigned int ub) {
    unsigned int j;
    if (pid >= NPROC || !uvalid(ub, 18)) return 0177777;
    kcopyout_word(ub >> 1, (unsigned int)p_state[pid]);
    kcopyout_word((ub >> 1) + 1, (unsigned int)p_parent[pid]);
    kcopyout_word((ub >> 1) + 2, p_nframes[pid] + p_nseg[pid]);
    for (j = 0; j < PNAMEW; j++)
        kcopyout_word((ub >> 1) + 3 + j, p_name[pid * PNAMEW + j]);
    return 0;
}

void wakeup(unsigned int chan) {
    int i;
    for (i = 0; i < NPROC; i++)
        if (p_state[i] == P_SLEEP && p_chan[i] == chan) p_state[i] = P_RUN;
}

__attribute__((noinline)) void ksleep(unsigned int chan) {
    p_chan[curproc] = chan;
    p_state[curproc] = P_SLEEP;
    st_sleeps++;
    k_ks_resumed = 0;
    asm volatile(
        "ESTA 2,k_ks_ac2,0\n\t"
        "ESTA 3,k_ks_ac3,0\n\t"
        "LDA 0,040\n\t"
        "ESTA 0,k_ks_sp,0\n\t"
        "LDA 0,041\n\t"
        "ESTA 0,k_ks_fp,0\n\t"
        "LDA 0,042\n\t"
        "ESTA 0,k_ks_lim,0\n\t"
        "ELEF 0,ksleep_back,0\n\t"
        "ESTA 0,k_ks_pc,0\n\t"
        "ksleep_back:\n\t"
        ::: "memory"
    );
    /* Second arrival (via k_kswitch): 040/041/042 and AC2/AC3 are back as
     * they were above, so this frame is intact; just return. */
    if (k_ks_resumed) return;
    p_ksp[curproc] = k_ks_sp;
    p_kfp[curproc] = k_ks_fp;
    p_klim[curproc] = k_ks_lim;
    p_kac2[curproc] = k_ks_ac2;
    p_kac3[curproc] = k_ks_ac3;
    p_kpc[curproc] = k_ks_pc;
    p_kin[curproc] = 1;
    save_cur();                 /* its user context, for when it returns */
    sched();
}

/* The filesystem is one sleep-lock: a process may sleep on the disk in the
 * middle of an fs operation, and nothing else may enter fs.c/the buffer
 * cache until it is done (IO_NOTES.md). */
static void fs_lock(void) {
#ifdef FSLOCK_STATS
    st_lk_acq++;
    if (fs_locked) {
        st_lk_wait++;
        if (bdev_busy()) st_lk_wait_dskbusy++;
    }
#endif
    while (fs_locked) ksleep(CHAN_FSLOCK);
    fs_locked = 1;
}

static void fs_unlock(void) {
    fs_locked = 0;
    wakeup(CHAN_FSLOCK);
}

static void kill_cur(const char *why) {
    kputs("\r\nkernel: pid ");
    kputu((unsigned int)curproc);
    kputs(" ");
    kputs(why);
    kputs(" pc=");
    kputo(k_pc & 077777);
    kputs(" mapstat=");
    kputo(k_mapstat);
    kputs("\r\n");
    proc_exit(curproc, 0177776);        /* killed: status -2 */
    sched();
}

/* buf is a BYTE address (DEBUGGING_NOTES.md #38: pointer values are byte
 * addresses and chars are packed two per word, high byte first), so byte
 * b lives in word b>>1, in the high half when b is even. */
static int sys_write_user(unsigned int buf, unsigned int len) {
    unsigned int i, b, w, pt0;
    if (len == 0) return 0;
    if (buf + (len - 1) < buf) return -1;       /* wraps past 0177777 */
    pt0 = (unsigned int)curproc * NPAGES;
    for (i = 0; i < len; i++) {
        if ((i & 63) == 0) tty_poll();
        w = (buf + i) >> 1;
        if (!pte_valid(p_pt[pt0 + (w >> 10)])) return -1;
    }
    for (i = 0; i < len; i++) {
        b = buf + i;
        w = kcopyin_word(b >> 1);
        console_putchar((int)((b & 1) ? (w & 0377) : ((w >> 8) & 0377)));
        tty_poll();     /* polled TTO is slow: per character, as sys_writefd */
    }
    return (int)len;
}

/* static, not a do_syscall local: keeps its frame small (SHELL_NOTES.md,
 * "large frames") */
static char path[FS_MAXPATH];

static void do_syscall(void) {
    unsigned int r;
    st_syscalls++;
    fs_cwd = fsys_cwd(curproc);     /* relative paths (SHELL_NOTES.md) */
    r = 0;
    if (k_ac0 == SYS_PUTCHAR) {
        console_putchar((int)k_ac1);
    } else if (k_ac0 == SYS_GETCHAR) {
        r = tty_getc();
    } else if (k_ac0 == SYS_GETPID) {
        r = (unsigned int)curproc;
    } else if (k_ac0 == SYS_SEGALLOC) {
        r = sys_segalloc(k_ac1);
    } else if (k_ac0 == SYS_MAPWIN) {
        r = sys_mapwin(k_ac1, k_ac2);
    } else if (k_ac0 == SYS_WRITE) {
        r = (unsigned int)sys_write_user(k_ac1, k_ac2);
    } else if (k_ac0 == SYS_EXIT) {
#ifdef KTRACE_EXIT
        kputs("\r\nkernel: pid ");
        kputu((unsigned int)curproc);
        kputs(" exited, status ");
        kputu(k_ac1);
        kputs("\r\n");
#endif
        proc_exit(curproc, k_ac1);
        sched();
        return;
    } else if (k_ac0 == SYS_WAIT) {
        int c = (int)k_ac1;
        if (c < 0 || c >= NPROC || p_parent[c] != curproc ||
            p_state[c] == P_FREE) {
            r = 0177777;
        } else if (p_state[c] == P_ZOMBIE) {
            r = p_status[c];
            p_state[c] = P_FREE;
        } else {
            save_cur();                 /* block until c exits */
            p_waitpid[curproc] = c;
            p_state[curproc] = P_WAIT;
            sched();
            return;
        }
    } else if (k_ac0 == SYS_TTYRAW) {
        r = tty_setraw(k_ac1, curproc);
    } else if (k_ac0 == SYS_KILL) {
        r = sys_kill(k_ac1);
    } else if (k_ac0 == SYS_PSINFO) {
        r = sys_psinfo(k_ac1, k_ac2);
    } else if (k_ac0 == SYS_TAPE) {
        r = sys_tape(k_ac1, k_ac2, k_ac3);
    } else if (k_ac0 == SYS_SPAWN || k_ac0 == SYS_SPAWNV) {
        /* Under the fs lock: loading the image can sleep on the disk, and
         * `path` and the argv staging buffer are shared statics that the
         * lock also protects. Arguments copied to locals first -- k_ac*
         * belong to whoever runs next once this process sleeps. */
        unsigned int op = k_ac0, a1 = k_ac1, a2 = k_ac2;
        int c = -1;
        fs_lock();
        if (ucopystr(a1, path, FS_MAXPATH) == 0) {
            if (op == SYS_SPAWN || a2 == 0) args_one(path);
            if (op == SYS_SPAWN || a2 == 0 || args_user(a2) == 0)
                c = proc_spawn(path, curproc);
        }
        fs_unlock();
        r = (unsigned int)c;
    } else if ((k_ac0 >= SYS_OPEN && k_ac0 <= SYS_UNLINK) ||
               (k_ac0 >= SYS_CHDIR && k_ac0 <= SYS_STAT) ||
               k_ac0 == SYS_FREEBLK || k_ac0 == SYS_RENAME) {
        /* descriptor/file syscalls (fsys.c; kept out of this function so
         * its branches stay within JMP range -- SHELL_NOTES.md). The fs
         * lock only for calls that can reach fs.c: a console or pipe read
         * may sleep indefinitely and must not hold it, or a pipe's writer
         * could never get in (merge of IO_NOTES.md + SHELL_NOTES.md). */
        unsigned int op = k_ac0, a1 = k_ac1, a2 = k_ac2, a3 = k_ac3;
        int lk = fsys_needslock(op, a1);
        if (lk) fs_lock();
        r = op == SYS_FREEBLK ? fs_info(a1) : fsys_syscall(op, a1, a2, a3);
        if (lk) fs_unlock();
    } else {
        r = 0177777;        /* -1: unknown syscall */
    }
    /* resume the caller: AC0 = result, rest unchanged by SYC. The DIA
     * readback's bit 0 is unreliable after SYC (TRAP_NOTES.md) and is
     * not used -- every process runs with the same P_MAPSTAT. */
    if (p_killed[curproc]) {            /* killed during this syscall */
        proc_exit(curproc, KILLSTAT);
        sched();
        return;
    }
    k_ac0 = r;
    k_mapstat = P_MAPSTAT;
}

#ifdef KDEBUG
static void k_trap2(int cause);
void k_trap(int cause) {
    unsigned int in_pc = k_pc, in_ms = k_mapstat;
    int in_cur = curproc;
    k_trap2(cause);
    if ((k_pc & 077777) < 050) {
        kputs("\r\nKDEBUG resume pc="); kputo(k_pc);
        kputs(" cause="); kputu((unsigned int)cause);
        kputs(" in_pc="); kputo(in_pc);
        kputs(" in_ms="); kputo(in_ms);
        kputs(" in_cur="); kputu((unsigned int)in_cur);
        kputs(" cur="); kputu((unsigned int)curproc);
        kputs(" ac0="); kputo(k_ac0);
        kputs("\r\n");
        k_halt();
    }
}
static void k_trap2(int cause) {
#else
void k_trap(int cause) {
#endif
    if (cause == K_TICK || cause == K_DEV) {
        /* Interrupts reach the kernel only from user mode or from the idle
         * loop (IO_NOTES.md). User mode off must mean idle. */
        if ((k_mapstat & 1) == 0 && curproc >= 0) {
            kputs("\r\nkernel: PANIC interrupt in the kernel, mapstat=");
            kputo(k_mapstat);
            kputs("\r\n");
            k_halt();
        }
        if (cause == K_DEV) {
            if (k_devcode == 010) tty_intr();
            else if (bdev_devcode && k_devcode == bdev_devcode) bdev_intr();
            else {
                kputs("\r\nkernel: PANIC unexpected interrupt, device ");
                kputo(k_devcode);
                kputs("\r\n");
                k_halt();
            }
            if (curproc >= 0) {             /* back to the same user code */
                k_mapstat = P_MAPSTAT;      /* not the DIA readback */
                return;
            }
            sched();                        /* from idle */
        }
        st_ticks++;
        if (curproc >= 0) save_cur();
        sched();
        return;
    }
    if (cause == K_SYS) {
        do_syscall();
        return;
    }
    if (cause == K_FLT) {
        st_faults++;
        /* main() returning: _start runs HALT at 056 right after
         * EJSR main (EclipseAsmPrinter's fixed _start), and an I/O fault
         * reports the next address (manual Ch.2 p.2-30). AC0 holds
         * main's return value: treat it as exit(AC0). */
        if ((k_mapstat & 020000) && (k_pc & 077777) == 057 &&
            kcopyin_word(056) == 063077) {
            st_faults--;                /* a normal exit, not a fault */
            proc_exit(curproc, k_ac0);
            sched();
        }
        else if (k_mapstat & 020000) kill_cur("killed: I/O protection fault");
        /* Code 010000 is write-protect in the manual, but processes run
         * without MapStat's WP-enable bit (020), so it cannot be a real
         * WP fault here: SIMH's PutMap raises this same code for a WRITE
         * to an invalid page (eclipse_cpu.c ~5589). Real hardware may
         * report a validity fault instead (KERNEL_NOTES.md). */
        else if (k_mapstat & 010000) kill_cur("killed: write to unmapped page");
        else if (k_mapstat & 004000) kill_cur("killed: indirect-chain fault");
        else kill_cur("killed: validity fault (read or fetch, unmapped page)");
        return;
    }
    kputs("\r\nkernel: PANIC unknown trap cause\r\n");
    k_halt();
}

/* PIT period: DOA loads the counter, which counts UP and interrupts on
 * wrapping past 0177777 (PROCESS_NOTES.md section 1). */
/* Time slice: the PIT counts at 10 kHz (100 us; eclipse_cpu.c pit_tps,
 * matching DG's PIT) and interrupts when the count wraps past 0177777.
 * 160 counts = 16 ms, about 60 Hz like V6 Unix. It used to be 8 counts
 * (0.8 ms, from PROCESS_NOTES.md's timer probe, where fast ticks made
 * testing easy): a deterministic-clock profile then showed two thirds of
 * all CPU time in the context switch (load), KERNEL_NOTES.md section 11. */
unsigned int k_pitinit = 0200000 - 160;
/* MSKO word: 1 = masked. Only the devices the kernel serves may
 * interrupt -- PIT (01000), TTI (02) and the disk backend's device if it is
 * interrupt-driven. Everything else is masked: a device the kernel never
 * touches could still hold Done from before boot (the tape bootstrap's last
 * read left MTA's Done set, and its interrupt panicked the first tape-booted
 * kernel, FS_NOTES.md section 7). nova_defs.h PI_* bits. */
unsigned int k_pimask = 0177777 & ~(01000u | 02u);

void k_start(void) {
    unsigned int i;
    for (i = 0; i < (NPROC + 1) * KSTK; i++) {
        if ((i & 31) == 0) tty_poll();
        kstk[i] = KSTK_FILL;
    }
    if (bdev_devcode)           /* interrupt-driven disk backend (DSK) */
        k_pimask &= ~0100u;
    asm volatile("NIOC 022" ::: "memory");   /* MTA: drop Done left by the tape boot */
    tty_start();
    asm volatile(
        "ELEF 0,k_stkflt,0\n\t"
        "STA 0,043\n\t"            /* stack overflow -> k_stkflt (HALT) */
        "ELEF 0,k_int,0\n\t"
        "STA 0,1\n\t"
        "ELEF 0,k_syc,0\n\t"
        "STA 0,2\n\t"
        "ELEF 0,k_flt,0\n\t"
        "STA 0,3\n\t"
        "ELDA 0,k_pimask,0\n\t"
        "DOB 0,077\n\t"            /* MSKO: PIT, TTI, DSK may interrupt */
        "ELDA 0,k_pitinit,0\n\t"
        "DOA 0,PIT\n\t"
        "NIOS PIT\n\t"             /* self-arming periodic tick */
        ::: "memory"
    );
    curproc = -1;
    sched();                       /* never returns */
}
