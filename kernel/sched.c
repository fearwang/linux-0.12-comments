/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

#define _S(nr) (1<<((nr)-1)) /* 信号从1开始 而bit从0开始 */
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))/* 可以被block的信号 */

void show_task(int nr,struct task_struct * p)/* 显示nr号任务的信息 */
{
	int i,j = 4096-sizeof(struct task_struct);/* j=内核堆栈的底部 */

	printk("%d: pid=%d, state=%d, father=%d, child=%d, ",nr,p->pid,
		p->state, p->p_pptr->pid, p->p_cptr ? p->p_cptr->pid : -1);/* 打印pid状况 */
	i=0;
	while (i<j && !((char *)(p+1))[i])/* 跳过内核堆栈底部为0的部分 */
		i++;
	printk("%d/%d chars free in kstack\n\r",i,j); /* i是空闲的大小 j是内核堆栈的总大小 */
	printk("   PC=%08X.", *(1019 + (unsigned long *) p)); /* 打印内核堆栈中的ip指针，即陷入内核时用户态的代码段ip地址 */
	if (p->p_ysptr || p->p_osptr) 					/* 想想堆栈的布局 */
		printk("   Younger sib=%d, older sib=%d\n\r", 
			p->p_ysptr ? p->p_ysptr->pid : -1,/*  */
			p->p_osptr ? p->p_osptr->pid : -1);
	else
		printk("\n\r");
}

/* 调用show_task显示所有任务的状态 */
void show_state(void)
{
	int i;/*  */

	printk("\rTask-info:\n\r");/*  */
	for (i=0;i<NR_TASKS;i++)
		if (task[i])
			show_task(i,task[i]);
}

//8253定时器初始值  100hz 10ms一个中断
#define LATCH (1193180/HZ)

extern void mem_use(void);

/* 时钟中断和系统调用的入口 */
extern int timer_interrupt(void);
extern int system_call(void);

union task_union {
	struct task_struct task;/*  */
	char stack[PAGE_SIZE];
};

//0号任务 数据是静态定义的
static union task_union init_task = {INIT_TASK,};/*  */

unsigned long volatile jiffies=0;/*  */
unsigned long startup_time=0;
int jiffies_offset = 0;		/* # clock ticks to add to get "true
				   time".  Should always be less than
				   1 second's worth.  For time fanatics
				   who like to syncronize their machines
				   to WWV :-) */

struct task_struct *current = &(init_task.task);/* 初始化current为0号任务 */
struct task_struct *last_task_used_math = NULL;

struct task_struct * task[NR_TASKS] = {&(init_task.task), }; /* 初始任务占用第0项 即为0号任务 */

//任务0的用户态堆栈   内核切换到任务0运行前 它是内核的初始化使用的堆栈
long user_stack [ PAGE_SIZE>>2 ] ;/*  */

