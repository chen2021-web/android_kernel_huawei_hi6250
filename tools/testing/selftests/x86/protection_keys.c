/*
 * Tests x86 Memory Protection Keys (see Documentation/x86/protection-keys.txt)
 *
 * There are examples in here of:
 *  * how to set protection keys on memory
 *  * how to set/clear bits in PKRU (the rights register)
 *  * how to handle SEGV_PKRU signals and extract pkey-relevant
 *    information from the siginfo
 *
 * Things to add:
 *	make sure KSM and KSM COW breaking works
 *	prefault pages in at malloc, or not
 *	protect MPX bounds tables with protection keys?
 *	make sure VMA splitting/merging is working correctly
 *	OOMs can destroy mm->mmap (see exit_mmap()), so make sure it is immune to pkeys
 *	look for pkey "leaks" where it is still set on a VMA but "freed" back to the kernel
 *	do a plain mprotect() to a mprotect_pkey() area and make sure the pkey sticks
 *
 * Compile like this:
 *	gcc      -o protection_keys    -O2 -g -std=gnu99 -pthread -Wall protection_keys.c -lrt -ldl -lm
 *	gcc -m32 -o protection_keys_32 -O2 -g -std=gnu99 -pthread -Wall protection_keys.c -lrt -ldl -lm
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <time.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <assert.h>
#include <stdlib.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <setjmp.h>

#include "pkey-helpers.h"

int iteration_nr = 1;
int test_nr;

unsigned int shadow_pkru;

#define HPAGE_SIZE	(1UL<<21)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))
#define ALIGN_UP(x, align_to)	(((x) + ((align_to)-1)) & ~((align_to)-1))
#define ALIGN_DOWN(x, align_to) ((x) & ~((align_to)-1))
#define ALIGN_PTR_UP(p, ptr_align_to)	((typeof(p))ALIGN_UP((unsigned long)(p),	ptr_align_to))
#define ALIGN_PTR_DOWN(p, ptr_align_to)	((typeof(p))ALIGN_DOWN((unsigned long)(p),	ptr_align_to))
#define __stringify_1(x...)     #x
#define __stringify(x...)       __stringify_1(x)

#define PTR_ERR_ENOTSUP ((void *)-ENOTSUP)

int dprint_in_signal;
char dprint_in_signal_buffer[DPRINT_IN_SIGNAL_BUF_SIZE];

extern void abort_hooks(void);
#define pkey_assert(condition) do {		\
	if (!(condition)) {			\
		dprintf0("assert() at %s::%d test_nr: %d iteration: %d\n", \
				__FILE__, __LINE__,	\
				test_nr, iteration_nr);	\
		dprintf0("errno at assert: %d", errno);	\
		abort_hooks();			\
		assert(condition);		\
	}					\
} while (0)
#define raw_assert(cond) assert(cond)

void cat_into_file(char *str, char *file)
{
	int fd = open(file, O_RDWR);
	int ret;

	dprintf2("%s(): writing '%s' to '%s'\n", __func__, str, file);
	/*
	 * these need to be raw because they are called under
	 * pkey_assert()
	 */
	raw_assert(fd >= 0);
	ret = write(fd, str, strlen(str));
	if (ret != strlen(str)) {
		perror("write to file failed");
		fprintf(stderr, "filename: '%s' str: '%s'\n", file, str);
		raw_assert(0);
	}
	close(fd);
}

#if CONTROL_TRACING > 0
static int warned_tracing;
int tracing_root_ok(void)
{
	if (geteuid() != 0) {
		if (!warned_tracing)
			fprintf(stderr, "WARNING: not run as root, "
					"can not do tracing control\n");
		warned_tracing = 1;
		return 0;
	}
	return 1;
}
#endif

void tracing_on(void)
{
#if CONTROL_TRACING > 0
#define TRACEDIR "/sys/kernel/debug/tracing"
	char pidstr[32];

	if (!tracing_root_ok())
		return;

	sprintf(pidstr, "%d", getpid());
	cat_into_file("0", TRACEDIR "/tracing_on");
	cat_into_file("\n", TRACEDIR "/trace");
	if (1) {
		cat_into_file("function_graph", TRACEDIR "/current_tracer");
		cat_into_file("1", TRACEDIR "/options/funcgraph-proc");
	} else {
		cat_into_file("nop", TRACEDIR "/current_tracer");
	}
	cat_into_file(pidstr, TRACEDIR "/set_ftrace_pid");
	cat_into_file("1", TRACEDIR "/tracing_on");
	dprintf1("enabled tracing\n");
#endif
}

void tracing_off(void)
{
#if CONTROL_TRACING > 0
	if (!tracing_root_ok())
		return;
	cat_into_file("0", "/sys/kernel/debug/tracing/tracing_on");
#endif
}

void abort_hooks(void)
{
	fprintf(stderr, "running %s()...\n", __func__);
	tracing_off();
#ifdef SLEEP_ON_ABORT
	sleep(SLEEP_ON_ABORT);
#endif
}

static inline void __page_o_noops(void)
{
	/* 8-bytes of instruction * 512 bytes = 1 page */
	asm(".rept 512 ; nopl 0x7eeeeeee(%eax) ; .endr");
}

/*
 * This attempts to have roughly a page of instructions followed by a few
 * instructions that do a write, and another page of instructions.  That
 * way, we are pretty sure that the write is in the second page of
 * instructions and has at least a page of padding behind it.
 *
 * *That* lets us be sure to madvise() away the write instruction, which
 * will then fault, which makes sure that the fault code handles
 * execute-only memory properly.
 */
__attribute__((__aligned__(PAGE_SIZE)))
void lots_o_noops_around_write(int *write_to_me)
{
	dprintf3("running %s()\n", __func__);
	__page_o_noops();
	/* Assume this happens in the second page of instructions: */
	*write_to_me = __LINE__;
	/* pad out by another page: */
	__page_o_noops();
	dprintf3("%s() done\n", __func__);
}

/* Define some kernel-like types */
#define  u8 uint8_t
#define u16 uint16_t
#define u32 uint32_t
#define u64 uint64_t

#ifdef __i386__
#define SYS_mprotect_key 380
#define SYS_pkey_alloc	 381
#define SYS_pkey_free	 382
#define REG_IP_IDX REG_EIP
#define si_pkey_offset 0x18
#else
#define SYS_mprotect_key 329
#define SYS_pkey_alloc	 330
#define SYS_pkey_free	 331
#define REG_IP_IDX REG_RIP
#define si_pkey_offset 0x20
#endif

