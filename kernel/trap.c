#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"  // for struct sleeplock
#include "fs.h"         // for struct inode, NDIRECT, readi, ilock, iunlock
#include "file.h"       // for struct file (uses sleeplock, inode)
#include "fcntl.h"      // for PROT_READ, PROT_WRITE
#include "proc.h"

struct spinlock tickslock;
uint ticks;

// -----------------------------------------------------------------------------
// Shared physical pages for mmap() mappings
// Keyed by (struct file *f, file_page_number).
// This lets multiple processes share physical pages when they mmap the same file
// (e.g., parent/child after fork), which is what we need for shared memory
// like the Pong game.
//
// NOTE: This still uses kalloc() pages, not the buffer cache. That means
// there's still "double caching" versus the fs buffer cache, but we now
// share pages across processes and use reference counts.
// -----------------------------------------------------------------------------

#define MMAP_MAX_PAGES 1024

struct mmap_phys_page {
  struct file *f;      // which file this page comes from
  uint64 file_page;    // file offset / PGSIZE
  char   *mem;         // physical page (kalloc'ed)
  int     refcnt;      // number of PTEs pointing at this page
  int     in_use;
};

static struct mmap_phys_page mmap_pages[MMAP_MAX_PAGES];
static struct spinlock mmap_pages_lock;

// Called once at boot from procinit().
void
mmap_phys_init(void)
{
  initlock(&mmap_pages_lock, "mmap_pages");
  for (int i = 0; i < MMAP_MAX_PAGES; i++) {
    mmap_pages[i].in_use = 0;
    mmap_pages[i].f = 0;
    mmap_pages[i].file_page = 0;
    mmap_pages[i].mem = 0;
    mmap_pages[i].refcnt = 0;
  }
}

// Internal helper: find entry by (file, file_page)
static struct mmap_phys_page *
mmap_find_locked(struct file *f, uint64 file_page)
{
  for (int i = 0; i < MMAP_MAX_PAGES; i++) {
    if (mmap_pages[i].in_use &&
        mmap_pages[i].f == f &&
        mmap_pages[i].file_page == file_page) {
      return &mmap_pages[i];
    }
  }
  return 0;
}

// Internal helper: find entry by its mem pointer.
static struct mmap_phys_page *
mmap_find_by_mem_locked(void *mem)
{
  for (int i = 0; i < MMAP_MAX_PAGES; i++) {
    if (mmap_pages[i].in_use &&
        mmap_pages[i].mem == (char *)mem) {
      return &mmap_pages[i];
    }
  }
  return 0;
}

// Get a shared page for (f, file_off).
// - file_off must be page aligned.
// - If a page already exists, bump refcnt and return it.
// - Otherwise, allocate + read from the file, insert into table, refcnt=1.
char *
mmap_get_shared_page(struct file *f, uint64 file_off)
{
  if (file_off % PGSIZE != 0) {
    // we only support page-aligned file offsets for now
    return 0;
  }
  uint64 file_page = file_off / PGSIZE;

  // First try to find an existing page.
  acquire(&mmap_pages_lock);
  struct mmap_phys_page *e = mmap_find_locked(f, file_page);
  if (e) {
    e->refcnt++;
    char *mem = e->mem;
    release(&mmap_pages_lock);
    return mem;
  }

  // Not found — we'll have to allocate and read.
  // Reserve a free slot first so that we don't race with another creator.
  struct mmap_phys_page *slot = 0;
  for (int i = 0; i < MMAP_MAX_PAGES; i++) {
    if (!mmap_pages[i].in_use) {
      slot = &mmap_pages[i];
      slot->in_use = 1;
      slot->f = f;
      slot->file_page = file_page;
      slot->mem = 0;
      slot->refcnt = 0;   // we'll set to 1 after filling
      break;
    }
  }
  release(&mmap_pages_lock);

  if (slot == 0) {
    // table full
    return 0;
  }

  // Allocate + read file data outside the lock (readi may sleep).
  char *mem = (char *)kalloc();
  if (mem == 0) {
    // roll back reservation
    acquire(&mmap_pages_lock);
    slot->in_use = 0;
    slot->f = 0;
    slot->file_page = 0;
    release(&mmap_pages_lock);
    return 0;
  }
  memset(mem, 0, PGSIZE);

  // Read PGSIZE bytes starting at file_off into mem.
  begin_op();
  ilock(f->ip);
  int n = readi(f->ip, 0, (uint64)mem, file_off, PGSIZE);
  iunlock(f->ip);
  end_op();

  if (n < 0) {
    kfree(mem);
    acquire(&mmap_pages_lock);
    slot->in_use = 0;
    slot->f = 0;
    slot->file_page = 0;
    release(&mmap_pages_lock);
    return 0;
  }

  // Now finalize / deduplicate under the lock.
  acquire(&mmap_pages_lock);
  // Maybe someone else created the same page while we were reading.
  e = mmap_find_locked(f, file_page);
  if (e && e != slot) {
    // Another entry won the race. Use it instead.
    e->refcnt++;
    char *winner_mem = e->mem;
    // Free our slot + page.
    slot->in_use = 0;
    slot->f = 0;
    slot->file_page = 0;
    release(&mmap_pages_lock);
    kfree(mem);
    return winner_mem;
  }

  // We're the first; fill the reserved slot.
  slot->mem = mem;
  slot->refcnt = 1;
  char *ret = mem;
  release(&mmap_pages_lock);
  return ret;
}

