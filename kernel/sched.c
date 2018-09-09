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

#define _S(nr) (1<<((nr)-1)) /* �źŴ�1��ʼ ��bit��0��ʼ */
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))/* ���Ա�block���ź� */

void show_task(int nr,struct task_struct * p)/* ��ʾnr���������Ϣ */
{
	int i,j = 4096-sizeof(struct task_struct);/* j=�ں˶�ջ�ĵײ� */

	printk("%d: pid=%d, state=%d, father=%d, child=%d, ",nr,p->pid,
		p->state, p->p_pptr->pid, p->p_cptr ? p->p_cptr->pid : -1);/* ��ӡpid״�� */
	i=0;
	while (i<j && !((char *)(p+1))[i])/* �����ں˶�ջ�ײ�Ϊ0�Ĳ��� */
		i++;
	printk("%d/%d chars free in kstack\n\r",i,j); /* i�ǿ��еĴ�С j���ں˶�ջ���ܴ�С */
	printk("   PC=%08X.", *(1019 + (unsigned long *) p)); /* ��ӡ�ں˶�ջ�е�ipָ�룬�������ں�ʱ�û�̬�Ĵ����ip��ַ */
	if (p->p_ysptr || p->p_osptr) 					/* �����ջ�Ĳ��� */
		printk("   Younger sib=%d, older sib=%d\n\r", 
			p->p_ysptr ? p->p_ysptr->pid : -1,/*  */
			p->p_osptr ? p->p_osptr->pid : -1);
	else
		printk("\n\r");
}

/* ����show_task��ʾ���������״̬ */
void show_state(void)
{
	int i;/*  */

	printk("\rTask-info:\n\r");/*  */
	for (i=0;i<NR_TASKS;i++)
		if (task[i])
			show_task(i,task[i]);
}

//8253��ʱ����ʼֵ  100hz 10msһ���ж�
#define LATCH (1193180/HZ)

extern void mem_use(void);

/* ʱ���жϺ�ϵͳ���õ���� */
extern int timer_interrupt(void);
extern int system_call(void);

union task_union {
	struct task_struct task;/*  */
	char stack[PAGE_SIZE];
};

//0������ �����Ǿ�̬�����
static union task_union init_task = {INIT_TASK,};/*  */

unsigned long volatile jiffies=0;/*  */
unsigned long startup_time=0;
int jiffies_offset = 0;		/* # clock ticks to add to get "true
				   time".  Should always be less than
				   1 second's worth.  For time fanatics
				   who like to syncronize their machines
				   to WWV :-) */

struct task_struct *current = &(init_task.task);/* ��ʼ��currentΪ0������ */
struct task_struct *last_task_used_math = NULL;

struct task_struct * task[NR_TASKS] = {&(init_task.task), }; /* ��ʼ����ռ�õ�0�� ��Ϊ0������ */

//����0���û�̬��ջ   �ں��л�������0����ǰ �����ں˵ĳ�ʼ��ʹ�õĶ�ջ
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
		if (*p) { /* ������������ */
			if ((*p)->timeout && (*p)->timeout < jiffies) { /* ������ù�timeout �ҵ�ȷ�Ѿ�timeout */
				(*p)->timeout = 0;/* ��reset timeout=0 */
				if ((*p)->state == TASK_INTERRUPTIBLE)/* �ҽ���������Ϊ TASK_RUNNING*/
					(*p)->state = TASK_RUNNING;
			}
			if ((*p)->alarm && (*p)->alarm < jiffies) {/* ������ù�alarm ���Ѿ����� */
				(*p)->signal |= (1<<(SIGALRM-1));/* ������signal bitmap�еĶ�Ӧλ   ��ret_from_syscall�лᴦ���ź�*/
				(*p)->alarm = 0;  /* reset alarm */
			}
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&/* ������ź���Ҫ���� �������� TASK_INTERRUPTIBLE*/
			(*p)->state==TASK_INTERRUPTIBLE)/*  */
				(*p)->state=TASK_RUNNING;/*  ������״̬����ΪTASK_RUNNING */
		}

/* this is the scheduler proper: */

	while (1) {
		c = -1;/*  */
		next = 0;
		i = NR_TASKS;/*  */
		p = &task[NR_TASKS];/* pָ�����һ������ */
		while (--i) {/* ������������ */
			if (!*--p)/* �����ǰindexû��ָ������ ������ */
				continue;
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)/* �������״̬��TASK_RUNNING ��ʱ��Ƭ����c */
				c = (*p)->counter, next = i; /* ��next=i  ѭ������Ӧ��nextӦ����ʱ��Ƭ���Ŀ����н���*/
		}
		if (c) break;/* c!=0 ��֤��ѡ��������next breakִ��switch to */
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)/* ������ζ����������ʱ��Ƭ��������  �����������������ʱ��Ƭ */
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;/*  */
	}
	switch_to(next);
}