void dump_mem(void *dumpme, int len_bytes)
{
	char *c = (void *)dumpme;
	int i;

	for (i = 0; i < len_bytes; i += sizeof(u64)) {
		u64 *ptr = (u64 *)(c + i);
		dprintf1("dump[%03d][@%p]: %016jx\n", i, ptr, *ptr);
	}
}

#define __SI_FAULT      (3 << 16)
#define SEGV_BNDERR     (__SI_FAULT|3)  /* failed address bound checks */
#define SEGV_PKUERR     (__SI_FAULT|4)

static char *si_code_str(int si_code)
{
	if (si_code & SEGV_MAPERR)
		return "SEGV_MAPERR";
	if (si_code & SEGV_ACCERR)
		return "SEGV_ACCERR";
	if (si_code & SEGV_BNDERR)
		return "SEGV_BNDERR";
	if (si_code & SEGV_PKUERR)
		return "SEGV_PKUERR";
	return "UNKNOWN";
}

int pkru_faults;
int last_si_pkey = -1;
void signal_handler(int signum, siginfo_t *si, void *vucontext)
{
	ucontext_t *uctxt = vucontext;
	int trapno;
	unsigned long ip;
	char *fpregs;
	u32 *pkru_ptr;
	u64 si_pkey;
	u32 *si_pkey_ptr;
	int pkru_offset;
	fpregset_t fpregset;

	dprint_in_signal = 1;
	dprintf1(">>>>===============SIGSEGV============================\n");
	dprintf1("%s()::%d, pkru: 0x%x shadow: %x\n", __func__, __LINE__,
			__rdpkru(), shadow_pkru);

	trapno = uctxt->uc_mcontext.gregs[REG_TRAPNO];
	ip = uctxt->uc_mcontext.gregs[REG_IP_IDX];
	fpregset = uctxt->uc_mcontext.fpregs;
	fpregs = (void *)fpregset;

	dprintf2("%s() trapno: %d ip: 0x%lx info->si_code: %s/%d\n", __func__,
			trapno, ip, si_code_str(si->si_code), si->si_code);
#ifdef __i386__
	/*
	 * 32-bit has some extra padding so that userspace can tell whether
	 * the XSTATE header is present in addition to the "legacy" FPU
	 * state.  We just assume that it is here.
	 */
	fpregs += 0x70;
#endif
	pkru_offset = pkru_xstate_offset();
	pkru_ptr = (void *)(&fpregs[pkru_offset]);

	dprintf1("siginfo: %p\n", si);
	dprintf1(" fpregs: %p\n", fpregs);
	/*
	 * If we got a PKRU fault, we *HAVE* to have at least one bit set in
	 * here.
	 */
	dprintf1("pkru_xstate_offset: %d\n", pkru_xstate_offset());
	if (DEBUG_LEVEL > 4)
		dump_mem(pkru_ptr - 128, 256);
	pkey_assert(*pkru_ptr);

	si_pkey_ptr = (u32 *)(((u8 *)si) + si_pkey_offset);
	dprintf1("si_pkey_ptr: %p\n", si_pkey_ptr);
	dump_mem(si_pkey_ptr - 8, 24);
	si_pkey = *si_pkey_ptr;
	pkey_assert(si_pkey < NR_PKEYS);
	last_si_pkey = si_pkey;

	if ((si->si_code == SEGV_MAPERR) ||
	    (si->si_code == SEGV_ACCERR) ||
	    (si->si_code == SEGV_BNDERR)) {
		printf("non-PK si_code, exiting...\n");
		exit(4);
	}

	dprintf1("signal pkru from xsave: %08x\n", *pkru_ptr);
	/* need __rdpkru() version so we do not do shadow_pkru checking */
	dprintf1("signal pkru from  pkru: %08x\n", __rdpkru());
	dprintf1("si_pkey from siginfo: %jx\n", si_pkey);
	*(u64 *)pkru_ptr = 0x00000000;
	dprintf1("WARNING: set PRKU=0 to allow faulting instruction to continue\n");
	pkru_faults++;
	dprintf1("<<<<==================================================\n");
	return;
	if (trapno == 14) {
		fprintf(stderr,
			"ERROR: In signal handler, page fault, trapno = %d, ip = %016lx\n",
			trapno, ip);
		fprintf(stderr, "si_addr %p\n", si->si_addr);
		fprintf(stderr, "REG_ERR: %lx\n",
				(unsigned long)uctxt->uc_mcontext.gregs[REG_ERR]);
		exit(1);
	} else {
		fprintf(stderr, "unexpected trap %d! at 0x%lx\n", trapno, ip);
		fprintf(stderr, "si_addr %p\n", si->si_addr);
		fprintf(stderr, "REG_ERR: %lx\n",
				(unsigned long)uctxt->uc_mcontext.gregs[REG_ERR]);
		exit(2);
	}
	dprint_in_signal = 0;
}

int wait_all_children(void)
{
	int status;
	return waitpid(-1, &status, 0);
}

void sig_chld(int x)
{
	dprint_in_signal = 1;
	dprintf2("[%d] SIGCHLD: %d\n", getpid(), x);
	dprint_in_signal = 0;
}

void setup_sigsegv_handler(void)
{
	int r, rs;
	struct sigaction newact;
	struct sigaction oldact;

	/* #PF is mapped to sigsegv */
	int signum  = SIGSEGV;

	newact.sa_handler = 0;
	newact.sa_sigaction = signal_handler;

	/*sigset_t - signals to block while in the handler */
	/* get the old signal mask. */
	rs = sigprocmask(SIG_SETMASK, 0, &newact.sa_mask);
	pkey_assert(rs == 0);

	/* call sa_sigaction, not sa_handler*/
	newact.sa_flags = SA_SIGINFO;

	newact.sa_restorer = 0;  /* void(*)(), obsolete */
	r = sigaction(signum, &newact, &oldact);
	r = sigaction(SIGALRM, &newact, &oldact);
	pkey_assert(r == 0);
}

void setup_handlers(void)
{
	signal(SIGCHLD, &sig_chld);
	setup_sigsegv_handler();
}

