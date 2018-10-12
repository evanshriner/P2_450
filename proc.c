#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "MLFQ.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

struct MLFQ mlfq;
int queuepointers[NQUEUE];

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  // stack pointer
  char *sp;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;
  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  // puts a newly created process at back of first queue
  // enqueue(&mlfq.queues[0], p);
  p->queue = 1;
  release(&ptable.lock);


  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  // kstack is processes stack size
  sp = p->kstack + KSTACKSIZE;

  // put the process pointer at the end of queue 1

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  // sets trap frame pointer to where the stack pointer is
  p->tf = (struct trapframe*)sp;
  
  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  
  p = allocproc();
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
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
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  
  sz = proc->sz;
  if(n > 0){
    if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  proc->sz = sz;
  switchuvm(proc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;

  // Allocate process.
  if((np = allocproc()) == 0)
    return -1;

  // Copy process state from p.
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = proc->sz;
  np->parent = proc;
  *np->tf = *proc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);

  safestrcpy(np->name, proc->name, sizeof(proc->name));
 
  pid = np->pid;

  // lock to force the compiler to emit the np->state write last.
  acquire(&ptable.lock);
  np->state = RUNNABLE;
  release(&ptable.lock);
  
  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *p;
  int fd;

  if(proc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd]){
      fileclose(proc->ofile[fd]);
      proc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(proc->cwd);
  end_op();
  proc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(proc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == proc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  proc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for zombie children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != proc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->state = UNUSED;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->queue = 0;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || proc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  static int boosttimer = 12;
  //struct queue *cqueue;
  //int i;

  for(;;){
    //i = -1;
    // Enable interrupts on this processor.
    sti();
    //cprintf("got to beginning of scheduler\n");
    // Loop over queues looking for process to run.
    acquire(&ptable.lock);

    // MLFQ check each queue for processes
    for (int j = 1; j <= NQUEUE; j++) {

      mlfq.timeup = j;
      int didRun = 0;
      int consumed = 0;

      // look for the next process that is in this queue, in the ptable
      for (int k = 0; k < NPROC; k++) {

        // start from the queue pointer
        // this starts from the front of queue j, and checks our all the processes after and everythuing before
        p = &ptable.proc[(k + queuepointers[j]) % NPROC];

        // is this process in Qj?
        if(p->queue == j && p->state == RUNNABLE) {
          
          // set the queuepointer for Qj to k
          queuepointers[j] = k+1;
          if (queuepointers[j] >= NPROC) {
            queuepointers[j] = 0;
          }

          didRun = 1;

          // while time remaining is not equal to 0
          while (mlfq.timeup) {
            cprintf("mlfq.timeup: %d\n", mlfq.timeup);
            proc = p;
            switchuvm(p);
            p->state = RUNNING;
            //cprintf("about to switch\n");

            // run process until interrupt or complete
            swtch(&cpu->scheduler, proc->context);
            switchkvm();

            //cprintf("process %s interrupted by timer interrupt \n", p->name, p->pid, i + 1);

            --boosttimer;
            ++consumed;
            if (!boosttimer){
              cprintf("boosting all processes to q1\n");
              boost();
              boosttimer = 12; // reset boost timer to 12 interrupts / 120ms
            }
            // if process comes back as not runnable
            // cprintf("processes current state: %d \n", p->state);
            if (p->state != RUNNABLE){
                // if zombie or on i/o
                break;
            }
            --mlfq.timeup;
          }

          // process has used its time slice,
          if (consumed != j) {
              cprintf("process %s %d went to sleep and only consumed %dms in Q%d \n", p->name, p->pid, consumed * 10, j); // multiply by 10 for 10ms
          } else {
              cprintf("process %s %d has consumed the full %dms in Q%d \n", p->name, p->pid, consumed * 10, j); // multiply by 10 for 10ms
          }

          // time is up, set to next q if still not zombie and not in q6
          if (p->state != ZOMBIE) {
            if (j != NQUEUE) {
                p->queue++;
            }
          }

          // Process is done running for now.
          // It should have changed its p->state before coming back.
          proc = 0;
          break; // we ran a process, start the whole scheduler from the top

        }
      }
      if (didRun) break; // if this wasnt here, would continue to search through next queue in MLFQ for next runnable process.
    }

    /*for (cqueue = mlfq.queues; cqueue < &mlfq.queues[NQUEUE]; cqueue++) {
      ++i;
      // cprintf("queue size: %d\n", cqueue->size);
      if (!cqueue->size) {// if queue does not have processes in it
          // cprintf("continuing past queue%d", i + 1);
          continue;
      }

      // has found a queue with processes with it. get process
      p = peek(cqueue);
      //cprintf("process at front: %s\n", p->name);
      // set time slice value
      mlfq.timeup = cqueue->quantum;

      while (mlfq.timeup) {
          // if time remaining is not equal to 0
          proc = p;
          switchuvm(p);
          p->state = RUNNING;
          //cprintf("about to switch\n");
          // run process until interrupt or complete
          swtch(&cpu->scheduler, proc->context);
          switchkvm();

          cprintf("process %s interrupted by timer interrupt \n", p->name, p->pid, i + 1);
          // if process comes back as not runnable
          cprintf("processes current state: %d \n", p->state);
          if (p->state != RUNNABLE){

              // check if process is done running/complete (ZOMBIE)
              if (p->state == ZOMBIE) {
                  dequeue(cqueue);
              }
              // what if process is on i/o thread and not able to make use of cpu even though it is scheduled?
              // should we just continue to run it anyway? this takes it out and puts it
              // in next queue

              // if zombie or on i/o
              break;
          }
          --mlfq.timeup;
      }
      // process has used its time slice,
      cprintf("process %s %d has consumed %d ms in Q%d \n", p->name, p->pid, cqueue->quantum, i + 1);

        // time is up, swap if still not zombie and not in q6
        if (p->state != ZOMBIE) {

          if (i != (NQUEUE - 1)) {
              swapqueue(&mlfq.queues[i], &mlfq.queues[i + 1], p);
          } else { // in q6, send to back of the queue
              swapqueue(&mlfq.queues[i], &mlfq.queues[i], p);
          }
        }

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        proc = 0;
        break; // if this wasnt here, would continue to search through next queue in MLFQ for next runnable process.
    }*/
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state.
void
sched(void)
{
  int intena;

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(cpu->ncli != 1)
    panic("sched locks");
  // this shows that the running processe's will always be a different process than running when it goes into the queue.
  if(proc->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = cpu->intena;
  // HOLY SHIT!! so this is where it switches to what is pointing at the scheduler (the line of code where you left off), then the scheduler
  // sets up the next process.
  // sched is called from every process API that haults the process (wait, yield,) and timer interrupt
  swtch(&proc->context, cpu->scheduler);
  cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  proc->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
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
void
sleep(void *chan, struct spinlock *lk)
{
  if(proc == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  proc->chan = chan;
  proc->state = SLEEPING;
  sched();

  // Tidy up.
  proc->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];
  
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

void
mlfqinit(void)
{
// loop thru queues and set the quantum time
  for (int i=0; i < NQUEUE; i++) {
    mlfq.queues[i].quantum = i+1 * 10;
    mlfq.queues[i].size = 0;
    mlfq.queues[i].front = -1;
    mlfq.queues[i].back = -1;
  }
}

// adds the proc pointer to back of the circular queue
// updates front and back queue pointers
// updates queue size
void
enqueue(struct queue *q, struct proc *p)
{
  // add process to the back of the queue
  // if the back is 63, move back to 0

  // nothing in the queue, set both to 0 initially
  if (q->front == -1 && q->back == -1) {
    q->front = 0;
    q->back = 0;
  } else {
    q->back++;
  }

  if (q->back >= 64)
    q->back = 0;

  q->q[q->back] = p;
  q->size++;
  cprintf("%s has been added to back of Q%d\n", p->name, q->quantum / 10);
}


// sets the process back to init (UNUSED, etc)
// sets the queue slot designated by pos to null
// updates front and back queue pointers (queue->front, queue->back)
void
dequeue(struct queue *q)
{
  // increment front
  cprintf("dequeue called\n");
  q->front++;
  q->size--;
  // if front is greater than back, queue is empty
  /* TODO: this is not the case. ex:
    front is at 63, back is at 7. will overwrite
    those 7 slots that were added by enqueue*/
 // if (q->front > q->back) {
 //   q->back = -1;
 //   q->front = -1;
 // }

  // check for out of bounds
  if(q->front >= 64)
    q->front = 0;
  // TODO: back will never be changed in this function, so it will never need to be checked
  if (q->back >= 64)
    q->back = 0;
}

// dequeues process at position
// enqueues process in back of next queue
void
swapqueue(struct queue *preq, struct queue *postq, struct proc *p)
{
  enqueue(postq, p);
  dequeue(preq);
}

// takes mlfq
// loops through all queues and moves them up to front
// as it finds them
// note: you will not need to check if they can all fit in the
// first queue because there is a max of 64 processes in the entire
void
boost(){
  struct proc *p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if (p->queue > 1){
      p->queue = 1;
    }
  }
}

// returns the next process in the queue
struct proc*
peek(struct queue *q) {
  return q->q[q->front];
}

// return amount of processess in the queue
int
getsize(struct queue *q) {
  if (q->front == -1 && q->back == -1)
    return 0;

  if (q->front < q->back) {
    return q->back - q->front + 1;
  } else {
    return 65 - q->front + q->back;
  }
}

