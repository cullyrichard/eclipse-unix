// kentry.s -- hardware entry/exit for the N-process kernel (kernel/).
// Hand-written Eclipse S/140 assembly, appended to the compiled kernel by
// eclipse-cc (a `.s` source). See KERNEL_NOTES.md for the design.
//
// Three entry vectors, one exit path:
//   k_int  -- location 1, device interrupt (the PIT tick).
//   k_syc  -- location 2, SYC (system call).
//   k_flt  -- location 3, MMPU protection fault (validity, write-protect,
//             I/O-protect -- the last is how a user-mode HALT, NIOP MAP
//             or any other I/O instruction lands here).
// Each entry captures the interrupted user context into the k_* globals
// (defined in kernel/proc.c), sets k_cause, resets the kernel stack, and
// calls k_trap(cause) (compiled C). k_trap leaves the context to resume
// -- the same process or a different one -- in the same k_* globals, and
// control falls into k_resume.
//
// Invariants this file relies on (see KERNEL_NOTES.md for evidence):
//   - The kernel itself always runs with ION off. Interrupts are only ever
//     enabled by k_resume's NIOS 077, one instruction before the EJMP into
//     user mode, so every k_int entry interrupted user code, never the
//     kernel. (k_trap cross-checks this via k_mapstat bit 0.)
//   - SYC and protection faults both disable the user map BEFORE pushing
//     their 5-word return block (eclipse_cpu.c: Usermap = 0 precedes the
//     push; TRAP_NOTES.md cites the manual for SYC), so the block always
//     lands on the kernel's own stack in physical page 0, never on the
//     user's -- each process can have a private logical page 0 of its own.
//     Interrupts push nothing; the return PC is in physical location 0.
//   - Only user map A is ever used for processes. The current process's
//     page table is always the one loaded in map A; map B is kernel scratch
//     for far access (kernel/kmem.c).

	dev MAP = 03
	dev PIT = 043

// ---------------------------------------------------------------- k_int
k_int:
	ESTA 0, k_ac0
	DIA 0, MAP		// first MAP access: MapIntMode readback
	ESTA 0, k_mapstat
	ESTA 1, k_ac1
	ESTA 2, k_ac2
	ESTA 3, k_ac3
	LDA 0, 0		// interrupted PC (physical location 0)
	ELDA 1, k_bit15
	MOV# 0, 0, SZC		// carry still the user's: skip if 0
	ADD 1, 0		// carry was 1: fold into bit 15
	ESTA 0, k_pc
	DIB 1, 077		// INTA: device code of the requester
	ESTA 1, k_devcode
	ELDA 0, k_devcode
	ELDA 1, k_pitcode
	SUB# 0, 1, SZR		// skip if device == PIT
	JMP k_int_other
	NIOC PIT		// ack; also cancels the ticking unit ...
	NIOS PIT		// ... so restart it (PROCESS_NOTES.md)
	ELDA 0, k_c_tick
	JMP k_call
k_int_other:
	ELDA 0, k_c_dev
	JMP k_call

// ---------------------------------------------------------------- k_syc
k_syc:
	NIOC 077		// INTDS -- SYSCALL_NOTES.md section 7
	ESTA 0, k_ac0
	DIA 0, MAP		// bit 0 NOT trustworthy after SYC (TRAP_NOTES.md)
	ESTA 0, k_mapstat
	ESTA 1, k_ac1
	ESTA 2, k_ac2
	ESTA 3, k_ac3
	ELDA 0, k_c_sys
	JMP k_popblk

// ---------------------------------------------------------------- k_flt
k_flt:
	ESTA 0, k_ac0		// ION already off (fault path clears INT_ION)
	DIA 0, MAP		// fault code bits are in MapStat
	ESTA 0, k_mapstat
	ESTA 1, k_ac1
	ESTA 2, k_ac2
	ESTA 3, k_ac3
	ELDA 0, k_c_flt

// Pop the 5-word return block (AC0,AC1,AC2,AC3,PC|carry) SYC/fault
// pushed onto the kernel stack. Only the PC word is needed: SYC leaves
// AC0-AC3 unchanged, and a fault's registers are only reported, never
// resumed. AC0 = cause on entry, preserved.
k_popblk:
	LDA 2, 040		// -> last pushed word (PC|carry)
	LDA 1, 0, 2
	ESTA 1, k_pc

// ---------------------------------------------------------------- k_call
// AC0 = cause. Reset the kernel stack to its base (the kernel is never
// re-entered: it runs ION-off, and user code cannot reach it except
// through these vectors), then call k_trap(cause) using the verified
// 1-argument hand-asm -> compiled-C shape from examples/syscall_entry.s.
k_call:
	ELDA 1, k_stackbase
	STA 1, 040
	ISZ 040, 0
	STA 0, @040
	EJSR k_trap, 0
	DSZ 040, 0

// ---------------------------------------------------------------- k_resume
// Enter user mode with the context in k_ac0..k_ac3, k_pc (bit 15 = carry),
// k_mapstat. If k_reload != 0, first LMP k_ptbuf (32 words) into user
// map A. Also jumped to directly by kernel/proc.c's k_start(), which
// also installs the vectors and sets k_stackbase.
k_resume:
	ELDA 0, k_reload
	MOV 0, 0, SNR		// skip if nonzero
	JMP k_noload
	SUB 0, 0
	DOA 0, MAP		// Map Select = user A, user mode off
	ELDA 1, k_c32		// AC1 = word count
	ELEF 2, k_ptbuf		// AC2 = address of the page table
	SUB 0, 0		// AC0 = relocation constant 0
	LMP
	SUB 0, 0
	ESTA 0, k_reload
k_noload:
	ELDA 0, k_mapstat
	DOA 0, MAP		// user mode takes effect at the EJMP below
	ELDA 0, k_pc
	ELDA 1, k_pcmask
	AND 1, 0
	ESTA 0, k_target
	ELDA 0, k_pc
	MOVL 0, 0		// carry := bit 15 of k_pc
	ELDA 0, k_ac0		// ELDA leaves carry alone
	ELDA 1, k_ac1
	ELDA 2, k_ac2
	ELDA 3, k_ac3
	NIOS 077		// INTEN: one more instruction runs first
	EJMP @k_target

	var k_bit15 = 0100000
	var k_pcmask = 077777
	var k_pitcode = 043
	var k_c32 = 32
	var k_c_tick = 1
	var k_c_sys = 2
	var k_c_flt = 3
	var k_c_dev = 4
	var k_target = 0
	var k_stackbase = 0
