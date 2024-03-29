#include "types.h"
#include "mmu.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "mmap.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"

struct
{
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int cpuid()
{
  return mycpu() - cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu *
mycpu(void)
{
  int apicid, i;

  if (readeflags() & FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i)
  {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc *
myproc(void)
{
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

// PAGEBREAK: 32
//  Look in the process table for an UNUSED proc.
//  If found, change state to EMBRYO and initialize
//  state required to run in the kernel.
//  Otherwise return 0.
static struct proc *
allocproc(void)
{
  // here we set up the proc structure
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if ((p->kstack = kalloc()) == 0)
  { // here we call kalloc which is going to return
    // it returns the first free page of memory
    // it returns a pointer to a VA
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE; // this is just incrementing the stack pointer to the top of the stack

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe *)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint *)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context *)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

// PAGEBREAK: 32
//  Set up first user process.
void userinit(void)
{
  // here is the userinit process and it is the first thing that happens
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc(); // 1. allocproc is called
  // alloc proc returns the process structure which includes the
  // p->kstack which is the kernal stack of the process

  initproc = p;
  if ((p->pgdir = setupkvm()) == 0) // 3. setupkvm is the next call to investigate
                                    //
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0; // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if (n > 0)
  {
    if ((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  else if (n < 0)
  {
    if ((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int fork(void)
{
  // now lets take a look at our new program we have created
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  

  // THIS NEXT SECTION OF CODE IS THE IMPLEMTATION OF MAPSHARED

  np->num_mappings = curproc->num_mappings; // copy the number of mappings

  // Set up the new page directory for the child
  if ((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0)
  {
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }

  // Copy and mark the parent's pages as COW
  for (i = 0; i < curproc->num_mappings; i++)
  {
    struct mem_mapping *map = &curproc->memoryMappings[i];

    // Check if mapping is private and should be COW
    if (map->flags & MAP_PRIVATE)
    {
     
      // go through the mappings for the current proc and make all pte's read only, then copy all the pte's to the child

      for (uint address = map->addr; address < map->addr + map->length; address += PGSIZE)
      {
        // Access the PTE for the parent.
        pte_t *pte = walkpgdir(curproc->pgdir, (void *)address, 0);
        if (pte && (*pte & PTE_P))
        {
          // Make the parent's page read-only and set the COW flag.
          *pte &= ~PTE_W;
          *pte |= PTE_COW;
          lcr3(V2P(curproc->pgdir)); // Flush the TLB to ensure the new PTE setting takes effect.

          // Now ensure the child has a PTE for the same address.
          pte_t *child_pte = walkpgdir(np->pgdir, (void *)address, 1); // Pass 1 to create the PTE if it does not exist.
          if (child_pte)
          {
            // Copy parent PTE to child PTE.
            *child_pte = *pte;
          }
          else
          {
            panic("Failed to allocate PTE for child.");
          }
        }
      }
    }
    np->memoryMappings[i] = *map;
  }
 
  //END SECTION

  // Copy process state from proc.
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for (i = 0; i < NOFILE; i++)
    if (curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if (curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for (fd = 0; fd < NOFILE; fd++)
  {
    if (curproc->ofile[fd])
    {
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->parent == curproc)
    {
      p->parent = initproc;
      if (p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }
  // clear out memory of
  memset(curproc->memoryMappings, 0, sizeof(struct mem_mapping) * 32);
  curproc->num_mappings = 0;

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->parent != curproc)
        continue;
      havekids = 1;
      if (p->state == ZOMBIE)
      {
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || curproc->killed)
    {
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock); // DOC: wait-sleep
  }
}

// PAGEBREAK: 42
//  Per-CPU process scheduler.
//  Each CPU calls scheduler() after setting itself up.
//  Scheduler never returns.  It loops, doing:
//   - choose a process to run
//   - swtch to start running that process
//   - eventually that process transfers control
//       via swtch back to the scheduler.
void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;

  for (;;)
  {
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&ptable.lock))
    panic("sched ptable.lock");
  if (mycpu()->ncli != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (readeflags() & FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  acquire(&ptable.lock); // DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first)
  {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if (p == 0)
    panic("sleep");

  if (lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if (lk != &ptable.lock)
  {                        // DOC: sleeplock0
    acquire(&ptable.lock); // DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if (lk != &ptable.lock)
  { // DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

// PAGEBREAK!
//  Wake up all processes sleeping on chan.
//  The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      p->killed = 1;
      // Wake process from sleep if necessary.
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

int count_children(struct proc *parent_proc)
{
  struct proc *p;
  int count = 0;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->parent == parent_proc)
    {
      count++;
    }
  }
  release(&ptable.lock);

  return count;
}

// PAGEBREAK: 36
//  Print a process listing to console.  For debugging.
//  Runs when user types ^P on console.
//  No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [EMBRYO] "embryo",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if (p->state == SLEEPING)
    {
      getcallerpcs((uint *)p->context->ebp + 2, pc);
      for (i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

// I decided to define our user level functions in proc.c as this is where almost everyting happens

int page_fault_handler(uint va)
{
  struct file *f = 0;
  struct inode *ip = 0;

  /* Case 1 - Lazy Allocation */
  // go throgh kalloc routine
  struct proc *currproc = myproc(); // get the current process

  pte_t *pte = walkpgdir(currproc->pgdir, (void *)va, 0);


  // Check if the page fault was due to a write on a COW page
  if (pte && (*pte & PTE_COW) && !(*pte & PTE_W))
  {
    // This is a COW fault, handle it
    char *mem = kalloc();
    if (mem == 0)
    {
      panic("Out of memory - COW page fault handler"); // Handle allocation failure
    }
    char *old_page = P2V(PTE_ADDR(*pte)); // Get the address of the old page
    memmove(mem, old_page, PGSIZE);       // Copy contents to the new page

    // Update PTE to point to the new page and make it writable
    *pte = V2P(mem) | PTE_FLAGS(*pte) | PTE_W;
    *pte &= ~PTE_COW; // Clear the COW flag

    lcr3(V2P(currproc->pgdir)); // Flush the TLB

    return 1; // COW fault handled successfully
  }

  // get the number of array of mappings
  int num_mappings = currproc->num_mappings;

  // I don't even care about the other mappings

  for (int i = 0; i < num_mappings; i++)
  {

    // now I need to get the specific mapping at I
    struct mem_mapping map = currproc->memoryMappings[i];

    if (va >= map.addr && va < PGROUNDUP(map.addr + map.length))
    {

      map.allocated = 1; // set the mapping to be allocated
      
      if (map.fd > 0)
      {
        f = currproc->ofile[map.fd];
        if (f == 0)
        {
          // Handle error: Invalid file descriptor
          panic("mapping failed 1");
        }
        ip = f->ip;
      }

      int file_backed = 0;
      char *mem = kalloc(); // Call THIS ONLY IF PGDIR FAILS and if it doesn't fail teh value returned by pdgir will be parent's pgdir
      if (mem == 0)
      {
        // Handle error: free any previously allocated pages
        panic("mapping failed 2"); // Allocation failed
      }

      // the first check we want to do is check to see if it is Map_grows up
      if ((map.flags & MAP_GROWSUP))
      {
        uint end_of_mapping = PGROUNDUP(map.addr + map.length) + PGSIZE; // this is going to get end of our mapping
        uint next_mapping_start = 0xFFFFFFFF;

        for (int j = 0; j < num_mappings; j++)
        {

          if (currproc->memoryMappings[j].addr >= end_of_mapping &&
              currproc->memoryMappings[j].addr < next_mapping_start)
          {
            next_mapping_start = currproc->memoryMappings[j].addr; // this sets the next mapping start to be in the right place

            break;
          }
        }
        if (PGSIZE >= next_mapping_start - end_of_mapping)
        {
          break;
        }

        currproc->memoryMappings[i].length += (int)PGSIZE;

        // if not mapped anonymous we need to increment the file size

      }

      if (!(map.flags & MAP_ANONYMOUS))
      { // this is the case where we are mapping from a file
        file_backed = 1;
        

        uint page_in_file = PGROUNDDOWN(va);                      // this is the page aligned
        int offset_into_file = (int)page_in_file - (int)map.addr; // this is were we want to grab the data in the file
        

        // now we need to read the contents of the file
        if (file_backed)
        {

          char buffer[PGSIZE]; // Create a buffer to hold the read data
          begin_op();
          ilock(ip);
          // if it is a guard page do not read
          readi(ip, buffer, offset_into_file, PGSIZE);
          iunlock(ip);
          end_op();

          memmove(mem, buffer, PGSIZE);
        }
      }
      if (!file_backed)
      {
        memset(mem, 0, PGSIZE); // zero out the page
      }

      if (mappages(currproc->pgdir, (char *)va, PGSIZE, V2P(mem), PTE_W | PTE_U) < 0)
      {
        kfree(mem);
      }
      else
      {
        // After successful mappages call
        walkpgdir(currproc->pgdir, (void *)va, 0);

      }

      file_backed = 0;
      return 1;
    }
  }

  cprintf("Segmentation Fault\n");
  return -1;
}