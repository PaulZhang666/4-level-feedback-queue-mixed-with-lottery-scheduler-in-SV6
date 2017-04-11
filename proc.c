#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "pstat.h"

struct proc* q0[64];
struct proc* q1[64];
struct proc* q2[64];
struct proc* q3[64];
int c0 =-1;
int c1=-1;
int c2=-1;
int c3=-1;
int clkPerPrio[4] ={1,2,4,8};
struct pstat pstat_var;

struct {
	struct spinlock lock;
	struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

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
static struct proc *
	allocproc (void)
{
	struct proc *p;
	char *sp;

	acquire (&ptable.lock);
	for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
		if (p->state == UNUSED)
			goto found;

	p->tickets = 1000/p->pid;
	p->priority = 0;
	p->clicks = 0;
	c0++;
	q0[c0] = p;
	pstat_var.inuse[p->pid] = 1;
	pstat_var.priority[p->pid] = p->priority;
	pstat_var.ticks[p->pid][0] = 0;
	pstat_var.ticks[p->pid][1] = 0;
	pstat_var.ticks[p->pid][2] = 0;
	pstat_var.ticks[p->pid][3] = 0;
	pstat_var.pid[p->pid] = p->pid;
	release(&ptable.lock);
	return 0;


found:
	p->state = EMBRYO;
	p->pid = nextpid++;
	p->tickets = 1000/nextpid;
	pstat_var.inuse[p->pid] = 1;
	p->priority = 0;
	p->clicks = 0;
	c0++;
	q0[c0] = p;
	pstat_var.priority[p->pid] = p->priority;
	pstat_var.ticks[p->pid][0] = 0;
	pstat_var.ticks[p->pid][1] = 0;
	pstat_var.ticks[p->pid][2] = 0;
	pstat_var.ticks[p->pid][3] = 0;
	pstat_var.pid[p->pid] = p->pid;
	release (&ptable.lock);

	// Allocate kernel stack if possible.
	if ((p->kstack = kalloc ()) == 0)
	{
		p->state = UNUSED;
		return 0;
	}
	sp = p->kstack + KSTACKSIZE;

	// Leave room for trap frame.
	sp -= sizeof *p->tf;
	p->tf = (struct trapframe *) sp;

	// Set up new context to start executing at forkret,
	// which returns to trapret.
	sp -= 4;
	*(uint *) sp = (uint) trapret;

	sp -= sizeof *p->context;
	p->context = (struct context *) sp;
	memset (p->context, 0, sizeof *p->context);
	p->context->eip = (uint) forkret;

	return p;
}
//PAGEBREAK: 32
// Set up first user process.
void
	userinit (void)
{
	struct proc *p;
	extern char _binary_initcode_start[], _binary_initcode_size[];

	p = allocproc ();
	acquire (&ptable.lock);
	initproc = p;
	if ((p->pgdir = setupkvm ()) == 0)
		panic ("userinit: out of memory?");
	inituvm (p->pgdir, _binary_initcode_start, (int) _binary_initcode_size);
	p->sz = PGSIZE;
	memset (p->tf, 0, sizeof (*p->tf));
	p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
	p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
	p->tf->es = p->tf->ds;
	p->tf->ss = p->tf->ds;
	p->tf->eflags = FL_IF;
	p->tf->esp = PGSIZE;
	p->tf->eip = 0;		// beginning of initcode.S

	safestrcpy (p->name, "initcode", sizeof (p->name));
	p->cwd = namei ("/");

	p->state = RUNNABLE;
	p->tickets = 1000/p->pid;
	release (&ptable.lock);
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
	np->tickets = 1000/np->pid;
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
//	  via swtch back to the scheduler.
void
	addToRear (struct proc **q, struct proc* p, int *c)
{
	*q[*c] =*p;
	(*c)++;
}

void
	addToFront (int *q, int pid, int *c)
{
	//TODO if more than 64 process;
	int i;
	for (i = *c; i > 0; i++)
	{
		q[i] = q[i - 1];
	}
	q[0] = pid;
	(*c)++;
}

struct proc *
	nextReady (int *q, int *c)
{
	//cprintf("entering nextReady method  c-- %d q[0] %d \n",*c,q[0]);
	struct proc *p = 0;
	int pid;
	int i;
	for (i = 0; i < *c; i++)
	{
		//cprintf("inside for loop *c---%d \n",*c);
		pid = q[i];
		for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
		{
			//cprintf ("in the for loop before if ");
			if (p->pid == pid && p->state == RUNNABLE)
			{
				//		cprintf ("runnable process %d", p->pid);
				return p;
			}
		}
	}
	//cprintf ("runnable 0");
	return p;
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//	  via swtch back to the scheduler.
void
	scheduler (void)
{
	//struct rtcdate d;
	struct proc *p;
	int i0, i1, i2, i3;
	int j0, j1, j2, j3;
	int run_0[64];
	int foo, qqq;
	int ticket_all;
	int winner;
	int point;
	for(;;)
	{

		// Enable interrupts on this processor.
		sti();
		// Loop over process table looking for process to run.
		acquire(&ptable.lock);

		while(c0!=-1)
		{
			for(i0=0;i0<=c0;i0++)
			{
				if(q0[i0]->state == ZOMBIE)
				{
					q0[i0]=0;
					for(j0=i0;j0<=c0-1;j0++)
						q0[j0] = q0[j0+1];
					q0[c0] = 0;	
					c0--;
					i0--;		
				}
			}

			foo = -1;
			ticket_all = 0;      
			//cprintf("c0:%d\n", c0);
			for(i0 = 0; i0 <= c0; i0++)
			{        
				//cprintf("1-%d:pid=%d;p_name=%s;p_state=%d\n", i0, q0[i0]->pid, q0[i0]->name, q0[i0]->state);
				if(q0[i0]->state != RUNNABLE)
				{
					//cprintf("%d\n", q0[i0]->state);       
					continue;
				}
				else if(q0[i0]->state == RUNNABLE)
				{
					foo++;
					run_0[foo] = i0; 
				}
				//cprintf("foo:%d\n", foo);       		       		
			}
			if(foo == -1)
				break;
			for(qqq = 0; qqq <= foo; qqq++)
			{
				ticket_all += q0[run_0[qqq]] -> tickets;
				//cprintf("added\n");
			}
			cprintf("ticket_all = %d\n", ticket_all);			
			//date(d);

			winner = ((c0 + 1) * 123456) % ticket_all;
			point = 0;
			for(qqq = 0; qqq<= foo; qqq++)
			{
				point += q0[run_0[qqq]] -> tickets;
				if (winner <= point)
					break; 
			} 
			i0 = run_0[qqq];
			cprintf("winner is %d with %d tickets\n", i0, q0[i0]->tickets);

			p = q0[i0];
			proc = q0[i0];
			p->clicks++;
			switchuvm(p);
			p->state = RUNNING;
			cprintf("1-%d:pid=%d; p_name=%s; p_click=%d; c0:%d\n",i0,p->pid,p->name, p->clicks, c0);
			//cprintf("%d,%d,%d,%d\n", c0, c1, c2, c3);
			swtch(&cpu->scheduler, proc->context);
			switchkvm();

			pstat_var.ticks[p->pid][0]=p->clicks;

			if(p->clicks ==clkPerPrio[0])
			{
				cprintf("1->2\n");
				/*copy proc to lower priority queue*/
				c1++;
				proc->priority=proc->priority+1;
				pstat_var.priority[proc->pid] = proc->priority;
				q1[c1] = proc;

				/*delete proc from q0*/
				q0[i0]=0;
				for(j0=i0;j0<=c0-1;j0++)
				{
					q0[j0] = q0[j0+1];
				}
				q0[c0] = 0;
				proc->clicks = 0;
				c0--;
				i0--;				 
			}
			for(i0=0;i0<=c0;i0++)
			{
				if(q0[i0]->state == ZOMBIE)
				{
					q0[i0]=0;
					for(j0=i0;j0<=c0-1;j0++)
						q0[j0] = q0[j0+1];
					q0[c0] = 0;	
					c0--;
					i0--;		
				}
			}
			proc = 0;

		}

		while(c1!=-1)
		{
			for(i1=0;i1<=c1;i1++)
			{
				if(q1[i1]->state == ZOMBIE)
				{
					q1[i1]=0;
					for(j1=i1;j1<=c1-1;j1++)
						q1[j1] = q1[j1+1];
					q1[c1] = 0;	
					c1--;
					i1--;		
				}
			}      
			foo = -1;
			ticket_all = 0;      
			//cprintf("c0:%d\n", c0);
			for(i1 = 0; i1 <= c1; i1++)
			{        
				//cprintf("1-%d:pid=%d;p_name=%s;p_state=%d\n", i0, q0[i0]->pid, q0[i0]->name, q0[i0]->state);
				if(q1[i1]->state != RUNNABLE)
				{
					//cprintf("%d\n", q0[i0]->state);       
					continue;
				}
				else if(q1[i1]->state == RUNNABLE)
				{
					foo++;
					run_0[foo] = i1; 
				}
				//cprintf("foo:%d\n", foo);       		       		
			}
			if(foo == -1)
				break;
			for(qqq = 0; qqq <= foo; qqq++)
			{
				ticket_all += q1[run_0[qqq]] -> tickets;
				//cprintf("added\n");
			}
			cprintf("ticket_all = %d\n", ticket_all);			
			//date(d);

			winner = ((c1 + 1) * 123456) % ticket_all;
			point = 0;
			for(qqq = 0; qqq<= foo; qqq++)
			{
				point += q1[run_0[qqq]] -> tickets;
				if (winner <= point)
					break; 
			} 
			i1 = run_0[qqq];
			cprintf("winner is %d with %d tickets\n", i1, q1[i1]->tickets);

			p=q1[i1];
			proc = q1[i1];
			proc->clicks++;
			switchuvm(p);
			p->state = RUNNING;
			cprintf("2-%d:pid=%d; p_name=%s; p_click=%d; c1=%d\n",i1,p->pid,p->name, p->clicks, c1);
			//cprintf("%d,%d,%d,%d\n", c0, c1, c2, c3);
			swtch(&cpu->scheduler, proc->context);
			switchkvm();

			pstat_var.ticks[p->pid][1]=p->clicks;
			if(p->clicks ==clkPerPrio[1])
			{
				cprintf("2->3\n");
				/*copy proc to lower priority queue*/
				c2++;
				proc->priority=proc->priority+1;
				pstat_var.priority[proc->pid] = proc->priority;
				q2[c2] = proc;

				/*delete proc from q0*/
				q1[i1]=0;
				for(j1=i1;j1<=c1-1;j1++)
					q1[j1] = q1[j1+1];
				q1[c1] = 0;
				proc->clicks = 0;
				c1--;
				i1--;					  
			}
			for(i1=0;i1<=c1;i1++)
			{
				if(q1[i1]->state == ZOMBIE)
				{
					q1[i1]=0;
					for(j1=i1;j1<=c1-1;j1++)
						q1[j1] = q1[j1+1];
					q1[c1] = 0;	
					c1--;
					i1--;		
				}
			}
			proc = 0;
		}       


		while(c2!=-1)
		{
			for(i2=0;i2<=c2;i2++)
			{
				if(q2[i2]->state == ZOMBIE)
				{
					q2[i2]=0;
					for(j2=i2;j2<=c2-1;j2++)
						q2[j2] = q2[j2+1];
					q2[c2] = 0;	
					c2--;
					i2--;		
				}
			}       
			//cprintf("c2:%d\n", c2);
			foo = -1;
			ticket_all = 0;      
			//cprintf("c0:%d\n", c0);
			for(i2 = 0; i2 <= c2; i2++)
			{        
				//cprintf("1-%d:pid=%d;p_name=%s;p_state=%d\n", i0, q0[i0]->pid, q0[i0]->name, q0[i0]->state);
				if(q2[i2]->state != RUNNABLE)
				{
					//cprintf("%d\n", q0[i0]->state);       
					continue;
				}
				else if(q2[i2]->state == RUNNABLE)
				{
					foo++;
					run_0[foo] = i2; 
				}
				//cprintf("foo:%d\n", foo);       		       		
			}
			if(foo == -1)
				break;
			for(qqq = 0; qqq <= foo; qqq++)
			{
				ticket_all += q2[run_0[qqq]] -> tickets;
				//cprintf("added\n");
			}
			cprintf("ticket_all = %d\n", ticket_all);			
			//date(d);

			winner = ((c2 + 1) * 123456) % ticket_all;
			point = 0;
			for(qqq = 0; qqq<= foo; qqq++)
			{
				point += q2[run_0[qqq]] -> tickets;
				if (winner <= point)
					break; 
			} 
			i2 = run_0[qqq];
			cprintf("winner is %d with %d tickets\n", i2, q2[i2]->tickets);

			p=q2[i2];
			proc = q2[i2];
			proc->clicks++;
			switchuvm(p);
			p->state = RUNNING;
			cprintf("3-%d:pid=%d; p_name=%s; p_click=%d,c2=%d\n",i2,p->pid,p->name, p->clicks, c2);
			//cprintf("%d,%d,%d,%d\n", c0, c1, c2, c3);
			swtch(&cpu->scheduler, proc->context);
			switchkvm();

			pstat_var.ticks[p->pid][2]=p->clicks;;
			if(p->clicks ==clkPerPrio[2])
			{
				cprintf("3->4\n");
				/*copy proc to lower priority queue*/
				c3++;
				proc->priority=proc->priority+1;
				pstat_var.priority[p->pid] = p->priority;
				q3[c3] = proc;

				/*delete proc from q0*/
				q2[i2]=0;
				for(j2=i2;j2<=c2-1;j2++)
					q2[j2] = q2[j2+1];
				q2[c2] =0;
				proc->clicks = 0;
				c2--;
				i2--;
			}
			for(i2=0;i2<=c2;i2++)
			{
				if(q2[i2]->state == ZOMBIE)
				{
					q2[i2]=0;
					for(j2=i2;j2<=c2-1;j2++)
						q2[j2] = q2[j2+1];
					q2[c2] = 0;	
					c2--;
					i2--;		
				}
			}
			proc = 0;
		}            


		while(c3!=-1)
		{
			for(i3=0;i3<=c3;i3++)
			{
				if(q3[i3]->state == ZOMBIE)
				{
					q3[i3]=0;
					for(j3=i3;j3<=c3-1;j3++)
						q3[j3] = q3[j3+1];
					q3[c3] = 0;	
					c3--;
					i3--;		
				}
			}      
			//cprintf("c3:%d\n", c3);
			foo = -1;
			ticket_all = 0;      
			//cprintf("c0:%d\n", c0);
			for(i3 = 0; i3 <= c3; i3++)
			{        
				//cprintf("1-%d:pid=%d;p_name=%s;p_state=%d\n", i0, q0[i0]->pid, q0[i0]->name, q0[i0]->state);
				if(q3[i3]->state != RUNNABLE)
				{
					//cprintf("%d\n", q0[i0]->state);       
					continue;
				}
				else if(q3[i3]->state == RUNNABLE)
				{
					foo++;
					run_0[foo] = i3; 
				}
				//cprintf("foo:%d\n", foo);       		       		
			}
			if(foo == -1)
				break;
			for(qqq = 0; qqq <= foo; qqq++)
			{
				ticket_all += q3[run_0[qqq]] -> tickets;
				//cprintf("added\n");
			}
			cprintf("ticket_all = %d\n", ticket_all);			
			//date(d);

			winner = ((c3 + 1) * 123456) % ticket_all;
			point = 0;
			for(qqq = 0; qqq<= foo; qqq++)
			{
				point += q3[run_0[qqq]] -> tickets;
				if (winner <= point)
					break; 
			} 
			i3 = run_0[qqq];
			cprintf("winner is %d with %d tickets\n", i3, q3[i3]->tickets);

			p=q3[i3];
			proc = q3[i3];
			proc->clicks++;
			switchuvm(p);
			p->state = RUNNING;
			cprintf("4-%d:pid=%d; p_name=%s; p_click=%d,c3=%d\n",i3,p->pid,p->name, p->clicks,c3);
			//cprintf("%d,%d,%d,%d\n", c0, c1, c2, c3);
			swtch(&cpu->scheduler, proc->context);
			switchkvm();

			pstat_var.priority[p->pid] = p->priority;
			pstat_var.ticks[p->pid][3]=p->clicks;;
			/*move process to end of its own queue */						 


			for(i3=0;i3<=c3;i3++)
			{
				if(q3[i3]->state == ZOMBIE)
				{
					q3[i3]=0;
					for(j3=i3;j3<=c3-1;j3++)
						q3[j3] = q3[j3+1];
					q3[c3] = 0;	
					c3--;
					i3--;		
				}
			}
			proc = 0;     
		}

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
	if(proc->state == RUNNING)
		panic("sched running");
	if(readeflags()&FL_IF)
		panic("sched interruptible");
	intena = cpu->intena;
	swtch(&proc->context, cpu->scheduler);
	cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void
	yield(void)
{
	acquire(&ptable.lock);  //DOC: yieldlock
	proc->state = RUNNABLE;
	proc->tickets = 1000/proc->pid;
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
		{
			p->state = RUNNABLE;
			p->tickets = 1000/p->pid;
		}

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
			{
				p->state = RUNNABLE;
				p->tickets = 1000/p->pid;
			}

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
		[UNUSED]	"unused",
		[EMBRYO]	"embryo",
		[SLEEPING]  "sleep ",
		[RUNNABLE]  "runble",
		[RUNNING]   "run   ",
		[ZOMBIE]	"zombie"
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