struct {
	long * a;
	short b;
	} stack_start = { & user_stack [PAGE_SIZE>>2] , 0x10 };
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */
void math_state_restore()/*  */
{
	if (last_task_used_math == current)
		return;
	__asm__("fwait");
	if (last_task_used_math) {
		__asm__("fnsave %0"::"m" (last_task_used_math->tss.i387));
	}
	last_task_used_math=current;
	if (current->used_math) {
		__asm__("frstor %0"::"m" (current->tss.i387));
	} else {
		__asm__("fninit"::);
		current->used_math=1;
	}
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */
void schedule(void)/*  */
{
	int i,next,c;
	struct task_struct ** p;

/* check alarm, wake up any interruptible tasks that have got a signal */

	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		if (*p) { /* 遍历任务数组 */
			if ((*p)->timeout && (*p)->timeout < jiffies) { /* 如果设置过timeout 且的确已经timeout */
				(*p)->timeout = 0;/* 则reset timeout=0 */
				if ((*p)->state == TASK_INTERRUPTIBLE)/* 且将任务设置为 TASK_RUNNING*/
					(*p)->state = TASK_RUNNING;
			}
			if ((*p)->alarm && (*p)->alarm < jiffies) {/* 如果设置过alarm 且已经过期 */
				(*p)->signal |= (1<<(SIGALRM-1));/* 则设置signal bitmap中的对应位   在ret_from_syscall中会处理信号*/
				(*p)->alarm = 0;  /* reset alarm */
			}
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&/* 如果有信号需要处理 且任务是 TASK_INTERRUPTIBLE*/
			(*p)->state==TASK_INTERRUPTIBLE)/*  */
				(*p)->state=TASK_RUNNING;/*  则将任务状态设置为TASK_RUNNING */
		}

/* this is the scheduler proper: */

	while (1) {
		c = -1;/*  */
		next = 0;
		i = NR_TASKS;/*  */
		p = &task[NR_TASKS];/* p指向最后一个任务 */
		while (--i) {/* 遍历任务数组 */
			if (!*--p)/* 如果当前index没有指向任务 则跳过 */
				continue;
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)/* 如果任务状态是TASK_RUNNING 且时间片大于c */
				c = (*p)->counter, next = i; /* 则next=i  循环完了应该next应该是时间片最大的可运行进程*/
		}
		if (c) break;/* c!=0 则证明选择到了任务next break执行switch to */
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)/* 否则意味着所有任务时间片都用完了  重新设置所有任务的时间片 */
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;/*  */
	}
	switch_to(next);
}

int sys_pause(void)/* pause系统调用   将自己设置为 TASK_INTERRUPTIBLE 然后调用schedule*/
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}

static inline void __sleep_on(struct task_struct **p, int state)/* 在指定资源的队列上睡眠，p是资源的睡眠队列头 一开始应该是null */
{
	struct task_struct *tmp;

	if (!p)/* 空指针 退出 */
		return;
	if (current == &(init_task.task))/* 0号任务 不允许睡眠 */
		panic("task[0] trying to sleep");
	tmp = *p;/* tmp指向资源队列头 可能是null */
	*p = current;/* 资源队列头指向当前要睡眠的任务 */
	current->state = state;/*  */
repeat:	schedule();/* 进行调度 */

	if (*p && *p != current) {/* 当任务被重新调度时，如果资源队列头指向一个不是自己的进程  */
		(**p).state = 0;/* 则需要将当前队列头部的进程状态设置为running */
		current->state = TASK_UNINTERRUPTIBLE;/* 且将自己重新设置为 TASK_UNINTERRUPTIBLE， 被自己在这里唤醒的进程会依次将队列上的下一个进程唤醒  见下面的代码*/
		goto repeat;/*  */
	}
	if (!*p)/*  */
		printk("Warning: *P = NULL\n\r");
	if (*p = tmp)/* 自己唤醒了，因此需要退出资源等待队列，重新将资源等待队列头指向之前的头部 */
		tmp->state=0;/* 并唤醒在自己之前进入队列的头部进程 */
}

void interruptible_sleep_on(struct task_struct **p)/* __sleep_on的封装 进入TASK_INTERRUPTIBLE */
{
	__sleep_on(p,TASK_INTERRUPTIBLE);
}

void sleep_on(struct task_struct **p)
{
	__sleep_on(p,TASK_UNINTERRUPTIBLE); /* 进入 TASK_UNINTERRUPTIBLE 无法被信号唤醒*/
}