int sys_pause(void)/* pauseϵͳ����   ���Լ�����Ϊ TASK_INTERRUPTIBLE Ȼ�����schedule*/
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}

static inline void __sleep_on(struct task_struct **p, int state)/* ��ָ����Դ�Ķ�����˯�ߣ�p����Դ��˯�߶���ͷ һ��ʼӦ����null */
{
	struct task_struct *tmp;

	if (!p)/* ��ָ�� �˳� */
		return;
	if (current == &(init_task.task))/* 0������ ������˯�� */
		panic("task[0] trying to sleep");
	tmp = *p;/* tmpָ����Դ����ͷ ������null */
	*p = current;/* ��Դ����ͷָ��ǰҪ˯�ߵ����� */
	current->state = state;/*  */
repeat:	schedule();/* ���е��� */

	if (*p && *p != current) {/* ���������µ���ʱ�������Դ����ͷָ��һ�������Լ��Ľ���  */
		(**p).state = 0;/* ����Ҫ����ǰ����ͷ���Ľ���״̬����Ϊrunning */
		current->state = TASK_UNINTERRUPTIBLE;/* �ҽ��Լ���������Ϊ TASK_UNINTERRUPTIBLE�� ���Լ������﻽�ѵĽ��̻����ν������ϵ���һ�����̻���  ������Ĵ���*/
		goto repeat;/*  */
	}
	if (!*p)/*  */
		printk("Warning: *P = NULL\n\r");
	if (*p = tmp)/* �Լ������ˣ������Ҫ�˳���Դ�ȴ����У����½���Դ�ȴ�����ͷָ��֮ǰ��ͷ�� */
		tmp->state=0;/* ���������Լ�֮ǰ������е�ͷ������ */
}

void interruptible_sleep_on(struct task_struct **p)/* __sleep_on�ķ�װ ����TASK_INTERRUPTIBLE */
{
	__sleep_on(p,TASK_INTERRUPTIBLE);
}

void sleep_on(struct task_struct **p)
{
	__sleep_on(p,TASK_UNINTERRUPTIBLE); /* ���� TASK_UNINTERRUPTIBLE �޷����źŻ���*/
}


