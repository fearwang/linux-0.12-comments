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
/* д��֤ �������д ������ҳ�沢���� */
extern void write_verify(unsigned long address);

long last_pid=0;/* find_empty_process ���� */

/* ���̿ռ�����дǰ��֤ �����������Ϊ386���� ��Ȩ��0����д�û��ռ�ֻ��ҳ��  �����Ҫ�Լ����  ��û��ȱҳ�쳣���ں�̬ */
void verify_area(void * addr,int size)
{
	unsigned long start;

	start = (unsigned long) addr;/*  */
	size += start & 0xfff;/* ��ҳΪ��λ ���    ��size��չ�ɴ�����ҳ�����ַ��ʼ�ĳ���*/
	start &= 0xfffff000;/* ���䵽������ҳ�����ʼ��ַ */
	start += get_base(current->ldt[2]);/* �߼���ַ���϶λ���ַ������Ե�ַ */
	while (size>0) {/*  */
		size -= 4096;/*  */
		write_verify(start);/* ��֤һ��ҳ�� ѭ�� */
		start += 4096;/*  */
	}
}

/* ������ҳҳ�� ������ĵ�ַ�ռ������ô���ε�base��limit ������ҳ��*/
int copy_mem(int nr/* ������� */,struct task_struct * p/* ������task struct */)
{
	unsigned long old_data_base,new_data_base,data_limit;/*  */
	unsigned long old_code_base,new_code_base,code_limit;

	code_limit=get_limit(0x0f);/* ����ȡ��ǰ���̵�ldt�еĴ�������ݶ��޳� */
	data_limit=get_limit(0x17);/*  */
	old_code_base = get_base(current->ldt[1]);/* Ȼ��ȡ����κ����ݶλ���ַ */
	old_data_base = get_base(current->ldt[2]);/*  */
	if (old_data_base != old_code_base)/* 0.12Ҫ���������ݶ���ʼ��ַһ�� */
		panic("We don't support separate I&D");/*  */
	if (data_limit < code_limit)/* ��Ҫ�����ݶ�Ҫ���ڴ���� */
		panic("Bad data_limit");/*  */
	new_data_base = new_code_base = nr * TASK_SIZE;/* ��������ŵõ�����ַ */
	p->start_code = new_code_base;/* start code = �λ���ַ */
	set_base(p->ldt[1],new_code_base);/* ���þֲ����ݶκʹ���ε�base��ַ */
	set_base(p->ldt[2],new_data_base);/*  */
	if (copy_page_tables(old_data_base,new_data_base,data_limit)) {/* ����ҳ�� */
		free_page_tables(new_data_base,data_limit);/* ʧ�����ͷŷ����ҳ�� */
		return -ENOMEM;/*  */
	}/*  */
	return 0;/* �ɹ�����0 */
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 */
 
 /* fork����Ҫ�߼� ����ǻ��׶�ѹ��� */
/* �����￴ �ӽ��̱����Ⱥ� ֱ�Ӵ��û�̬��ʼ���У�data��stackһ������д�����ͻᡤ����дʱ����
	����һ�㣬sp��ʱ�Ѿ����������ˣ��͸�����һ����״̬

	ִ��exec��Ӧ�ûᱻ������λ
*/
int copy_process(int nr/* �ҵ��Ŀ�������� */,long ebp,long edi,long esi,long gs,long none,
		long ebx,long ecx,long edx, long orig_eax, 
		long fs,long es,long ds,
		long eip,long cs,long eflags,long esp,long ss)
{
	struct task_struct *p;/*  */
	int i;/*  */
	struct file *f;

	p = (struct task_struct *) get_free_page();/* ���ȷ���һҳ��task struct */
	if (!p)
		return -EAGAIN;/* û�з��䵽ҳ ������� */
	task[nr] = p;/* ��������ָ��ָ��task struct */
	*p = *current;	/* NOTE! this doesn't copy the supervisor stack ��ȫ����task struct*/
	p->state = TASK_UNINTERRUPTIBLE;/* ����fork����������Ϊ TASK_UNINTERRUPTIBLE*/
	p->pid = last_pid;/* �³̺� */
	p->counter = p->priority;/* ����ʱ��Ƭ */
	p->signal = 0;/* �ź�λͼ */
	p->alarm = 0;/* ���Ӷ�ʱ����0 */
	p->leader = 0;		/* process leadership doesn't inherit ���̵��쵼Ȩ���ܼ̳�*/
	p->utime = p->stime = 0;/* �û�̬���ں�̬����ʱ����0 */
	p->cutime = p->cstime = 0;/* �ӽ��̵��û�̬�ͺ���̬������ʱ�� */
	p->start_time = jiffies;/* ���̿�ʼ����ʱ�� */

	// ��������tss
	p->tss.back_link = 0;/*  */
	p->tss.esp0 = PAGE_SIZE + (long) p;/* �ں�ջָ��task struct����ҳ���� */
	p->tss.ss0 = 0x10;/* �ں����ݶ� */
	/* fork����ӽ��̣�������������ȵ�����Ȼ���û�̬��ʼ���� */
	p->tss.eip = eip;/* ���ص�ַ�͵���fork�ĸ�������һ���� */
	p->tss.eflags = eflags;/*  */
	p->tss.eax = 0;/* �ӽ���Ӧ�÷���0 */
	p->tss.ecx = ecx;/*  */
	p->tss.edx = edx;
	p->tss.ebx = ebx;
	p->tss.esp = esp; /* Ҳ���û�̬�Ķ�ջָ�� */
	p->tss.ebp = ebp;
	p->tss.esi = esi;
	p->tss.edi = edi;
	p->tss.es = es & 0xffff;/* �μĴ�����16bit  ����csֻ��һ������*/ 
	p->tss.cs = cs & 0xffff; /* fork����ӽ��̣�������������ȵ�����Ȼ���û�̬��ʼ���� */
	p->tss.ss = ss & 0xffff; 
	p->tss.ds = ds & 0xffff;
	p->tss.fs = fs & 0xffff;
	p->tss.gs = gs & 0xffff;
	p->tss.ldt = _LDT(nr);/* ����ldt�ֶ�ָ�� �ӽ��̵�ldt�������� */
	p->tss.trace_bitmap = 0x80000000;/*  */
	
	if (last_task_used_math == current)/* Э��������� */
		__asm__("clts ; fnsave %0 ; frstor %0"::"m" (p->tss.i387));
	/* ���濪ʼ���ƽ���ҳ�� */
	//ͬʱ���������ldt��base��limit
	if (copy_mem(nr,p)) {
		task[nr] = NULL;/* ʧ�����ͷ��������� */
		free_page((long) p);/* �ͷ�task structռ�õ�ҳ */
		return -EAGAIN;/* ���ش��� */
	}
	/* �����̴��ļ��� �ӽ����й�����Щ�򿪵��ļ� */
	for (i=0; i<NR_OPEN;i++)/*  */
		if (f=p->filp[i])
			f->f_count++;/*  */
	if (current->pwd)/* ���Ӹ���������ļ������ü��� */
		current->pwd->i_count++;/*  */
	if (current->root)/*  */
		current->root->i_count++;/*  */
	if (current->executable)
		current->executable->i_count++;/*  */
	if (current->library)/*  */
		current->library->i_count++;/*  */
	set_tss_desc(gdt+(nr<<1)+FIRST_TSS_ENTRY,&(p->tss));/* ���������ldt��gdt�ж�Ӧ������ */
	set_ldt_desc(gdt+(nr<<1)+FIRST_LDT_ENTRY,&(p->ldt));/* ����tss��gdt�ж�Ӧ������ */
	p->p_pptr = current;/* ����������Ϊ��ǰ����fork�Ľ��� */
	p->p_cptr = 0;
	p->p_ysptr = 0;/* û���ӽ��̺͵ܵܽ��� �Լ��ǵ�ǰ�����̵��������ӽ��� */
	p->p_osptr = current->p_cptr;/* ����������Ϊ��ǰ���̵��������ӽ���  �Լ���Ϊ�������ӽ��� */
	if (p->p_osptr)/*  */
		p->p_osptr->p_ysptr = p;/* �Ѹ����̵ĵܵܽ���ָ��ָ��p */
	current->p_cptr = p;/* fork�����Ľ�������Ϊ�����̵��������ӽ��� */
	p->state = TASK_RUNNING;	/* do this last, just in case   ���fork�����Ľ�������Ϊrunning*/
	return last_pid; /* ����fork�Ľ��� �����ﷵ�ص����ӽ���pid */
}

/*  */
int find_empty_process(void)
{
	int i;

	repeat:/*  */
		if ((++last_pid)<0) last_pid=1;/* ȫ�ֱ���++ */
		for(i=0 ; i<NR_TASKS ; i++)/* �ҵ����е����������� */
			if (task[i] && ((task[i]->pid == last_pid) ||/*  */
				        (task[i]->pgrp == last_pid)))/* pid���������� ���� */
				goto repeat;/*  */
	for(i=1 ; i<NR_TASKS ; i++)/*  */
		if (!task[i])/*  */
			return i;/* �����ҵ�������index ��Ϊ����� */
	return -EAGAIN;/*  */
}
