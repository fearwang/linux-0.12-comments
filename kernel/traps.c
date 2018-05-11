/*
 *  linux/kernel/traps.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'Traps.c' handles hardware traps and faults after we have saved some
 * state in 'asm.s'. Currently mostly a debugging-aid, will be extended
 * to mainly kill the offending process (probably by giving it a signal,
 * but possibly by killing it outright if necessary).
 */
#include <string.h>

#include <linux/head.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/segment.h>
#include <asm/io.h>

/* 取段seg中addr地址处的一个字节 */
#define get_seg_byte(seg,addr) ({ \
register char __res; \
__asm__("push %%fs;mov %%ax,%%fs;movb %%fs:%2,%%al;pop %%fs" \
	:"=a" (__res):"0" (seg),"m" (*(addr))); \
__res;})

/* 取段seg中addr地址处的一个字长 long */
#define get_seg_long(seg,addr) ({ \
register unsigned long __res; \
__asm__("push %%fs;mov %%ax,%%fs;movl %%fs:%2,%%eax;pop %%fs" \
	:"=a" (__res):"0" (seg),"m" (*(addr))); \
__res;})

//取段寄存器fs的值
#define _fs() ({ \
register unsigned short __res; \
__asm__("mov %%fs,%%ax":"=a" (__res):); \
__res;})

//定义一些函数原型，实现标号在汇编代码中，对应的标号有一个_前缀   trap_init时候设置对应的门的入口
void page_exception(void);

void divide_error(void);
void debug(void);
void nmi(void);
void int3(void);
void overflow(void);
void bounds(void);
void invalid_op(void);
void device_not_available(void);
void double_fault(void);
void coprocessor_segment_overrun(void);
void invalid_TSS(void);
void segment_not_present(void);
void stack_segment(void);
void general_protection(void);
void page_fault(void);
void coprocessor_error(void);
void reserved(void);
void parallel_interrupt(void);
void irq13(void);
void alignment_check(void);

/* 打印出错中断的名称，出错号 调用程序的eip eflags esp fs段寄存器  段基地址 段长度 pid task_id 如果出错在用户态 还打印16字节堆栈内容 */
static void die(char * str /* 出错名称 */,long esp_ptr/* sp:指向eip的地址 */,long nr/* err code */)
{
	long * esp = (long *) esp_ptr;/*  */
	int i;

	printk("%s: %04x\n\r",str,nr&0xffff);/* 打印出错名称 和 出错号 */
	printk("EIP:\t%04x:%p\nEFLAGS:\t%p\nESP:\t%04x:%p\n",/* 打印出错时的 cs:eip  eflags ss:esp*/
		esp[1],esp[0],esp[2],esp[4],esp[3]);
	printk("fs: %04x\n",_fs());/* 打印fs段寄存器的值 */
	printk("base: %p, limit: %p\n",get_base(current->ldt[1]),get_limit(0x17));/* 打印任务的代码段基地址 和段限长 */
	if (esp[4] == 0x17) {/* 0x17 异常发生在用户态 */
		printk("Stack: ");
		for (i=0;i<4;i++)/* 还打印用户栈的16byte */
			printk("%p ",get_seg_long(0x17,i+(long *)esp[3]));/*  */
		printk("\n");
	}
	str(i);/* 取当前任务的任务号 */
	printk("Pid: %d, process nr: %d\n\r",current->pid,0xffff & i); /* 打印当前进程的pid和任务号 */
	for(i=0;i<10;i++)/* 打印10byte的指令码 */
		printk("%02x ",0xff & get_seg_byte(esp[1],(i+(char *)esp[0])));/*  */
	printk("\n\r");
	do_exit(11);		/* play segment exception */
}

void do_double_fault(long esp, long error_code)
{
	die("double fault",esp,error_code);/*  */
}

void do_general_protection(long esp, long error_code)
{
	die("general protection",esp,error_code);/*  */
}

