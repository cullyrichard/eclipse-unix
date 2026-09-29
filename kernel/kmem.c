#include "kmem.h"

/* Far access for the N-process kernel. Same single-cycle mechanism as
 * examples/mmpu.c (NIOP MAP from supervisor maps exactly the next memory
 * reference through the "Enable" user map -- MMPU_NOTES.md Phase 1), with
 * one deliberate difference: examples/mmpu.c LMPs its one-word scratch
 * PTE into user map A slot 0, which here would silently corrupt the
 * CURRENT process's page 0 mapping (map A always holds the running
 * process's page table -- kernel/kentry.s). So kfar_* use user map B as
 * the scratch map instead, and always leave MapStat as "select A, enable
 * A, user off" (DOA 0) afterwards so kcopyin_word's NIOP goes through A.
 *
 * DOA MAP word bits (MMPU_NOTES.md "two-user-map context switch"):
 *   0400 = Map Select B (the map the next LMP loads)
 *   0004 = A/B = B      (the map NIOP / user mode will use)
 * so 0404 = "load B, single-cycle through B", user mode off.
 *
 * Every value crosses the asm boundary via named globals and AC2/AC3 are
 * saved/restored -- examples/mmpu.c's verified pattern, not re-derived.
 * Callers run with ION off (all kernel code does).
 */

static unsigned int _k_pte;
static unsigned int _k_off;
static unsigned int _k_val;
static unsigned int _k_save2;
static unsigned int _k_save3;
unsigned int _k_selB = 0404;

static void kfar_setB(unsigned int pfn) {
    _k_pte = pfn & 01777;  /* logical slot 0, physical page pfn */
    _k_save2 = 0;
    _k_save3 = 0;
    asm volatile(
        "ESTA 2,_k_save2,0\n\t"
        "ESTA 3,_k_save3,0\n\t"
        "ELDA 0,_k_selB,0\n\t"
        "DOA 0,MAP\n\t"            /* select B, enable B, user off */
        "SUB 1,1\n\t"
        "INC 1,1\n\t"              /* AC1 = 1 word */
        "ELEF 2,_k_pte,0\n\t"
        "SUB 0,0\n\t"              /* AC0 = relocation 0 */
        "LMP\n\t"                  /* Map[B][0] = pfn */
        "ELDA 2,_k_save2,0\n\t"
        "ELDA 3,_k_save3,0\n\t"
    );
}

void kfar_write(unsigned int pfn, unsigned int off, unsigned int val) {
    kfar_setB(pfn);
    _k_off = off & 01777;
    _k_val = val;
    asm volatile(
        "ESTA 2,_k_save2,0\n\t"
        "ESTA 3,_k_save3,0\n\t"
        "ELDA 2,_k_off,0\n\t"
        "ELDA 3,_k_val,0\n\t"
        "NIOP MAP\n\t"
        "ESTA 3,0,2\n\t"           /* the one mapped (through B) write */
        "SUB 0,0\n\t"
        "DOA 0,MAP\n\t"            /* back to select A, enable A */
        "ELDA 2,_k_save2,0\n\t"
        "ELDA 3,_k_save3,0\n\t"
    );
}

unsigned int kfar_read(unsigned int pfn, unsigned int off) {
    kfar_setB(pfn);
    _k_off = off & 01777;
    asm volatile(
        "ESTA 2,_k_save2,0\n\t"
        "ESTA 3,_k_save3,0\n\t"
        "ELDA 2,_k_off,0\n\t"
        "NIOP MAP\n\t"
        "ELDA 3,0,2\n\t"           /* the one mapped (through B) read */
        "ESTA 3,_k_val,0\n\t"
        "SUB 0,0\n\t"
        "DOA 0,MAP\n\t"
        "ELDA 2,_k_save2,0\n\t"
        "ELDA 3,_k_save3,0\n\t"
    );
    return _k_val;
}

/* Read one word at logical address uaddr in the CURRENT process's address
 * space (user map A, loaded by kentry.s). The caller must have checked
 * that uaddr's page is valid: a single-cycle access skips the MMPU's
 * validity check (eclipse_cpu.c: `== INVALID && !SingleCycle`), so an
 * unmapped page would silently read physical page 01777 instead of
 * faulting. */
unsigned int kcopyin_word(unsigned int uaddr) {
    _k_off = uaddr & 077777;
    _k_save2 = 0;
    _k_save3 = 0;
    asm volatile(
        "ESTA 2,_k_save2,0\n\t"
        "ESTA 3,_k_save3,0\n\t"
        "SUB 0,0\n\t"
        "DOA 0,MAP\n\t"            /* enable A (the current process) */
        "ELDA 2,_k_off,0\n\t"
        "NIOP MAP\n\t"
        "ELDA 3,0,2\n\t"
        "ESTA 3,_k_val,0\n\t"
        "ELDA 2,_k_save2,0\n\t"
        "ELDA 3,_k_save3,0\n\t"
    );
    return _k_val;
}