/* ͬ����� ��ĳ����Դ�ĵȴ�����ͷ  wake up������������ϵĵ�һ������ */
void wake_up(struct task_struct **p)
{
	if (p && *p) {
		if ((**p).state == TASK_STOPPED)
			printk("wake_up: TASK_STOPPED");
		if ((**p).state == TASK_ZOMBIE)
			printk("wake_up: TASK_ZOMBIE");
		(**p).state=0; /* ����Ϊrunning */
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
int ticks_to_floppy_on(unsigned int nr)/* ���Ķ������豸��ʱ���ٽ��з��� */
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

/* ��������64��timer */
#define TIME_REQUESTS 64 

static struct timer_list {/* ��ʱ���ṹ ��ʱʱ��ʹ����� */
	long jiffies;
	void (*fn)();
	struct timer_list * next;
} timer_list[TIME_REQUESTS], * next_timer = NULL;  /* �����ʱ��������ȫ�ֵ� */

/* ��Ӷ�ʱ��  ��������ʹ�ö�ʱ��ִ��������ر�������ʱ���� */
void add_timer(long jiffies, void (*fn)(void))/* ���һ��timer list��ȫ����timer_list���� */
{
	struct timer_list * p;

	if (!fn)  /* û��ָ����ʱ�������� ֱ�ӷ��� */
		return;
	cli();
	if (jiffies <= 0)/* ָ���ĳ�ʱʱ���Ѿ����� ֱ��ִ�д����� */
		(fn)();
	else {
		for (p = timer_list ; p < timer_list + TIME_REQUESTS ; p++)/*  ����Ӷ�ʱ���������ҵ�һ�������� */
			if (!p->fn)/* �ҵ��� ����ѭ�� */
				break;
		if (p >= timer_list + TIME_REQUESTS)/* 64����������� panic */
			panic("No more time requests free");
		p->fn = fn;/* ����ҵ��Ŀ����� */
		p->jiffies = jiffies;
		p->next = next_timer; /* �γ����� ���ղ���˳�� ��������°��ճ�ʱʱ������ */
		next_timer = p;   /* next timerָ��������Ķ�ʱ��  ���䳬ʱʱ�䲻һ������С�� */
		while (p->next && p->next->jiffies < p->jiffies) {/* ��ʱ�����ճ�ʱʱ������ */
			//��ô���ĺô���ÿ��ʱ���ж�ֻҪ�Ե�һ��timer�ĳ�ʱʱ�����--
			p->jiffies -= p->next->jiffies;/* �����߼������� �������Ķ�ʱ����ʱʱ��ȵ�һ��С �򲻻����ѭ��*/
								/*��ʱԭ����һ����ʱ����ʱʱ�� û�м�ȥ �²���Ķ�ʱ���ĳ�ʱʱ�� �ǲ��Ե�*/
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

/* ��ʱ���жϴ������б�����  cpl��ָ����ʱ���ж�ʱ��ǰ����ִ�еĴ���ε�cpl ���û�̬�����ں�̬ */
/* ������Ҫ�������ʱ��Ƭ  */
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
	//���洦����Ļ���� ����
	
	if (hd_timeout)/* hd��ʱ�����ݼ���=0 ����Ӳ�̷��ʳ�ʱ���� */
		if (!--hd_timeout)
			hd_times_out();

	if (beepcount)/* �������������� �رշ��� */
		if (!--beepcount)
			sysbeepstop();

	if (cpl)/* ����cpl���������û�̬�����ں�̬����ʱ�� */
		current->utime++; /* ��ʵ������� ����ȷ  ���������ʱ���ж��м䷢���ں�̬���û�̬���л� �����countӦ�����û�̬���ں�̬��0.5 */
	else
		current->stime++;

	if (next_timer) {/*next_timerָ��ʱ������ͷ�� ������adder_timerʱ��ذ�ʱ������(��bug)  */
		next_timer->jiffies--; /* ͷ����ʱ��--  ���г�ʱ��ʱ��ִ�����˺�  ��һ����ʱ��ҲҪ-- */
		while (next_timer && next_timer->jiffies <= 0) {/* ��ʱ��ִ�д����� */
			void (*fn)(void);
			
			fn = next_timer->fn;/*  */
			next_timer->fn = NULL;/*  */
			next_timer = next_timer->next;/*  */
			(fn)();/*  */
		}
	}
	
	if (current_DOR & 0xf0)/* ���������� */
		do_floppy_timer();/*  */
	//��ǰ����ʱ��Ƭ ��û������ ��ֱ���˳�
	if ((--current->counter)>0) return;/*  */
	current->counter=0;/* ����ʱ��Ƭ ��counter��0 */
	if (!cpl) return; /* ��ʱ���жϷ������ں�̬ ��ʱ���ǲ�������ִ��schedule  �������ν���ں˲�����ռ */
	schedule();/* ��֮ ʱ���жϷ������û�̬  ����ֱ����ռ */
}

int sys_alarm(long seconds)/*  */
{/*  */
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ; /* ʣ��δ��� */
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0; /* ����ת���ɵδ��� ���õ�task struct�� */
	return (old);/*����ʣ��δ��� */
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

int sys_nice(long increment)/* ���ݲ������ͽ��̵�prio �����������0  */
{
	if (current->priority-increment>0) /* �������С��0 ����ܻ�ʹ�������ȼ����� */
		current->priority -= increment;
	return 0;
}

void sched_init(void)/*  */
{
	int i;
	struct desc_struct * p;

	if (sizeof(struct sigaction) != 16)
		panic("Struct sigaction MUST be 16 bytes");
	//��������0��tss��ldt��gdt
	set_tss_desc(gdt+FIRST_TSS_ENTRY,&(init_task.task.tss));
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt));

	//����gdt��ʣ������tss��ldt
	p = gdt+2+FIRST_TSS_ENTRY;
	//�������������task struct�Ͷ�Ӧ��tss��ldtΪ��Чֵ
	for(i=1;i<NR_TASKS;i++) {
		task[i] = NULL;
		p->a=p->b=0;
		p++;
		p->a=p->b=0;
		p++;
	}
/* Clear NT, so that we won't have troubles with that later on */
//���eflags�е�NT��־��NT��־��ʹ�������Ŵ����ж�ʱ���ڹ��췵����
	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl");
//�ֶ���������0��ldt��tr�Ĵ���
	ltr(0);
	lldt(0);
	//�����Ƕ�8259���
	outb_p(0x36,0x43);		/* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	outb(LATCH >> 8 , 0x40);	/* MSB */
	//����ʱ���ж�
	set_intr_gate(0x20,&timer_interrupt);
	outb(inb_p(0x21)&~0x01,0x21);
	//����ϵͳ����ʹ�õ�idt��
	set_system_gate(0x80,&system_call);
}