pid_t fork_lazy_child(void)
{
	pid_t forkret;

	forkret = fork();
	pkey_assert(forkret >= 0);
	dprintf3("[%d] fork() ret: %d\n", getpid(), forkret);

	if (!forkret) {
		/* in the child */
		while (1) {
			dprintf1("child sleeping...\n");
			sleep(30);
		}
	}
	return forkret;
}

void davecmp(void *_a, void *_b, int len)
{
	int i;
	unsigned long *a = _a;
	unsigned long *b = _b;

	for (i = 0; i < len / sizeof(*a); i++) {
		if (a[i] == b[i])
			continue;

		dprintf3("[%3d]: a: %016lx b: %016lx\n", i, a[i], b[i]);
	}
}

void dumpit(char *f)
{
	int fd = open(f, O_RDONLY);
	char buf[100];
	int nr_read;

	dprintf2("maps fd: %d\n", fd);
	do {
		nr_read = read(fd, &buf[0], sizeof(buf));
		write(1, buf, nr_read);
	} while (nr_read > 0);
	close(fd);
}

#define PKEY_DISABLE_ACCESS    0x1
#define PKEY_DISABLE_WRITE     0x2

u32 pkey_get(int pkey, unsigned long flags)
{
	u32 mask = (PKEY_DISABLE_ACCESS|PKEY_DISABLE_WRITE);
	u32 pkru = __rdpkru();
	u32 shifted_pkru;
	u32 masked_pkru;

	dprintf1("%s(pkey=%d, flags=%lx) = %x / %d\n",
			__func__, pkey, flags, 0, 0);
	dprintf2("%s() raw pkru: %x\n", __func__, pkru);

	shifted_pkru = (pkru >> (pkey * PKRU_BITS_PER_PKEY));
	dprintf2("%s() shifted_pkru: %x\n", __func__, shifted_pkru);
	masked_pkru = shifted_pkru & mask;
	dprintf2("%s() masked  pkru: %x\n", __func__, masked_pkru);
	/*
	 * shift down the relevant bits to the lowest two, then
	 * mask off all the other high bits.
	 */
	return masked_pkru;
}

int pkey_set(int pkey, unsigned long rights, unsigned long flags)
{
	u32 mask = (PKEY_DISABLE_ACCESS|PKEY_DISABLE_WRITE);
	u32 old_pkru = __rdpkru();
	u32 new_pkru;

	/* make sure that 'rights' only contains the bits we expect: */
	assert(!(rights & ~mask));

	/* copy old pkru */
	new_pkru = old_pkru;
	/* mask out bits from pkey in old value: */
	new_pkru &= ~(mask << (pkey * PKRU_BITS_PER_PKEY));
	/* OR in new bits for pkey: */
	new_pkru |= (rights << (pkey * PKRU_BITS_PER_PKEY));

	__wrpkru(new_pkru);

	dprintf3("%s(pkey=%d, rights=%lx, flags=%lx) = %x pkru now: %x old_pkru: %x\n",
			__func__, pkey, rights, flags, 0, __rdpkru(), old_pkru);
	return 0;
}

void pkey_disable_set(int pkey, int flags)
{
	unsigned long syscall_flags = 0;
	int ret;
	int pkey_rights;
	u32 orig_pkru;

	dprintf1("START->%s(%d, 0x%x)\n", __func__,
		pkey, flags);
	pkey_assert(flags & (PKEY_DISABLE_ACCESS | PKEY_DISABLE_WRITE));

	pkey_rights = pkey_get(pkey, syscall_flags);

	dprintf1("%s(%d) pkey_get(%d): %x\n", __func__,
			pkey, pkey, pkey_rights);
	pkey_assert(pkey_rights >= 0);

	pkey_rights |= flags;

	ret = pkey_set(pkey, pkey_rights, syscall_flags);
	assert(!ret);
	/*pkru and flags have the same format */
	shadow_pkru |= flags << (pkey * 2);
	dprintf1("%s(%d) shadow: 0x%x\n", __func__, pkey, shadow_pkru);

	pkey_assert(ret >= 0);

	pkey_rights = pkey_get(pkey, syscall_flags);
	dprintf1("%s(%d) pkey_get(%d): %x\n", __func__,
			pkey, pkey, pkey_rights);

	dprintf1("%s(%d) pkru: 0x%x\n", __func__, pkey, rdpkru());
	if (flags)
		pkey_assert(rdpkru() > orig_pkru);
	dprintf1("END<---%s(%d, 0x%x)\n", __func__,
		pkey, flags);
}

void pkey_disable_clear(int pkey, int flags)
{
	unsigned long syscall_flags = 0;
	int ret;
	int pkey_rights = pkey_get(pkey, syscall_flags);
	u32 orig_pkru = rdpkru();

	pkey_assert(flags & (PKEY_DISABLE_ACCESS | PKEY_DISABLE_WRITE));

	dprintf1("%s(%d) pkey_get(%d): %x\n", __func__,
			pkey, pkey, pkey_rights);
	pkey_assert(pkey_rights >= 0);

	pkey_rights |= flags;

	ret = pkey_set(pkey, pkey_rights, 0);
	/* pkru and flags have the same format */
	shadow_pkru &= ~(flags << (pkey * 2));
	pkey_assert(ret >= 0);

	pkey_rights = pkey_get(pkey, syscall_flags);
	dprintf1("%s(%d) pkey_get(%d): %x\n", __func__,
			pkey, pkey, pkey_rights);

	dprintf1("%s(%d) pkru: 0x%x\n", __func__, pkey, rdpkru());
	if (flags)
		assert(rdpkru() > orig_pkru);
}

void pkey_write_allow(int pkey)
{
	pkey_disable_clear(pkey, PKEY_DISABLE_WRITE);
}
void pkey_write_deny(int pkey)
{
	pkey_disable_set(pkey, PKEY_DISABLE_WRITE);
}
void pkey_access_allow(int pkey)
{
	pkey_disable_clear(pkey, PKEY_DISABLE_ACCESS);
}
void pkey_access_deny(int pkey)
{
	pkey_disable_set(pkey, PKEY_DISABLE_ACCESS);
}

