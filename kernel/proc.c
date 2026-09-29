#include "proc.h"
#include "kmem.h"
#include "kio.h"
#include "ksyscall.h"
#include "../examples/pmm.h"
#include "../examples/console.h"

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
/* Big-memory segments (KERNEL_NOTES.md section 9): extra physical frames a
 * process owns beyond its 32KW logical space, reachable only by mapping
 * them into a "window" of its logical pages with SYS_MAPWIN.
 * p_fixed marks the pages proc_create mapped (image + stack): those can
 * never be remapped, so a window can't pull the caller's own code or
 * stack out from under it. */
unsigned int p_fixed[NPROC * NPAGES];
unsigned int p_seg[NPROC * MAXSEG];
unsigned int p_nseg[NPROC];

int curproc = -1;
int loaded = -1;            /* whose page table is in map A */

/* statistics, printed at shutdown */
unsigned int st_ticks, st_switches, st_syscalls, st_faults;

static void k_halt(void) {
    asm volatile("HALT\n\t");
}

static int pte_valid(unsigned int pte) {
    return (pte & PTE_INVALID) != PTE_INVALID;
}

int proc_create(const unsigned int *words, unsigned int nwords,
                unsigned int base, unsigned int stacktop,
                unsigned int stackwords) {
    int n, s;
    unsigned int i, a, pfn, lastimg, sfirst, slast, pt0;

    n = -1;
    for (s = 0; s < NPROC; s++) {
        if (p_state[s] == P_FREE) { n = s; break; }
    }
    if (n < 0) return -1;

    pt0 = (unsigned int)n * NPAGES;
    lastimg = (base + nwords - 1) >> 10;
    sfirst = stacktop >> 10;
    slast = (stacktop + stackwords - 1) >> 10;
    if (slast > NPAGES - 1) slast = NPAGES - 1;

    p_nframes[n] = 0;
    p_nseg[n] = 0;
#ifdef KDEBUG
    kputs("  dbg lastimg="); kputu(lastimg);
    kputs(" sfirst="); kputu(sfirst);
    kputs(" slast="); kputu(slast);
    kputs(" stacktop="); kputo(stacktop);
    kputs("\r\n");
#endif
    for (s = 0; s < NPAGES; s++) {
        p_pt[pt0 + s] = PTE_INVALID | ((unsigned int)s << 10);
        p_fixed[pt0 + s] = 0;
        if ((unsigned int)s <= lastimg ||
            ((unsigned int)s >= sfirst && (unsigned int)s <= slast)) {
            pfn = (unsigned int)pmm_alloc_page();
            if (pfn == (unsigned int)PMM_NONE) return -1;
            p_pt[pt0 + s] = ((unsigned int)s << 10) | pfn;
            p_fixed[pt0 + s] = 1;
            p_nframes[n]++;
            for (i = 0; i < 1024; i++) kfar_write(pfn, i, 0);
        }
    }
    for (i = 0; i < nwords; i++) {
        a = base + i;
        pfn = p_pt[pt0 + (a >> 10)] & 01777;
        kfar_write(pfn, a & 01777, words[i]);
    }

    p_ac0[n] = 0; p_ac1[n] = 0; p_ac2[n] = 0; p_ac3[n] = 0;
    p_pc[n] = base;         /* _start; carry clear */
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
        pte = p_pt[pt0 + s];
        if (p_fixed[pt0 + s] && pte_valid(pte))
            pmm_free_page((int)(pte & 01777));
    }
    for (s = 0; (unsigned int)s < p_nseg[n]; s++)
        pmm_free_page((int)p_seg[sg0 + s]);
    p_nseg[n] = 0;
    p_state[n] = P_ZOMBIE;
    if (loaded == n) loaded = -1;
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

static void load(int n) {
    int s;
    if (n != loaded) {
        for (s = 0; s < NPAGES; s++)
            k_ptbuf[s] = p_pt[(unsigned int)n * NPAGES + s];
        k_reload = 1;
        loaded = n;
    }
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
    kputs("\r\n");
    k_halt();
}

