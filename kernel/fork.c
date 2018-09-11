/*
 *  linux/kernel/fork.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s), and some misc functions ('verify_area').
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
 */
#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>
/* 写验证 如果不可写 则申请页面并复制 */
extern void write_verify(unsigned long address);

long last_pid=0;/* find_empty_process 设置 */

/* 进程空间区域写前验证 这个函数是因为386机器 特权级0可以写用户空间只读页面  因此需要自己检查  而没有缺页异常在内核态 */
void verify_area(void * addr,int size)
{
	unsigned long start;

	start = (unsigned long) addr;/*  */
	size += start & 0xfff;/* 按页为单位 检查    将size扩展成从所在页面基地址开始的长度*/
	start &= 0xfffff000;/* 对其到所属于页面的起始地址 */
	start += get_base(current->ldt[2]);/* 逻辑地址加上段基地址变成线性地址 */
	while (size>0) {/*  */
		size -= 4096;/*  */
		write_verify(start);/* 验证一个页面 循环 */
		start += 4096;/*  */
	}
}

/* 复制内页页表 在任务的地址空间中设置代码段的base和limit 并复制页表*/
int copy_mem(int nr/* 新任务号 */,struct task_struct * p/* 新任务task struct */)
{
	unsigned long old_data_base,new_data_base,data_limit;/*  */
	unsigned long old_code_base,new_code_base,code_limit;

	code_limit=get_limit(0x0f);/* 首先取当前进程的ldt中的代码和数据段限长 */
	data_limit=get_limit(0x17);/*  */
	old_code_base = get_base(current->ldt[1]);/* 然后取代码段和数据段基地址 */
	old_data_base = get_base(current->ldt[2]);/*  */
	if (old_data_base != old_code_base)/* 0.12要求代码和数据段起始地址一致 */
		panic("We don't support separate I&D");/*  */
	if (data_limit < code_limit)/* 并要求数据段要长于代码段 */
		panic("Bad data_limit");/*  */
	new_data_base = new_code_base = nr * TASK_SIZE;/* 根据任务号得到基地址 */
	p->start_code = new_code_base;/* start code = 段基地址 */
	set_base(p->ldt[1],new_code_base);/* 设置局部数据段和代码段的base地址 */
	set_base(p->ldt[2],new_data_base);/*  */
	if (copy_page_tables(old_data_base,new_data_base,data_limit)) {/* 复制页表 */
		free_page_tables(new_data_base,data_limit);/* 失败则释放分配的页表 */
		return -ENOMEM;/*  */
	}/*  */
	return 0;/* 成功返回0 */
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 */
 
 /* fork的主要逻辑 入参是汇编阶段压入的 */
/* 从这里看 子进程被调度后 直接从用户态开始运行，data和stack一旦发生写操作就会·发生写时复制
	但有一点，sp此时已经不是满的了，和父进程一样的状态

	执行exec后应该会被重新置位
*/
int copy_process(int nr/* 找到的空闲任务号 */,long ebp,long edi,long esi,long gs,long none,
		long ebx,long ecx,long edx, long orig_eax, 
		long fs,long es,long ds,
		long eip,long cs,long eflags,long esp,long ss)
{
	struct task_struct *p;/*  */
	int i;/*  */
	struct file *f;

	p = (struct task_struct *) get_free_page();/* 首先分配一页给task struct */
	if (!p)
		return -EAGAIN;/* 没有分配到页 则出错返回 */
	task[nr] = p;/* 人数数组指针指向task struct */
	*p = *current;	/* NOTE! this doesn't copy the supervisor stack 完全拷贝task struct*/
	p->state = TASK_UNINTERRUPTIBLE;/* 设置fork出来的任务为 TASK_UNINTERRUPTIBLE*/
	p->pid = last_pid;/* 新程号 */
	p->counter = p->priority;/* 运行时间片 */
	p->signal = 0;/* 信号位图 */
	p->alarm = 0;/* 闹钟定时器清0 */
	p->leader = 0;		/* process leadership doesn't inherit 进程的领导权不能继承*/
	p->utime = p->stime = 0;/* 用户态和内核态运行时间清0 */
	p->cutime = p->cstime = 0;/* 子进程的用户态和和心态的运行时间 */
	p->start_time = jiffies;/* 进程开始运行时间 */

	// 下面设置tss
	p->tss.back_link = 0;/*  */
	p->tss.esp0 = PAGE_SIZE + (long) p;/* 内核栈指向task struct所在页顶端 */
	p->tss.ss0 = 0x10;/* 内核数据段 */
	/* fork后的子进程，后面如果被调度到，则竟然从用户态开始运行 */
	p->tss.eip = eip;/* 返回地址和调用fork的父进程是一样的 */
	p->tss.eflags = eflags;/*  */
	p->tss.eax = 0;/* 子进程应该返回0 */
	p->tss.ecx = ecx;/*  */
	p->tss.edx = edx;
	p->tss.ebx = ebx;
	p->tss.esp = esp; /* 也是用户态的堆栈指针 */
	p->tss.ebp = ebp;
	p->tss.esi = esi;
	p->tss.edi = edi;
	p->tss.es = es & 0xffff;/* 段寄存器仅16bit  这里cs只是一个索引*/ 
	p->tss.cs = cs & 0xffff; /* fork后的子进程，后面如果被调度到，则竟然从用户态开始运行 */
	p->tss.ss = ss & 0xffff; 
	p->tss.ds = ds & 0xffff;
	p->tss.fs = fs & 0xffff;
	p->tss.gs = gs & 0xffff;
	p->tss.ldt = _LDT(nr);/* 设置ldt字段指向 子进程的ldt段描述符 */
	p->tss.trace_bitmap = 0x80000000;/*  */
	
	if (last_task_used_math == current)/* 协处理器相关 */
		__asm__("clts ; fnsave %0 ; frstor %0"::"m" (p->tss.i387));
	/* 下面开始复制进程页表 */
	//同时这里会设置ldt的base和limit
	if (copy_mem(nr,p)) {
		task[nr] = NULL;/* 失败则释放任务数组 */
		free_page((long) p);/* 释放task struct占用的页 */
		return -EAGAIN;/* 返回错误 */
	}
	/* 父进程打开文件了 子进程中共享这些打开的文件 */
	for (i=0; i<NR_OPEN;i++)/*  */
		if (f=p->filp[i])
			f->f_count++;/*  */
	if (current->pwd)/* 增加各个共享的文件的引用计数 */
		current->pwd->i_count++;/*  */
	if (current->root)/*  */
		current->root->i_count++;/*  */
	if (current->executable)
		current->executable->i_count++;/*  */
	if (current->library)/*  */
		current->library->i_count++;/*  */
	set_tss_desc(gdt+(nr<<1)+FIRST_TSS_ENTRY,&(p->tss));/* 设置任务的ldt到gdt中对应的项中 */
	set_ldt_desc(gdt+(nr<<1)+FIRST_LDT_ENTRY,&(p->ldt));/* 设置tss到gdt中对应的项中 */
	p->p_pptr = current;/* 父进程设置为当前调用fork的进程 */
	p->p_cptr = 0;
	p->p_ysptr = 0;/* 没有子进程和弟弟进程 自己是当前父进程的最年轻子进程 */
	p->p_osptr = current->p_cptr;/* 哥哥进程设置为当前进程的最年轻子进程  自己成为最年轻子进程 */
	if (p->p_osptr)/*  */
		p->p_osptr->p_ysptr = p;/* 把哥哥进程的弟弟进程指针指向p */
	current->p_cptr = p;/* fork出来的进程设置为父进程的最年轻子进程 */
	p->state = TASK_RUNNING;	/* do this last, just in case   最后将fork出来的进程设置为running*/
	return last_pid; /* 调用fork的进程 从这里返回的是子进程pid */
}

/*  */
int find_empty_process(void)
{
	int i;

	repeat:/*  */
		if ((++last_pid)<0) last_pid=1;/* 全局变量++ */
		for(i=0 ; i<NR_TASKS ; i++)/* 找到空闲的任务数组项 */
			if (task[i] && ((task[i]->pid == last_pid) ||/*  */
				        (task[i]->pgrp == last_pid)))/* pid不符合条件 重找 */
				goto repeat;/*  */
	for(i=1 ; i<NR_TASKS ; i++)/*  */
		if (!task[i])/*  */
			return i;/* 返回找到的数组index 作为任务号 */
	return -EAGAIN;/*  */
}
