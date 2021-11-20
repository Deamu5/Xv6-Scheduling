#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "procstat.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

int nextpid = 1;
static struct proc *initproc;

extern void trapret(void);
extern void forkret(void);
static void wakeup1(void *chan);
void addToQueue(struct pqueue *pq, struct proc *p);

int queue0[500], queue1[500],queue2[500];
char* name0[500],*name1[500],*name2[500];
char* state0[500],*state1[500],*state2[500];


void pinit(void){
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void){
  int apicid, i;
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
    for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void){
  char *sp;
  struct proc *p;
  
  acquire(&ptable.lock);
  p = ptable.proc;
  for(; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->priority=60;
  addToQueue(&q0,p);
  p->ctime=ticks;
//  cprintf("pid:%d starttime:%d",p->pid,p->ctime);
  p->current_queue=p->num_run=p->wtime=p->etime=p->rtime=0;
  int j=0;
  while(j<5){
    p->ticks[j]=0;
  j++;
  }
  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp =sp-sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp =sp- 4;
  *(uint*)sp = (uint)trapret;

  sp =sp- sizeof *p->context;
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
//   TOTAL_TICKS = 0;

  q0.numproc=q0.priority=q0.end = 0;
  q0.ticks = 1;
  
  q1.numproc=q1.end = 0;
  q1.priority = 1;
  q1.ticks = 2;
  
  q2.numproc=q2.end = 0;
  q2.priority = 2;
  q2.ticks = 8;
  
  q2.numproc=q3.end = 0;
  q3.priority = 3;
  q3.ticks = 8;
  
  q4.numproc=q4.end = 0;
  q4.priority = 4;
  q4.ticks = 8;
  
p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  p->ctime=ticks;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->ss = p->tf->ds;
  p->tf->es = p->tf->ds;

  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

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
int
growproc(int n){
  uint sz;
  struct proc *curproc = myproc();
  sz = curproc->sz;
  if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
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
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->state = UNUSED;
    np->kstack = 0;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
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
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
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
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;

  curproc->etime=ticks;
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
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
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
        p->ctime=0;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
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

int waitx(int* wtime,int* rtime)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    p = ptable.proc;
    for( ; p < &ptable.proc[NPROC]; p++){
      if((*p).parent != curproc)
        continue;
      havekids = 1;
      if((*p).state == ZOMBIE){
        // Found one.
        // Added time field update, else same from wait system call
        *wtime=p->wtime;
        *rtime = p->rtime;
//        cprintf("pid:%d endtime:%d\n",p->pid,p->etime);
        pid = (*p).pid;
        kfree(p->kstack);
        (*p).kstack = 0;
        freevm(p->pgdir);
        (*p).parent = 0;
        (*p).pid = 0;
        (*p).killed = 0;
        (*p).name[0] = 0;
        (*p).ctime=0;
        (*p).state = UNUSED;

        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}


//Return highest non-empty priorty queue with ready process
struct pqueue *returnQueue(void){
  if (q0.numproc){ 
    int i=0;
    for (; i < q0.end; i++){
      if (q0.queue[i]->state == RUNNABLE){
        return &q0;
      }
    }
  }
  if (q1.numproc){
    int i = 0;
    for (; i < q1.end; i++){
      if (q1.queue[i]->state == RUNNABLE){
        return &q1;
      }
    }
  }

  if (q2.numproc){
    int i = 0;
    for (; i < q2.end; i++){
      if (q2.queue[i]->state == RUNNABLE){
        return &q2;
      }
    }
  }

  if (q3.numproc)
  {   int i=0;
    for (; i < q3.end; i++){
      if (q3.queue[i]->state == RUNNABLE){
        return &q3;
      }
    }
  }

  if (q4.numproc){
    int i=0;
    for (; i < q4.end; i++){
      if (q4.queue[i]->state == RUNNABLE){
        return &q4;
      }
    }
  }
  //cprintf("No runnable processes");
  return &q0;
}

//Adds process to end of specified priority queue
void addToQueue(struct pqueue *pq, struct proc *p){
//    cprintf("addqueue:%d",pq->priority);
  (*pq).queue[pq->end] = p;
  (*p).Ticks = 0;
  (*pq).numproc+=1;
  (*pq).end++;

  p->current_queue = pq->priority;
}

//Removes process from the start of specified priority queue
void removeFromQueue(struct pqueue *pq, struct proc *p){
  if ((*pq).queue[0] != p)
    panic("LOOK IN REMOVE FROM QUEUE FUNCTION");

  int firstFreeIndex = pq->end;
  int i=0;
  for (; i < firstFreeIndex; i++){
    (*pq).queue[i] = (*pq).queue[i + 1];
  }
  (*pq).end--;
  (*pq).numproc--;
}

//Downgrades process to a lower priority queue
void downgradeQueue(struct pqueue *pq, struct proc *p){
  if ((*pq).priority == 0){
    removeFromQueue(&q0, p);
    addToQueue(&q1, p);
  }
  else if ((*pq).priority == 1){
    removeFromQueue(&q1, p);
    addToQueue(&q2, p);
  }
  else if ((*pq).priority == 2){
    removeFromQueue(&q2, p);
    addToQueue(&q3, p);
  }
  else if ((*pq).priority == 3){
    removeFromQueue(&q3, p);
    addToQueue(&q4, p);
  }
}

//Updates queue, total_ticks and wait_time fields
//for all processes every time a tick occurs
void updatePstat(void){
  struct proc *p;
p = ptable.proc;
  for (; p < &ptable.proc[NPROC]; p++){
    if ((*p).state == RUNNABLE){
      //index 499 keeps getting overwritten
      //since TOTAL_TICKS never goes past NTICKS (i.e 500)
      // probably won't matter
//      p->queue[TOTAL_TICKS] = p->current_queue;
//      p->total_ticks++;
      if ((*p).current_queue == 4)
        (*p).wtime++;
    }
  }
}

//Returns highest priority RUNNABLE process
struct proc *returnProc(void){
  struct pqueue *pq = returnQueue();
//  cprintf("hello3");
  if ((*pq).numproc == 0)
  {
    //cprintf("In returnProc - no runnable process");
    return 0;
  }
int i=0;
  for (; i < pq->end; i++){
//    cprintf("hello5");
    if ((*pq).queue[i]->state == RUNNABLE){
      struct proc *p = pq->queue[i];
      if (i != 0){
        //Swap with first
        struct proc *temp = pq->queue[0];
        (*pq).queue[0] = p;
        (*pq).queue[i] = temp;
      }
      if(4==(*pq).priority){
      removeFromQueue(&q4,p);
      addToQueue(&q4,p);
      }
      return p;
    }
  }

  //cprintf("At end of returnProc");
  return 0;
}


void
scheduler(void){
  struct proc *p=0;
  struct cpu *c = mycpu();
  (*c).proc = 0;
  
  while(1)
  {
    // Enable interrupts on this processor.
    sti();
    int fl;
    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
        (*p).wtime=ticks-p->ctime-p->rtime;
        fl=0;
        if((*p).state != RUNNABLE)
                continue;
        #ifdef DEFAULT
            if(p->state != RUNNABLE)
                continue;
        #else
        #ifdef PRIORITY           
            struct proc* highp=0;
            struct proc* p1=0;
//            cprintf("tuosja\n");
            if(p->state!=RUNNABLE)
                continue;
            highp=p;
            
//            acquire(&ptable.lock);

            for(p1= ptable.proc; p1 < &ptable.proc[NPROC]; p1++)
            {
                if((p1->state==RUNNABLE)&&(highp->priority>p1->priority))
                    highp=p1;
            }

//            release(&ptable.lock);

            if(highp != 0)
                p = highp;
        #else
        #ifdef FCFS

            if(p->state != RUNNABLE)
              continue;
            struct proc *minp = p;
            struct proc *p1= 0;            
//            acquire(&ptable.lock);
            for(p1= ptable.proc; p1 < &ptable.proc[NPROC]; p1++){
                if(((*p1).state==RUNNABLE)&&(minp->ctime>p1->ctime))
                    minp=p1;
            }
//            release(&ptable.lock);

            // // ignore init and sh processes from FCFS
            // if(p->pid <= 2)
            // {
            //     minp=p;
            // }
            if(minp != 0 && (*minp).state == RUNNABLE)
                p = minp;    
        #else
        #ifdef MLFQ
            fl=1;
//            cprintf("hello hello1");
            //returnProc returns the highest priority RUNNABLE process
//            cprintf("hello2");
            if (p != returnProc())
                continue;
            p->num_run++;
//            cprintf("hello2");

            struct pqueue *queue = returnQueue();

             //printQueues();

            int queuepriority = (*p).current_queue;
//            cprintf("queue:%d\n",queuepriority);
            int count = 0;
//            cprintf("hello2");
            while ((*p).state == RUNNABLE && count < queue->ticks)
            {     
//                cprintf("yo");
             
            // Switch to chosen process.  It is the process's job
            // to release ptable.lock and then reacquire it
            // before jumping back to us.
          p->ticks[p->current_queue]++;
            c->proc = p;
            switchuvm(p);
            p->state = RUNNING;
            swtch(&(c->scheduler), p->context);
            switchkvm();
            // Process is done running for now.
            // It should have changed its p->state before coming back.
            c->proc = 0;
            //If state is not RUNNABLE here then means tick did not complete

            //Added to maintain info about state when process RETURNS
            //to scheduler
           
            count=count+1;
            p->Ticks++;

            //Update queue pstat var of each process
//              updatePstat();

            //Keep track of number of ticks, where each
            //tick is valid only if a process is being run
            //If no processes are being run then code below is not executed.
//            if (TOTAL_TICKS < 500)
//                  TOTAL_TICKS++;
          }

          //Handles case where process uses its time slice and is demoted
        if ((*p).state == RUNNABLE && (p->current_queue == 0 || p->current_queue == 1 || p->current_queue == 2 || p->current_queue == 3)){
            downgradeQueue(queue, p);
        } 

          //Handles case where process in Q2 exceeds 50 ticks
        else if ((*p).state == RUNNABLE && p->current_queue == 1 && p->Ticks >= 250){
            removeFromQueue(&q1, p);
            addToQueue(&q0, p);
            p->current_queue = 0;
        }
        else if ((*p).state == RUNNABLE && p->current_queue == 2 && p->Ticks >= 500){
            removeFromQueue(&q2, p);
            addToQueue(&q1, p);
            p->current_queue = 1;      

        }
        else if ((*p).state == RUNNABLE && p->current_queue == 3 && p->Ticks >= 1000)
        {
            removeFromQueue(&q3, p);
            addToQueue(&q2, p);
            p->current_queue = 2;

        }
        else if ((*p).state == RUNNABLE && p->current_queue == 4 && p->Ticks >= 2000){
            removeFromQueue(&q4, p);
            addToQueue(&q3, p);
            p->current_queue = 3;

        }
        //Handles case where process does NOT use its timeslice
        else if ((*p).state == SLEEPING){
            removeFromQueue(queue, p);
            addToQueue(queue, p);
        }
        cprintf("%d %d\n",p->pid,p->current_queue);
//        continue;
//    }
//    release(&ptable.lock);
//    continue;


        #endif    
        #endif
        #endif    
        #endif   
        if(fl==0&&p!=0){      
            (*c).proc = p;
            switchuvm(p);
            (*p).state = RUNNING;

            swtch(&(c->scheduler), p->context);
            switchkvm();

            // Process is done running for now.
            // It should have changed its p->state before coming back.
            (*c).proc = 0;
          }
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
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
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
  struct proc *p = myproc();
  
  if(p == 0)
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
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

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

//function for running ps command;
int runps(void)
{
    struct proc* p;
    sti();              
    acquire(&ptable.lock);
    p=ptable.proc;
    cprintf("name\tpid\tstate   \tpriority\tctime\trtime\twtime\n");
    cprintf("\n");
    for(;p<&ptable.proc[NPROC];p++){
        if(p->state==SLEEPING)
            cprintf("%s\t%d\tSLEEPING \t%d              %d \t %d \t %d\n",p->name,p->pid,p->priority,p->ctime,p->rtime,p->wtime);
        else if(p->state==RUNNABLE)
            cprintf("%s\t%d\tRUNNABLE \t%d              %d \t %d \t %d\n",p->name,p->pid,p->priority,p->ctime,p->rtime,p->wtime);
        else if(p->state==RUNNING)
            cprintf("%s\t%d\tRUNNING \t%d              %d \t %d \t %d\n",p->name,p->pid,p->priority,p->ctime,p->rtime,p->wtime);
      }
    cprintf("\n");
    release(&ptable.lock);

    return 22;
}

//function for changing priority of a process
int changepr(int pid,int priority)
{
    struct proc* p;
    acquire(&ptable.lock);
    int value=0;
    p=ptable.proc;
    for(;p<&ptable.proc[NPROC];p++){
        if(pid==(*p).pid){
            value=(*p).priority;
            (*p).priority=priority;
            break;
        }
    }
    release(&ptable.lock);
    return value;
}

int gpinfo(struct proc_stat* ps,int pid)
{
    int fg=0;
    struct proc* p;
    acquire(&ptable.lock);
    p=ptable.proc;
    for(;p<&ptable.proc[NPROC];p++){
        if((*p).pid==pid){
            fg=1;
            break;
        }
    }
    release(&ptable.lock);
    if(!fg)
        return 0;

    (*ps).pid=p->pid;
    (*ps).runtime=p->rtime;
//   cprintf("runtime1:%d\n",p->rtime);
//    cprintf("runtime2:%d\n",ps->runtime);
    (*ps).num_run=p->num_run;
    (*ps).current_queue=p->current_queue;
    int j=0;
    for(;j<5;j++)
        ps->ticks[j]=p->ticks[j];
    
    return 1;
}
void rtimecontroller(){
    struct proc *p=ptable.proc;
    for(;p<&ptable.proc[NPROC];p++){
        if((*p).state==RUNNING){
            p->rtime++;
        }
    }
}