/* 同样入参 是某个资源的等待队列头  wake up唤醒这个队列上的第一个进程 */
void wake_up(struct task_struct **p)
{
	if (p && *p) {
		if ((**p).state == TASK_STOPPED)
			printk("wake_up: TASK_STOPPED");
		if ((**p).state == TASK_ZOMBIE)
			printk("wake_up: TASK_ZOMBIE");
		(**p).state=0; /* 设置为running */
	}
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
static struct task_struct * wait_motor[4] = {NULL,NULL,NULL,NULL};
static int  mon_timer[4]={0,0,0,0};
static int moff_timer[4]={0,0,0,0};
unsigned char current_DOR = 0x0C;

//
int ticks_to_floppy_on(unsigned int nr)/* 等阅读到块设备的时候再进行分析 */
{
	extern unsigned char selected;
	unsigned char mask = 0x10 << nr;

	if (nr>3)
		panic("floppy_on: nr>3");
	moff_timer[nr]=10000;		/* 100 s = very big :-) */
	cli();				/* use floppy_off to turn it off */
	mask |= current_DOR;
	if (!selected) {
		mask &= 0xFC;
		mask |= nr;
	}
	if (mask != current_DOR) {
		outb(mask,FD_DOR);
		if ((mask ^ current_DOR) & 0xf0)
			mon_timer[nr] = HZ/2;
		else if (mon_timer[nr] < 2)
			mon_timer[nr] = 2;
		current_DOR = mask;
	}
	sti();
	return mon_timer[nr];
}

void floppy_on(unsigned int nr)/*  */
{
	cli();
	while (ticks_to_floppy_on(nr))
		sleep_on(nr+wait_motor);
	sti();
}

void floppy_off(unsigned int nr)/*  */
{
	moff_timer[nr]=3*HZ;
}

void do_floppy_timer(void)/*  */
{
	int i;
	unsigned char mask = 0x10;

	for (i=0 ; i<4 ; i++,mask <<= 1) {
		if (!(mask & current_DOR))
			continue;
		if (mon_timer[i]) {
			if (!--mon_timer[i])
				wake_up(i+wait_motor);
		} else if (!moff_timer[i]) {
			current_DOR &= ~mask;
			outb(current_DOR,FD_DOR);
		} else
			moff_timer[i]--;
	}
}

/* 最多可以有64个timer */
#define TIME_REQUESTS 64 

static struct timer_list {/* 定时器结构 超时时间和处理函数 */
	long jiffies;
	void (*fn)();
	struct timer_list * next;
} timer_list[TIME_REQUESTS], * next_timer = NULL;  /* 这个定时器数组是全局的 */

/* 添加定时器  软驱驱动使用定时器执行启动或关闭马达的延时操作 */
void add_timer(long jiffies, void (*fn)(void))/* 添加一个timer list到全集的timer_list数组 */
{
	struct timer_list * p;

	if (!fn)  /* 没有指定定时器处理函数 直接返回 */
		return;
	cli();
	if (jiffies <= 0)/* 指定的超时时间已经到了 直接执行处理函数 */
		(fn)();
	else {
		for (p = timer_list ; p < timer_list + TIME_REQUESTS ; p++)/*  否则从定时器数组中找到一个空闲项 */
			if (!p->fn)/* 找到了 跳出循环 */
				break;
		if (p >= timer_list + TIME_REQUESTS)/* 64个项都被用完了 panic */
			panic("No more time requests free");
		p->fn = fn;/* 填充找到的空闲项 */
		p->jiffies = jiffies;
		p->next = next_timer; /* 形成链表 按照插入顺序 后面会重新按照超时时间排序 */
		next_timer = p;   /* next timer指向最后插入的定时器  但其超时时间不一定是最小的 */
		while (p->next && p->next->jiffies < p->jiffies) {/* 定时器按照超时时间排序 */
			//这么做的好处是每次时钟中断只要对第一个timer的超时时间进行--
			p->jiffies -= p->next->jiffies;/* 代码逻辑有问题 如果插入的定时器超时时间比第一个小 则不会进入循环*/
								/*此时原来第一个定时器超时时间 没有减去 新插入的定时器的超时时间 是不对的*/
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}

/* 在时钟中断处理函数中被调用  cpl是指发生时钟中断时当前正在执行的代码段的cpl 是用户态还是内核态 */
/* 这里面要处理进程时间片  */
void do_timer(long cpl)
{
	static int blanked = 0;

	if (blankcount || !blankinterval) {
		if (blanked)
			unblank_screen();
		if (blankcount)
			blankcount--;
		blanked = 0;
	} else if (!blanked) {
		blank_screen();
		blanked = 1;
	}
	//上面处理屏幕亮灭 不看
	
	if (hd_timeout)/* hd超时计数递减后=0 进行硬盘访问超时处理 */
		if (!--hd_timeout)
			hd_times_out();

	if (beepcount)/* 发声计数次数到 关闭发声 */
		if (!--beepcount)
			sysbeepstop();

	if (cpl)/* 根据cpl决定增加用户态或者内核态运行时间 */
		current->utime++; /* 其实这个计算 不精确  如果在两次时钟中断中间发声内核态和用户态的切换 则这个count应该是用户态和内核态各0.5 */
	else
		current->stime++;

	if (next_timer) {/*next_timer指向定时器链表头部 链表在adder_timer时候回按时间排序(有bug)  */
		next_timer->jiffies--; /* 头部定时器--  所有超时定时器执行完了后  下一个定时器也要-- */
		while (next_timer && next_timer->jiffies <= 0) {/* 超时则执行处理函数 */
			void (*fn)(void);
			
			fn = next_timer->fn;/*  */
			next_timer->fn = NULL;/*  */
			next_timer = next_timer->next;/*  */
			(fn)();/*  */
		}
	}
	
	if (current_DOR & 0xf0)/* 软盘马达相关 */
		do_floppy_timer();/*  */
	//当前进程时间片 还没有用完 则直接退出
	if ((--current->counter)>0) return;/*  */
	current->counter=0;/* 用完时间片 将counter置0 */
	if (!cpl) return; /* 当时钟中断发生在内核态 此时我们不会马上执行schedule  这就是所谓的内核不可抢占 */
	schedule();/* 反之 时钟中断发生在用户态  可以直接抢占 */
}

int sys_alarm(long seconds)/*  */
{/*  */
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ; /* 剩余滴答数 */
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0; /* 秒数转换成滴答数 设置到task struct中 */
	return (old);/*返回剩余滴答数 */
}

int sys_getpid(void)/*  */
{
	return current->pid;
}

int sys_getppid(void)/*  */
{
	return current->p_pptr->pid;
}

int sys_getuid(void)/*  */
{
	return current->uid;
}

int sys_geteuid(void)/*  */
{
	return current->euid;
}

int sys_getgid(void)/*  */
{
	return current->gid;
}

int sys_getegid(void)/*  */
{
	return current->egid;
}

int sys_nice(long increment)/* 根据参数降低进程的prio 如果参数大于0  */
{
	if (current->priority-increment>0) /* 如果参数小于0 则可能会使进程优先级增大 */
		current->priority -= increment;
	return 0;
}

void sched_init(void)/*  */
{
	int i;
	struct desc_struct * p;

	if (sizeof(struct sigaction) != 16)
		panic("Struct sigaction MUST be 16 bytes");
	//设置任务0的tss和ldt到gdt
	set_tss_desc(gdt+FIRST_TSS_ENTRY,&(init_task.task.tss));
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt));

	//设置gdt中剩下来的tss和ldt
	p = gdt+2+FIRST_TSS_ENTRY;
	//设置所有任务的task struct和对应的tss和ldt为无效值
	for(i=1;i<NR_TASKS;i++) {
		task[i] = NULL;
		p->a=p->b=0;
		p++;
		p->a=p->b=0;
		p++;
	}
/* Clear NT, so that we won't have troubles with that later on */
//清除eflags中的NT标志，NT标志在使用任务门处理中断时用于构造返回链
	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl");
//手动加载任务0的ldt和tr寄存器
	ltr(0);
	lldt(0);
	//以下是对8259编程
	outb_p(0x36,0x43);		/* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	outb(LATCH >> 8 , 0x40);	/* MSB */
	//设置时钟中断
	set_intr_gate(0x20,&timer_interrupt);
	outb(inb_p(0x21)&~0x01,0x21);
	//设置系统调用使用的idt项
	set_system_gate(0x80,&system_call);
}