// Drop a reference to a shared page. If refcnt drops to 0, free it.
void
mmap_release_shared_page(void *mem)
{
  if (mem == 0)
    return;

  acquire(&mmap_pages_lock);
  struct mmap_phys_page *e = mmap_find_by_mem_locked(mem);
  if (e == 0) {
    // Not one of our tracked mmap pages; just drop lock & kfree.
    release(&mmap_pages_lock);
    kfree(mem);
    return;
  }

  e->refcnt--;
  if (e->refcnt < 0) {
    // shouldn't happen
    e->refcnt = 0;
  }

  if (e->refcnt == 0) {
    e->in_use = 0;
    e->f = 0;
    e->file_page = 0;
    char *to_free = e->mem;
    e->mem = 0;
    release(&mmap_pages_lock);
    kfree(to_free);
    return;
  }

  release(&mmap_pages_lock);
}

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

static const char *
scause_desc(uint64 stval);

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//-----------------------------------------------------------------------------------------------------
int
read_mapping(pagetable_t pagetable, uint64 va) {
  struct proc *p = myproc();

  // reject bogus addresses above MAXVA, but no compare with p->sz
  if (va >= MAXVA) {
    return -1;
  }

  // work with page-aligned va
  uint64 va_page = PGROUNDDOWN(va);

  // find mapped_region that covers this va
  struct mapped_region *r = 0;
  for (int i = 0; i < 16; i++) {
    if (p->mapped_regions[i].in_use &&
        va_page >= p->mapped_regions[i].start_address &&
        va_page <  p->mapped_regions[i].end_address) {
      r = &p->mapped_regions[i];
      break;
    }
  }

  if (r == 0) {
    return -1;   // not in any mmap region
  }

  uint64 start = r->start_address;
  int prot = r->prot;
  struct file *pf = r->mapped_file;

  // compute file offset: mapping_offset + offset_within_mapping
  uint64 off_in_mapping = va_page - start;      // how far into mapping
  uint64 file_off = r->offset + off_in_mapping;

  // get (or create) a shared physical page for this file offset
  char *mem = mmap_get_shared_page(pf, file_off);
  if (mem == 0) {
    return -1;
  }

  // build PTE flags from prot
  int flags = PTE_U;
  if (prot & PROT_READ) {
    flags |= PTE_R;
  }
  if (prot & PROT_WRITE) {
    flags |= PTE_W;
  }

  if (mappages(pagetable, va_page, PGSIZE, (uint64)mem, flags) != 0) {
    // on failure, drop the reference we just took
    mmap_release_shared_page(mem);
    return -1;
  }

  return 0;
}

//-----------------------------------------------------------------------------------

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  } else if ((r_scause() & 0xff) == 13 || (r_scause() & 0xff) == 15) {  // TO HANDLE PAGE FAULT
      if (read_mapping(p->pagetable, r_stval()) < 0) {
        p->killed = 1;
      }
  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause %p (%s) pid=%d\n", r_scause(), scause_desc(r_scause()), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}

//-----------------------------------------------------------------------------------------------------

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // turn off interrupts, since we're switching
  // now from kerneltrap() to usertrap().
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(p->trap_va, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p (%s)\n", scause, scause_desc(scause));
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else {
      // the PLIC sends each device interrupt to every core,
      // which generates a lot of interrupts with irq==0.
    }

    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

static const char *
scause_desc(uint64 stval)
{
  static const char *intr_desc[16] = {
    [0] "user software interrupt",
    [1] "supervisor software interrupt",
    [2] "<reserved for future standard use>",
    [3] "<reserved for future standard use>",
    [4] "user timer interrupt",
    [5] "supervisor timer interrupt",
    [6] "<reserved for future standard use>",
    [7] "<reserved for future standard use>",
    [8] "user external interrupt",
    [9] "supervisor external interrupt",
    [10] "<reserved for future standard use>",
    [11] "<reserved for future standard use>",
    [12] "<reserved for future standard use>",
    [13] "<reserved for future standard use>",
    [14] "<reserved for future standard use>",
    [15] "<reserved for future standard use>",
  };
  static const char *nointr_desc[16] = {
    [0] "instruction address misaligned",
    [1] "instruction access fault",
    [2] "illegal instruction",
    [3] "breakpoint",
    [4] "load address misaligned",
    [5] "load access fault",
    [6] "store/AMO address misaligned",
    [7] "store/AMO access fault",
    [8] "environment call from U-mode",
    [9] "environment call from S-mode",
    [10] "<reserved for future standard use>",
    [11] "<reserved for future standard use>",
    [12] "instruction page fault",
    [13] "load page fault",
    [14] "<reserved for future standard use>",
    [15] "store/AMO page fault",
  };
  uint64 interrupt = stval & 0x8000000000000000L;
  uint64 code = stval & ~0x8000000000000000L;
  if (interrupt) {
    if (code < NELEM(intr_desc)) {
      return intr_desc[code];
    } else {
      return "<reserved for platform use>";
    }
  } else {
    if (code < NELEM(nointr_desc)) {
      return nointr_desc[code];
    } else if (code <= 23) {
      return "<reserved for future standard use>";
    } else if (code <= 31) {
      return "<reserved for custom use>";
    } else if (code <= 47) {
      return "<reserved for future standard use>";
    } else if (code <= 63) {
      return "<reserved for custom use>";
    } else {
      return "<reserved for future standard use>";
    }
  }
}