/* Round-robin: the next P_RUN slot after curproc (possibly curproc). */
static void sched(void) {
    int i, n;
    n = curproc;
    for (i = 0; i < NPROC; i++) {
        n = n + 1;
        if (n >= NPROC) n = 0;
        if (p_state[n] == P_RUN) {
            load(n);
            return;
        }
    }
    shutdown();
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
    proc_free(curproc);
    sched();
}

static int sys_write_user(unsigned int buf, unsigned int len) {
    unsigned int i, a, pt0;
    pt0 = (unsigned int)curproc * NPAGES;
    for (i = 0; i < len; i++) {
        a = buf + i;
        if (a > 077777 || !pte_valid(p_pt[pt0 + (a >> 10)])) return -1;
    }
    for (i = 0; i < len; i++)
        console_putchar((int)(kcopyin_word(buf + i) & 0377));
    return (int)len;
}

static void do_syscall(void) {
    unsigned int r;
    st_syscalls++;
    r = 0;
    if (k_ac0 == SYS_PUTCHAR) {
        console_putchar((int)k_ac1);
    } else if (k_ac0 == SYS_GETCHAR) {
        r = (unsigned int)console_getchar();
    } else if (k_ac0 == SYS_GETPID) {
        r = (unsigned int)curproc;
    } else if (k_ac0 == SYS_SEGALLOC) {
        r = sys_segalloc(k_ac1);
    } else if (k_ac0 == SYS_MAPWIN) {
        r = sys_mapwin(k_ac1, k_ac2);
    } else if (k_ac0 == SYS_WRITE) {
        r = (unsigned int)sys_write_user(k_ac1, k_ac2);
    } else if (k_ac0 == SYS_EXIT) {
        kputs("\r\nkernel: pid ");
        kputu((unsigned int)curproc);
        kputs(" exited, status ");
        kputu(k_ac1);
        kputs("\r\n");
        proc_free(curproc);
        sched();
        return;
    } else {
        r = 0177777;        /* -1: unknown syscall */
    }
    /* resume the caller: AC0 = result, rest unchanged by SYC. The DIA
     * readback's bit 0 is unreliable after SYC (TRAP_NOTES.md) and is
     * not used -- every process runs with the same P_MAPSTAT. */
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
    if (cause == K_TICK) {
        st_ticks++;
        if ((k_mapstat & 1) == 0) {
            /* would mean an interrupt reached the kernel itself, which
             * the ION-off-in-kernel invariant says cannot happen */
            kputs("\r\nkernel: PANIC tick with user mode off, mapstat=");
            kputo(k_mapstat);
            kputs("\r\n");
            k_halt();
        }
        save_cur();
        sched();
        return;
    }
    if (cause == K_SYS) {
        do_syscall();
        return;
    }
    if (cause == K_FLT) {
        st_faults++;
        if (k_mapstat & 020000) kill_cur("killed: I/O protection fault");
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
    kputs("\r\nkernel: PANIC unexpected interrupt, device ");
    kputo(k_devcode);
    kputs("\r\n");
    k_halt();
}

/* PIT period: DOA loads the counter, which counts UP and interrupts on
 * wrapping past 0177777 (PROCESS_NOTES.md section 1). */
unsigned int k_pitinit = 0177770;
unsigned int k_pimask = 0103;    /* PI_TTI|PI_TTO|PI_DSK */

void k_start(void) {
    asm volatile(
        "ELEF 0,k_int,0\n\t"
        "STA 0,1\n\t"
        "ELEF 0,k_syc,0\n\t"
        "STA 0,2\n\t"
        "ELEF 0,k_flt,0\n\t"
        "STA 0,3\n\t"
        "ELDA 0,k_pimask,0\n\t"
        "DOB 0,077\n\t"            /* MSKO: only the PIT may interrupt */
        "ELDA 0,k_pitinit,0\n\t"
        "DOA 0,PIT\n\t"
        "NIOS PIT\n\t"             /* self-arming periodic tick */
        "LDA 0,040\n\t"
        "ESTA 0,k_stackbase,0\n\t" /* trap-entry kernel stack base */
    );
    curproc = -1;
    sched();                       /* halts if nothing is runnable */
    asm volatile("EJMP k_resume,0\n\t");
}