void do_alignment_check(long esp, long error_code)
{
    die("alignment check",esp,error_code);/*  */
}

void do_divide_error(long esp, long error_code)
{
	die("divide error",esp,error_code);/*  */
}

//参数是asm.s中依次压栈的
void do_int3(long * esp, long error_code,
		long fs,long es,long ds,
		long ebp,long esi,long edi,
		long edx,long ecx,long ebx,long eax)
{
	int tr;

	__asm__("str %%ax":"=a" (tr):"0" (0));/* 去tr寄存器的值到tr变量 */
	printk("eax\t\tebx\t\tecx\t\tedx\n\r%8x\t%8x\t%8x\t%8x\n\r",
		eax,ebx,ecx,edx);
	printk("esi\t\tedi\t\tebp\t\tesp\n\r%8x\t%8x\t%8x\t%8x\n\r",/*  */
		esi,edi,ebp,(long) esp);
	printk("\n\rds\tes\tfs\ttr\n\r%4x\t%4x\t%4x\t%4x\n\r",
		ds,es,fs,tr);
	printk("EIP: %8x   CS: %4x  EFLAGS: %8x\n\r",esp[0],esp[1],esp[2]);/*  */
}

void do_nmi(long esp, long error_code)
{
	die("nmi",esp,error_code);/*  */
}

void do_debug(long esp, long error_code)
{
	die("debug",esp,error_code);/*  */
}

void do_overflow(long esp, long error_code)/*  */
{
	die("overflow",esp,error_code);
}

void do_bounds(long esp, long error_code)/*  */
{
	die("bounds",esp,error_code);/*  */
}

void do_invalid_op(long esp, long error_code)
{
	die("invalid operand",esp,error_code);/*  */
}

void do_device_not_available(long esp, long error_code)
{
	die("device not available",esp,error_code);/*  */
}

void do_coprocessor_segment_overrun(long esp, long error_code)
{
	die("coprocessor segment overrun",esp,error_code);/*  */
}

void do_invalid_TSS(long esp,long error_code)
{
	die("invalid TSS",esp,error_code);/*  */
}

void do_segment_not_present(long esp,long error_code)
{
	die("segment not present",esp,error_code);/*  */
}

void do_stack_segment(long esp,long error_code)
{
	die("stack segment",esp,error_code);/*  */
}

void do_coprocessor_error(long esp, long error_code)
{
	if (last_task_used_math != current)
		return;
	die("coprocessor error",esp,error_code);/*  */
}

void do_reserved(long esp, long error_code)
{
	die("reserved (15,17-47) error",esp,error_code);/*  */
}

void trap_init(void)
{
	int i;
//设置大部分异常的处理函数，其他特殊的由具体硬件初始化时设置
	set_trap_gate(0,&divide_error); /* 这些函数实际都在asm,s中 */
	set_trap_gate(1,&debug);
	set_trap_gate(2,&nmi);
	set_system_gate(3,&int3);	/* int3-5 can be called from all */
	set_system_gate(4,&overflow);
	set_system_gate(5,&bounds);
	set_trap_gate(6,&invalid_op);
	set_trap_gate(7,&device_not_available);
	set_trap_gate(8,&double_fault);
	set_trap_gate(9,&coprocessor_segment_overrun);
	set_trap_gate(10,&invalid_TSS);
	set_trap_gate(11,&segment_not_present);
	set_trap_gate(12,&stack_segment);
	set_trap_gate(13,&general_protection);
	//缺页中断
	set_trap_gate(14,&page_fault);
	set_trap_gate(15,&reserved);
	set_trap_gate(16,&coprocessor_error);
	set_trap_gate(17,&alignment_check);
	for (i=18;i<48;i++)
		set_trap_gate(i,&reserved);
	set_trap_gate(45,&irq13);
	outb_p(inb_p(0x21)&0xfb,0x21);
	outb(inb_p(0xA1)&0xdf,0xA1);
	set_trap_gate(39,&parallel_interrupt);
}