int sys_mprotect_pkey(void *ptr, size_t size, unsigned long orig_prot,
		unsigned long pkey)
{
	int sret;

	dprintf2("%s(0x%p, %zx, prot=%lx, pkey=%lx)\n", __func__,
			ptr, size, orig_prot, pkey);

	errno = 0;
	sret = syscall(SYS_mprotect_key, ptr, size, orig_prot, pkey);
	if (errno) {
		dprintf2("SYS_mprotect_key sret: %d\n", sret);
		dprintf2("SYS_mprotect_key prot: 0x%lx\n", orig_prot);
		dprintf2("SYS_mprotect_key failed, errno: %d\n", errno);
		if (DEBUG_LEVEL >= 2)
			perror("SYS_mprotect_pkey");
	}
	return sret;
}

int sys_pkey_alloc(unsigned long flags, unsigned long init_val)
{
	int ret = syscall(SYS_pkey_alloc, flags, init_val);
	dprintf1("%s(flags=%lx, init_val=%lx) syscall ret: %d errno: %d\n",
			__func__, flags, init_val, ret, errno);
	return ret;
}

int alloc_pkey(void)
{
	int ret;
	unsigned long init_val = 0x0;

	dprintf1("alloc_pkey()::%d, pkru: 0x%x shadow: %x\n",
			__LINE__, __rdpkru(), shadow_pkru);
	ret = sys_pkey_alloc(0, init_val);
	/*
	 * pkey_alloc() sets PKRU, so we need to reflect it in
	 * shadow_pkru:
	 */
	dprintf4("alloc_pkey()::%d, ret: %d pkru: 0x%x shadow: 0x%x\n",
			__LINE__, ret, __rdpkru(), shadow_pkru);
	if (ret) {
		/* clear both the bits: */
		shadow_pkru &= ~(0x3      << (ret * 2));
		dprintf4("alloc_pkey()::%d, ret: %d pkru: 0x%x shadow: 0x%x\n",
				__LINE__, ret, __rdpkru(), shadow_pkru);
		/*
		 * move the new state in from init_val
		 * (remember, we cheated and init_val == pkru format)
		 */
		shadow_pkru |=  (init_val << (ret * 2));
	}
	dprintf4("alloc_pkey()::%d, ret: %d pkru: 0x%x shadow: 0x%x\n",
			__LINE__, ret, __rdpkru(), shadow_pkru);
	dprintf1("alloc_pkey()::%d errno: %d\n", __LINE__, errno);
	/* for shadow checking: */
	rdpkru();
	dprintf4("alloc_pkey()::%d, ret: %d pkru: 0x%x shadow: 0x%x\n",
			__LINE__, ret, __rdpkru(), shadow_pkru);
	return ret;
}

int sys_pkey_free(unsigned long pkey)
{
	int ret = syscall(SYS_pkey_free, pkey);
	dprintf1("%s(pkey=%ld) syscall ret: %d\n", __func__, pkey, ret);
	return ret;
}

/*
 * I had a bug where pkey bits could be set by mprotect() but
 * not cleared.  This ensures we get lots of random bit sets
 * and clears on the vma and pte pkey bits.
 */
int alloc_random_pkey(void)
{
	int max_nr_pkey_allocs;
	int ret;
	int i;
	int alloced_pkeys[NR_PKEYS];
	int nr_alloced = 0;
	int random_index;
	memset(alloced_pkeys, 0, sizeof(alloced_pkeys));
	srand((unsigned int)time(NULL));

	/* allocate every possible key and make a note of which ones we got */
	max_nr_pkey_allocs = NR_PKEYS;
	for (i = 0; i < max_nr_pkey_allocs; i++) {
		int new_pkey = alloc_pkey();
		if (new_pkey < 0)
			break;
		alloced_pkeys[nr_alloced++] = new_pkey;
	}

	pkey_assert(nr_alloced > 0);
	/* select a random one out of the allocated ones */
	random_index = rand() % nr_alloced;
	ret = alloced_pkeys[random_index];
	/* now zero it out so we don't free it next */
	alloced_pkeys[random_index] = 0;

	/* go through the allocated ones that we did not want and free them */
	for (i = 0; i < nr_alloced; i++) {
		int free_ret;
		if (!alloced_pkeys[i])
			continue;
		free_ret = sys_pkey_free(alloced_pkeys[i]);
		pkey_assert(!free_ret);
	}
	dprintf1("%s()::%d, ret: %d pkru: 0x%x shadow: 0x%x\n", __func__,
			__LINE__, ret, __rdpkru(), shadow_pkru);
	return ret;
}

int mprotect_pkey(void *ptr, size_t size, unsigned long orig_prot,
		unsigned long pkey)
{
	int nr_iterations = random() % 100;
	int ret;

	while (0) {
		int rpkey = alloc_random_pkey();
		ret = sys_mprotect_pkey(ptr, size, orig_prot, pkey);
		dprintf1("sys_mprotect_pkey(%p, %zx, prot=0x%lx, pkey=%ld) ret: %d\n",
				ptr, size, orig_prot, pkey, ret);
		if (nr_iterations-- < 0)
			break;

		dprintf1("%s()::%d, ret: %d pkru: 0x%x shadow: 0x%x\n", __func__,
			__LINE__, ret, __rdpkru(), shadow_pkru);
		sys_pkey_free(rpkey);
		dprintf1("%s()::%d, ret: %d pkru: 0x%x shadow: 0x%x\n", __func__,
			__LINE__, ret, __rdpkru(), shadow_pkru);
	}
	pkey_assert(pkey < NR_PKEYS);

	ret = sys_mprotect_pkey(ptr, size, orig_prot, pkey);
	dprintf1("mprotect_pkey(%p, %zx, prot=0x%lx, pkey=%ld) ret: %d\n",
			ptr, size, orig_prot, pkey, ret);
	pkey_assert(!ret);
	dprintf1("%s()::%d, ret: %d pkru: 0x%x shadow: 0x%x\n", __func__,
			__LINE__, ret, __rdpkru(), shadow_pkru);
	return ret;
}

struct pkey_malloc_record {
	void *ptr;
	long size;
};
struct pkey_malloc_record *pkey_malloc_records;
long nr_pkey_malloc_records;
void record_pkey_malloc(void *ptr, long size)
{
	long i;
	struct pkey_malloc_record *rec = NULL;

	for (i = 0; i < nr_pkey_malloc_records; i++) {
		rec = &pkey_malloc_records[i];
		/* find a free record */
		if (rec)
			break;
	}
	if (!rec) {
		/* every record is full */
		size_t old_nr_records = nr_pkey_malloc_records;
		size_t new_nr_records = (nr_pkey_malloc_records * 2 + 1);
		size_t new_size = new_nr_records * sizeof(struct pkey_malloc_record);
		dprintf2("new_nr_records: %zd\n", new_nr_records);
		dprintf2("new_size: %zd\n", new_size);
		pkey_malloc_records = realloc(pkey_malloc_records, new_size);
		pkey_assert(pkey_malloc_records != NULL);
		rec = &pkey_malloc_records[nr_pkey_malloc_records];
		/*
		 * realloc() does not initialize memory, so zero it from
		 * the first new record all the way to the end.
		 */
		for (i = 0; i < new_nr_records - old_nr_records; i++)
			memset(rec + i, 0, sizeof(*rec));
	}
	dprintf3("filling malloc record[%d/%p]: {%p, %ld}\n",
		(int)(rec - pkey_malloc_records), rec, ptr, size);
	rec->ptr = ptr;
	rec->size = size;
	nr_pkey_malloc_records++;
}

void free_pkey_malloc(void *ptr)
{
	long i;
	int ret;
	dprintf3("%s(%p)\n", __func__, ptr);
	for (i = 0; i < nr_pkey_malloc_records; i++) {
		struct pkey_malloc_record *rec = &pkey_malloc_records[i];
		dprintf4("looking for ptr %p at record[%ld/%p]: {%p, %ld}\n",
				ptr, i, rec, rec->ptr, rec->size);
		if ((ptr <  rec->ptr) ||
		    (ptr >= rec->ptr + rec->size))
			continue;

		dprintf3("found ptr %p at record[%ld/%p]: {%p, %ld}\n",
				ptr, i, rec, rec->ptr, rec->size);
		nr_pkey_malloc_records--;
		ret = munmap(rec->ptr, rec->size);
		dprintf3("munmap ret: %d\n", ret);
		pkey_assert(!ret);
		dprintf3("clearing rec->ptr, rec: %p\n", rec);
		rec->ptr = NULL;
		dprintf3("done clearing rec->ptr, rec: %p\n", rec);
		return;
	}
	pkey_assert(false);
}


void *malloc_pkey_with_mprotect(long size, int prot, u16 pkey)
{
	void *ptr;
	int ret;

	rdpkru();
	dprintf1("doing %s(size=%ld, prot=0x%x, pkey=%d)\n", __func__,
			size, prot, pkey);
	pkey_assert(pkey < NR_PKEYS);
	ptr = mmap(NULL, size, prot, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
	pkey_assert(ptr != (void *)-1);
	ret = mprotect_pkey((void *)ptr, PAGE_SIZE, prot, pkey);
	pkey_assert(!ret);
	record_pkey_malloc(ptr, size);
	rdpkru();

	dprintf1("%s() for pkey %d @ %p\n", __func__, pkey, ptr);
	return ptr;
}

void *malloc_pkey_anon_huge(long size, int prot, u16 pkey)
{
	int ret;
	void *ptr;

	dprintf1("doing %s(size=%ld, prot=0x%x, pkey=%d)\n", __func__,
			size, prot, pkey);
	/*
	 * Guarantee we can fit at least one huge page in the resulting
	 * allocation by allocating space for 2:
	 */
	size = ALIGN_UP(size, HPAGE_SIZE * 2);
	ptr = mmap(NULL, size, PROT_NONE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
	pkey_assert(ptr != (void *)-1);
	record_pkey_malloc(ptr, size);
	mprotect_pkey(ptr, size, prot, pkey);

	dprintf1("unaligned ptr: %p\n", ptr);
	ptr = ALIGN_PTR_UP(ptr, HPAGE_SIZE);
	dprintf1("  aligned ptr: %p\n", ptr);
	ret = madvise(ptr, HPAGE_SIZE, MADV_HUGEPAGE);
	dprintf1("MADV_HUGEPAGE ret: %d\n", ret);
	ret = madvise(ptr, HPAGE_SIZE, MADV_WILLNEED);
	dprintf1("MADV_WILLNEED ret: %d\n", ret);
	memset(ptr, 0, HPAGE_SIZE);

	dprintf1("mmap()'d thp for pkey %d @ %p\n", pkey, ptr);
	return ptr;
}

int hugetlb_setup_ok;
#define GET_NR_HUGE_PAGES 10
void setup_hugetlbfs(void)
{
	int err;
	int fd;
	int validated_nr_pages;
	int i;
	char buf[] = "123";

	if (geteuid() != 0) {
		fprintf(stderr, "WARNING: not run as root, can not do hugetlb test\n");
		return;
	}

	cat_into_file(__stringify(GET_NR_HUGE_PAGES), "/proc/sys/vm/nr_hugepages");

	/*
	 * Now go make sure that we got the pages and that they
	 * are 2M pages.  Someone might have made 1G the default.
	 */
	fd = open("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages", O_RDONLY);
	if (fd < 0) {
		perror("opening sysfs 2M hugetlb config");
		return;
	}

	/* -1 to guarantee leaving the trailing \0 */
	err = read(fd, buf, sizeof(buf)-1);
	close(fd);
	if (err <= 0) {
		perror("reading sysfs 2M hugetlb config");
		return;
	}

	if (atoi(buf) != GET_NR_HUGE_PAGES) {
		fprintf(stderr, "could not confirm 2M pages, got: '%s' expected %d\n",
			buf, GET_NR_HUGE_PAGES);
		return;
	}

	hugetlb_setup_ok = 1;
}

void *malloc_pkey_hugetlb(long size, int prot, u16 pkey)
{
	void *ptr;
	int flags = MAP_ANONYMOUS|MAP_PRIVATE|MAP_HUGETLB;

	if (!hugetlb_setup_ok)
		return PTR_ERR_ENOTSUP;

	dprintf1("doing %s(%ld, %x, %x)\n", __func__, size, prot, pkey);
	size = ALIGN_UP(size, HPAGE_SIZE * 2);
	pkey_assert(pkey < NR_PKEYS);
	ptr = mmap(NULL, size, PROT_NONE, flags, -1, 0);
	pkey_assert(ptr != (void *)-1);
	mprotect_pkey(ptr, size, prot, pkey);

	record_pkey_malloc(ptr, size);

	dprintf1("mmap()'d hugetlbfs for pkey %d @ %p\n", pkey, ptr);
	return ptr;
}

void *malloc_pkey_mmap_dax(long size, int prot, u16 pkey)
{
	void *ptr;
	int fd;

	dprintf1("doing %s(size=%ld, prot=0x%x, pkey=%d)\n", __func__,
			size, prot, pkey);
	pkey_assert(pkey < NR_PKEYS);
	fd = open("/dax/foo", O_RDWR);
	pkey_assert(fd >= 0);

	ptr = mmap(0, size, prot, MAP_SHARED, fd, 0);
	pkey_assert(ptr != (void *)-1);

	mprotect_pkey(ptr, size, prot, pkey);

	record_pkey_malloc(ptr, size);

	dprintf1("mmap()'d for pkey %d @ %p\n", pkey, ptr);
	close(fd);
	return ptr;
}

void *(*pkey_malloc[])(long size, int prot, u16 pkey) = {

	malloc_pkey_with_mprotect,
	malloc_pkey_anon_huge,
	malloc_pkey_hugetlb
/* can not do direct with the pkey_mprotect() API:
	malloc_pkey_mmap_direct,
	malloc_pkey_mmap_dax,
*/
};

void *malloc_pkey(long size, int prot, u16 pkey)
{
	void *ret;
	static int malloc_type;
	int nr_malloc_types = ARRAY_SIZE(pkey_malloc);

	pkey_assert(pkey < NR_PKEYS);

	while (1) {
		pkey_assert(malloc_type < nr_malloc_types);

		ret = pkey_malloc[malloc_type](size, prot, pkey);
		pkey_assert(ret != (void *)-1);

		malloc_type++;
		if (malloc_type >= nr_malloc_types)
			malloc_type = (random()%nr_malloc_types);

		/* try again if the malloc_type we tried is unsupported */
		if (ret == PTR_ERR_ENOTSUP)
			continue;

		break;
	}

	dprintf3("%s(%ld, prot=%x, pkey=%x) returning: %p\n", __func__,
			size, prot, pkey, ret);
	return ret;
}

int last_pkru_faults;
void expected_pk_fault(int pkey)
{
	dprintf2("%s(): last_pkru_faults: %d pkru_faults: %d\n",
			__func__, last_pkru_faults, pkru_faults);
	dprintf2("%s(%d): last_si_pkey: %d\n", __func__, pkey, last_si_pkey);
	pkey_assert(last_pkru_faults + 1 == pkru_faults);
	pkey_assert(last_si_pkey == pkey);
	/*
	 * The signal handler shold have cleared out PKRU to let the
	 * test program continue.  We now have to restore it.
	 */
	if (__rdpkru() != 0)
		pkey_assert(0);

	__wrpkru(shadow_pkru);
	dprintf1("%s() set PKRU=%x to restore state after signal nuked it\n",
			__func__, shadow_pkru);
	last_pkru_faults = pkru_faults;
	last_si_pkey = -1;
}

void do_not_expect_pk_fault(void)
{
	pkey_assert(last_pkru_faults == pkru_faults);
}

int test_fds[10] = { -1 };
int nr_test_fds;
void __save_test_fd(int fd)
{
	pkey_assert(fd >= 0);
	pkey_assert(nr_test_fds < ARRAY_SIZE(test_fds));
	test_fds[nr_test_fds] = fd;
	nr_test_fds++;
}

int get_test_read_fd(void)
{
	int test_fd = open("/etc/passwd", O_RDONLY);
	__save_test_fd(test_fd);
	return test_fd;
}

void close_test_fds(void)
{
	int i;

	for (i = 0; i < nr_test_fds; i++) {
		if (test_fds[i] < 0)
			continue;
		close(test_fds[i]);
		test_fds[i] = -1;
	}
	nr_test_fds = 0;
}

#define barrier() __asm__ __volatile__("": : :"memory")
__attribute__((noinline)) int read_ptr(int *ptr)
{
	/*
	 * Keep GCC from optimizing this away somehow
	 */
	barrier();
	return *ptr;
}

void test_read_of_write_disabled_region(int *ptr, u16 pkey)
{
	int ptr_contents;

	dprintf1("disabling write access to PKEY[1], doing read\n");
	pkey_write_deny(pkey);
	ptr_contents = read_ptr(ptr);
	dprintf1("*ptr: %d\n", ptr_contents);
	dprintf1("\n");
}
void test_read_of_access_disabled_region(int *ptr, u16 pkey)
{
	int ptr_contents;

	dprintf1("disabling access to PKEY[%02d], doing read @ %p\n", pkey, ptr);
	rdpkru();
	pkey_access_deny(pkey);
	ptr_contents = read_ptr(ptr);
	dprintf1("*ptr: %d\n", ptr_contents);
	expected_pk_fault(pkey);
}
void test_write_of_write_disabled_region(int *ptr, u16 pkey)
{
	dprintf1("disabling write access to PKEY[%02d], doing write\n", pkey);
	pkey_write_deny(pkey);
	*ptr = __LINE__;
	expected_pk_fault(pkey);
}
void test_write_of_access_disabled_region(int *ptr, u16 pkey)
{
	dprintf1("disabling access to PKEY[%02d], doing write\n", pkey);
	pkey_access_deny(pkey);
	*ptr = __LINE__;
	expected_pk_fault(pkey);
}
void test_kernel_write_of_access_disabled_region(int *ptr, u16 pkey)
{
	int ret;
	int test_fd = get_test_read_fd();

	dprintf1("disabling access to PKEY[%02d], "
		 "having kernel read() to buffer\n", pkey);
	pkey_access_deny(pkey);
	ret = read(test_fd, ptr, 1);
	dprintf1("read ret: %d\n", ret);
	pkey_assert(ret);
}
void test_kernel_write_of_write_disabled_region(int *ptr, u16 pkey)
{
	int ret;
	int test_fd = get_test_read_fd();

	pkey_write_deny(pkey);
	ret = read(test_fd, ptr, 100);
	dprintf1("read ret: %d\n", ret);
	if (ret < 0 && (DEBUG_LEVEL > 0))
		perror("verbose read result (OK for this to be bad)");
	pkey_assert(ret);
}

void test_kernel_gup_of_access_disabled_region(int *ptr, u16 pkey)
{
	int pipe_ret, vmsplice_ret;
	struct iovec iov;
	int pipe_fds[2];

	pipe_ret = pipe(pipe_fds);

	pkey_assert(pipe_ret == 0);
	dprintf1("disabling access to PKEY[%02d], "
		 "having kernel vmsplice from buffer\n", pkey);
	pkey_access_deny(pkey);
	iov.iov_base = ptr;
	iov.iov_len = PAGE_SIZE;
	vmsplice_ret = vmsplice(pipe_fds[1], &iov, 1, SPLICE_F_GIFT);
	dprintf1("vmsplice() ret: %d\n", vmsplice_ret);
	pkey_assert(vmsplice_ret == -1);

	close(pipe_fds[0]);
	close(pipe_fds[1]);
}

void test_kernel_gup_write_to_write_disabled_region(int *ptr, u16 pkey)
{
	int ignored = 0xdada;
	int futex_ret;
	int some_int = __LINE__;

	dprintf1("disabling write to PKEY[%02d], "
		 "doing futex gunk in buffer\n", pkey);
	*ptr = some_int;
	pkey_write_deny(pkey);
	futex_ret = syscall(SYS_futex, ptr, FUTEX_WAIT, some_int-1, NULL,
			&ignored, ignored);
	if (DEBUG_LEVEL > 0)
		perror("futex");
	dprintf1("futex() ret: %d\n", futex_ret);
}

/* Assumes that all pkeys other than 'pkey' are unallocated */
void test_pkey_syscalls_on_non_allocated_pkey(int *ptr, u16 pkey)
{
	int err;
	int i;

	/* Note: 0 is the default pkey, so don't mess with it */
	for (i = 1; i < NR_PKEYS; i++) {
		if (pkey == i)
			continue;

		dprintf1("trying get/set/free to non-allocated pkey: %2d\n", i);
		err = sys_pkey_free(i);
		pkey_assert(err);

		/* not enforced when pkey_get() is not a syscall
		err = pkey_get(i, 0);
		pkey_assert(err < 0);
		*/

		err = sys_pkey_free(i);
		pkey_assert(err);

		err = sys_mprotect_pkey(ptr, PAGE_SIZE, PROT_READ, i);
		pkey_assert(err);
	}
}

/* Assumes that all pkeys other than 'pkey' are unallocated */
void test_pkey_syscalls_bad_args(int *ptr, u16 pkey)
{
	int err;
	int bad_flag = (PKEY_DISABLE_ACCESS | PKEY_DISABLE_WRITE) + 1;
	int bad_pkey = NR_PKEYS+99;

	/* not enforced when pkey_get() is not a syscall
	err = pkey_get(bad_pkey, bad_flag);
	pkey_assert(err < 0);
	*/

	/* pass a known-invalid pkey in: */
	err = sys_mprotect_pkey(ptr, PAGE_SIZE, PROT_READ, bad_pkey);
	pkey_assert(err);
}

void become_child(void)
{
	pid_t forkret;

	forkret = fork();
	pkey_assert(forkret >= 0);
	dprintf3("[%d] fork() ret: %d\n", getpid(), forkret);

	if (!forkret) {
		/* in the child */
		return;
	}
	exit(0);
}

/* Assumes that all pkeys other than 'pkey' are unallocated */
void test_pkey_alloc_exhaust(int *ptr, u16 pkey)
{
	unsigned long flags;
	unsigned long init_val;
	int err;
	int allocated_pkeys[NR_PKEYS] = {0};
	int nr_allocated_pkeys = 0;
	int i;

	for (i = 0; i < NR_PKEYS*3; i++) {
		int new_pkey;
		dprintf1("%s() alloc loop: %d\n", __func__, i);
		new_pkey = alloc_pkey();
		dprintf4("%s()::%d, err: %d pkru: 0x%x shadow: 0x%x\n", __func__,
				__LINE__, err, __rdpkru(), shadow_pkru);
		rdpkru(); /* for shadow checking */
		dprintf2("%s() errno: %d ENOSPC: %d\n", __func__, errno, ENOSPC);
		if ((new_pkey == -1) && (errno == ENOSPC)) {
			dprintf2("%s() failed to allocate pkey after %d tries\n",
				__func__, nr_allocated_pkeys);
		} else {
			/*
			 * Ensure the number of successes never
			 * exceeds the number of keys supported
			 * in the hardware.
			 */
			pkey_assert(nr_allocated_pkeys < NR_PKEYS);
			allocated_pkeys[nr_allocated_pkeys++] = new_pkey;
		}

		/*
		 * Make sure that allocation state is properly
		 * preserved across fork().
		 */
		if (i == NR_PKEYS*2)
			become_child();
	}

	dprintf3("%s()::%d\n", __func__, __LINE__);

	/*
	 * There are 16 pkeys supported in hardware.  One is taken
	 * up for the default (0) and another can be taken up by
	 * an execute-only mapping.  Ensure that we can allocate
	 * at least 14 (16-2).
	 */
	pkey_assert(i >= NR_PKEYS-2);

	for (i = 0; i < nr_allocated_pkeys; i++) {
		err = sys_pkey_free(allocated_pkeys[i]);
		pkey_assert(!err);
		rdpkru(); /* for shadow checking */
	}
}

void test_ptrace_of_child(int *ptr, u16 pkey)
{
	__attribute__((__unused__)) int peek_result;
	pid_t child_pid;
	void *ignored = 0;
	long ret;
	int status;
	/*
	 * This is the "control" for our little expermient.  Make sure
	 * we can always access it when ptracing.
	 */
	int *plain_ptr_unaligned = malloc(HPAGE_SIZE);
	int *plain_ptr = ALIGN_PTR_UP(plain_ptr_unaligned, PAGE_SIZE);

	/*
	 * Fork a child which is an exact copy of this process, of course.
	 * That means we can do all of our tests via ptrace() and then plain
	 * memory access and ensure they work differently.
	 */
	child_pid = fork_lazy_child();
	dprintf1("[%d] child pid: %d\n", getpid(), child_pid);

	ret = ptrace(PTRACE_ATTACH, child_pid, ignored, ignored);
	if (ret)
		perror("attach");
	dprintf1("[%d] attach ret: %ld %d\n", getpid(), ret, __LINE__);
	pkey_assert(ret != -1);
	ret = waitpid(child_pid, &status, WUNTRACED);
	if ((ret != child_pid) || !(WIFSTOPPED(status))) {
		fprintf(stderr, "weird waitpid result %ld stat %x\n",
				ret, status);
		pkey_assert(0);
	}
	dprintf2("waitpid ret: %ld\n", ret);
	dprintf2("waitpid status: %d\n", status);

	pkey_access_deny(pkey);
	pkey_write_deny(pkey);

	/* Write access, untested for now:
	ret = ptrace(PTRACE_POKEDATA, child_pid, peek_at, data);
	pkey_assert(ret != -1);
	dprintf1("poke at %p: %ld\n", peek_at, ret);
	*/

	/*
	 * Try to access the pkey-protected "ptr" via ptrace:
	 */
	ret = ptrace(PTRACE_PEEKDATA, child_pid, ptr, ignored);
	/* expect it to work, without an error: */
	pkey_assert(ret != -1);
	/* Now access from the current task, and expect an exception: */
	peek_result = read_ptr(ptr);
	expected_pk_fault(pkey);

	/*
	 * Try to access the NON-pkey-protected "plain_ptr" via ptrace:
	 */
	ret = ptrace(PTRACE_PEEKDATA, child_pid, plain_ptr, ignored);
	/* expect it to work, without an error: */
	pkey_assert(ret != -1);
	/* Now access from the current task, and expect NO exception: */
	peek_result = read_ptr(plain_ptr);
	do_not_expect_pk_fault();

	ret = ptrace(PTRACE_DETACH, child_pid, ignored, 0);
	pkey_assert(ret != -1);

	ret = kill(child_pid, SIGKILL);
	pkey_assert(ret != -1);

	wait(&status);

	free(plain_ptr_unaligned);
}

void test_executing_on_unreadable_memory(int *ptr, u16 pkey)
{
	void *p1;
	int scratch;
	int ptr_contents;
	int ret;

	p1 = ALIGN_PTR_UP(&lots_o_noops_around_write, PAGE_SIZE);
	dprintf3("&lots_o_noops: %p\n", &lots_o_noops_around_write);
	/* lots_o_noops_around_write should be page-aligned already */
	assert(p1 == &lots_o_noops_around_write);

	/* Point 'p1' at the *second* page of the function: */
	p1 += PAGE_SIZE;

	madvise(p1, PAGE_SIZE, MADV_DONTNEED);
	lots_o_noops_around_write(&scratch);
	ptr_contents = read_ptr(p1);
	dprintf2("ptr (%p) contents@%d: %x\n", p1, __LINE__, ptr_contents);

	ret = mprotect_pkey(p1, PAGE_SIZE, PROT_EXEC, (u64)pkey);
	pkey_assert(!ret);
	pkey_access_deny(pkey);

	dprintf2("pkru: %x\n", rdpkru());

	/*
	 * Make sure this is an *instruction* fault
	 */
	madvise(p1, PAGE_SIZE, MADV_DONTNEED);
	lots_o_noops_around_write(&scratch);
	do_not_expect_pk_fault();
	ptr_contents = read_ptr(p1);
	dprintf2("ptr (%p) contents@%d: %x\n", p1, __LINE__, ptr_contents);
	expected_pk_fault(pkey);
}

void test_mprotect_pkey_on_unsupported_cpu(int *ptr, u16 pkey)
{
	int size = PAGE_SIZE;
	int sret;

	if (cpu_has_pku()) {
		dprintf1("SKIP: %s: no CPU support\n", __func__);
		return;
	}

	sret = syscall(SYS_mprotect_key, ptr, size, PROT_READ, pkey);
	pkey_assert(sret < 0);
}

void (*pkey_tests[])(int *ptr, u16 pkey) = {
	test_read_of_write_disabled_region,
	test_read_of_access_disabled_region,
	test_write_of_write_disabled_region,
	test_write_of_access_disabled_region,
	test_kernel_write_of_access_disabled_region,
	test_kernel_write_of_write_disabled_region,
	test_kernel_gup_of_access_disabled_region,
	test_kernel_gup_write_to_write_disabled_region,
	test_executing_on_unreadable_memory,
	test_ptrace_of_child,
	test_pkey_syscalls_on_non_allocated_pkey,
	test_pkey_syscalls_bad_args,
	test_pkey_alloc_exhaust,
};

void run_tests_once(void)
{
	int *ptr;
	int prot = PROT_READ|PROT_WRITE;

	for (test_nr = 0; test_nr < ARRAY_SIZE(pkey_tests); test_nr++) {
		int pkey;
		int orig_pkru_faults = pkru_faults;

		dprintf1("======================\n");
		dprintf1("test %d preparing...\n", test_nr);

		tracing_on();
		pkey = alloc_random_pkey();
		dprintf1("test %d starting with pkey: %d\n", test_nr, pkey);
		ptr = malloc_pkey(PAGE_SIZE, prot, pkey);
		dprintf1("test %d starting...\n", test_nr);
		pkey_tests[test_nr](ptr, pkey);
		dprintf1("freeing test memory: %p\n", ptr);
		free_pkey_malloc(ptr);
		sys_pkey_free(pkey);

		dprintf1("pkru_faults: %d\n", pkru_faults);
		dprintf1("orig_pkru_faults: %d\n", orig_pkru_faults);

		tracing_off();
		close_test_fds();

		printf("test %2d PASSED (itertation %d)\n", test_nr, iteration_nr);
		dprintf1("======================\n\n");
	}
	iteration_nr++;
}

void pkey_setup_shadow(void)
{
	shadow_pkru = __rdpkru();
}

int main(void)
{
	int nr_iterations = 22;

	setup_handlers();

	printf("has pku: %d\n", cpu_has_pku());

	if (!cpu_has_pku()) {
		int size = PAGE_SIZE;
		int *ptr;

		printf("running PKEY tests for unsupported CPU/OS\n");

		ptr  = mmap(NULL, size, PROT_NONE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
		assert(ptr != (void *)-1);
		test_mprotect_pkey_on_unsupported_cpu(ptr, 1);
		exit(0);
	}

	pkey_setup_shadow();
	printf("startup pkru: %x\n", rdpkru());
	setup_hugetlbfs();

	while (nr_iterations-- > 0)
		run_tests_once();

	printf("done (all tests OK)\n");
	return 0;
}
