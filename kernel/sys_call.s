/*
 *  linux/kernel/system_call.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  system_call.s  contains the system-call low-level handling routines.
 * This also contains the timer-interrupt handler, as some of the code is
 * the same. The hd- and flopppy-interrupts are also here.
 *
 * NOTE: This code handles signal-recognition, which happens every time
 * after a timer-interrupt and after each system call. Ordinary interrupts
 * don't handle signal-recognition, as that would clutter them up totally
 * unnecessarily.
 *
 * Stack layout in 'ret_from_system_call':
 *
 *	 0(%esp) - %eax
 *	 4(%esp) - %ebx
 *	 8(%esp) - %ecx
 *	 C(%esp) - %edx
 *	10(%esp) - original %eax	(-1 if not system call)
 *	14(%esp) - %fs
 *	18(%esp) - %es
 *	1C(%esp) - %ds
 *	20(%esp) - %eip
 *	24(%esp) - %cs
 *	28(%esp) - %eflags
 *	2C(%esp) - %oldesp
 *	30(%esp) - %oldss
 */

SIG_CHLD	= 17

// ret_from_system_call时，各个寄存器在堆栈中的偏移值
EAX		= 0x00	// 系统调用返回值
EBX		= 0x04
ECX		= 0x08
EDX		= 0x0C
ORIG_EAX	= 0x10  //如果不是系统调用  是其他中断 该值是-1   否则是调用号
FS		= 0x14
ES		= 0x18
DS		= 0x1C
// cpu自动入栈
EIP		= 0x20
CS		= 0x24
EFLAGS		= 0x28
OLDESP		= 0x2C
OLDSS		= 0x30

// task struct中各个变量的offset
state	= 0		# these are offsets into the task-struct.
counter	= 4
priority = 8
signal	= 12
sigaction = 16		# MUST be 16 (=len of sigaction)
blocked = (33*16)

// sigaction成员变量的offset
# offsets within sigaction
sa_handler = 0
sa_mask = 4
sa_flags = 8
sa_restorer = 12

nr_system_calls = 82 // 一共82个系统调用
  
ENOSYS = 38  //系统调用号 出错码

/*
 * Ok, I get parallel printer interrupts while using the floppy for some
 * strange reason. Urgel. Now I just ignore them.
 */
.globl _system_call,_sys_fork,_timer_interrupt,_sys_execve
.globl _hd_interrupt,_floppy_interrupt,_parallel_interrupt
.globl _device_not_available, _coprocessor_error

.align 2
bad_sys_call:          //系统调用号出错时 返回-38
	pushl $-ENOSYS
	jmp ret_from_sys_call
.align 2
reschedule:
	pushl $ret_from_sys_call  //schedule返回时 从$ret_from_sys_call继续执行
	jmp _schedule
	
.align 2 //系统调用总入口
_system_call:   //cpu自动入栈 ss esp eflags  cs eip
	push %ds
	push %es
	push %fs
	pushl %eax		# save the orig_eax  即调用号
	pushl %edx		
	pushl %ecx		# push %ebx,%ecx,%edx as parameters
	pushl %ebx		# to the system call
	movl $0x10,%edx		# set up ds,es to kernel space
	mov %dx,%ds
	mov %dx,%es
	movl $0x17,%edx		# fs points to local data space
	mov %dx,%fs
	cmpl _NR_syscalls,%eax   //调用号错误
	jae bad_sys_call
	call _sys_call_table(,%eax,4)
	pushl %eax		//系统调用返回值入栈
2:
	//当进程不是可运行状态或者时间片用完了  就执行重新调度
	movl _current,%eax
	cmpl $0,state(%eax)		# state
	jne reschedule
	cmpl $0,counter(%eax)		# counter
	je reschedule 

	//reschedule返回后将从这里继续执行


	// ret from syscall的时候处理信号  处理syscall 时钟中断也会来到这里
ret_from_sys_call:
	movl _current,%eax		//eax保存当前任务号
	cmpl _task,%eax			# task[0] cannot have signals  任务0 则直接返回 不处理信号
	je 3f
	cmpw $0x0f,CS(%esp)		# was old code segment supervisor ? 是是不是从用户态走到这里  如果不是则是中断处理函数走到这里，直接退出 不处理信号
	jne 3f
	cmpw $0x17,OLDSS(%esp)		# was stack segment = 0x17 ?
	jne 3f
	movl signal(%eax),%ebx		//取得任务的信号位图
	movl blocked(%eax),%ecx		//取屏蔽位图
	notl %ecx			//屏蔽位图取反
	andl %ebx,%ecx			//&获得允许的位图
	bsfl %ecx,%ecx			//从低位开始扫描 看看是否有1的位
				//若有 则cx保留该位的偏移值
	je 3f				//若没有则退出
	btrl %ecx,%ebx			// 复位扫描到为1的bit
	movl %ebx,signal(%eax)			//更新任务的信号位图
	incl %ecx			//信号值调整为从1开始的值
	pushl %ecx			//信号值入栈
	call _do_signal			//调用信号处理函数
	popl %ecx		//弹出入栈的信号值
	testl %eax, %eax		//判断do_signal的返回值 不为0则跳转到2b  从2执行下来还会判断信号 
					//需要其他处理或任务调度
	jne 2b		# see if we need to switch tasks, or do more signals

	//从系统调用返回 
3:	popl %eax
	popl %ebx
	popl %ecx
	popl %edx
	addl $4, %esp	# skip orig_eax
	pop %fs
	pop %es
	pop %ds
	iret  //自动弹出cpu入栈的寄存器 

.align 2 //协处理器出错中断处理函数
_coprocessor_error:
	push %ds
	push %es
	push %fs
	pushl $-1		# fill in -1 for orig_eax  表明不是系统调用
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	movl $0x10,%eax  // ds es指向内核数据段
	mov %ax,%ds   
	mov %ax,%es
	movl $0x17,%eax  
	mov %ax,%fs // fs指向用户态数据段
	pushl $ret_from_sys_call  //返回地址入栈
	jmp _math_error

.align 2
_device_not_available:
	push %ds
	push %es
	push %fs
	pushl $-1		# fill in -1 for orig_eax
	pushl %edx
	pushl %ecx
	pushl %ebx		
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	pushl $ret_from_sys_call
	clts				# clear TS so that we can use math
	movl %cr0,%eax
	testl $0x4,%eax			# EM (math emulation bit)
	je _math_state_restore
	pushl %ebp
	pushl %esi
	pushl %edi
	pushl $0		# temporary storage for ORIG_EIP
	call _math_emulate
	addl $4,%esp
	popl %edi
	popl %esi
	popl %ebp
	ret

.align 2
//时钟中断处理函数
_timer_interrupt:
	push %ds		# save ds,es and put kernel data space
	push %es		# into them. %fs is used by _system_call
	push %fs
	pushl $-1		# fill in -1 for orig_eax
	pushl %edx		# we save %eax,%ecx,%edx as gcc doesn't
	pushl %ecx		# save those across function calls. %ebx
	pushl %ebx		# is saved as we use that in ret_sys_call  保存这些寄存器是我们自己要做的 gcc不帮我们做  后面ret_sys_call会使用这些寄存器
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs       // 设置好段寄存器  es ds内核段   fs->任务的局部数据段
	incl _jiffies			// 每个时钟中断中增加_jiffies
	movb $0x20,%al		# EOI to interrupt controller #1   操作8295a 结束中断
	outb %al,$0x20
	
	movl CS(%esp),%eax // 用当前特权级作为参数调用do timer  即执行系统调用的代码段的cpl 用于执行任务切换 计时等
	andl $3,%eax		# %eax is CPL (0 or 3, 0=supervisor)
	pushl %eax
	call _do_timer		# 'do_timer(long CPL)' does everything from
	addl $4,%esp		# task switching to accounting ...  //弹出do timer入参
	jmp ret_from_sys_call   # 所有处理逻辑都保证调用ret_from_sys_call的时候 堆栈上的布局是一致的


//sys_execve系统调用
.align 2
_sys_execve:
	lea EIP(%esp),%eax // eax指向堆栈中保存调用系统调用的代码地址eip指针处
	pushl %eax		//指针入栈 作为参数
	call _do_execve
	addl $4,%esp    //参数出栈
	ret

.align 2
_sys_fork:
	call _find_empty_process //为新进程取得进程号
	testl %eax,%eax		//eax中返回进程号 若返回负数则退出
	js 1f
	push %gs
	pushl %esi
	pushl %edi
	pushl %ebp
	pushl %eax
	call _copy_process
	addl $20,%esp //弹出入栈的参数
1:	ret

_hd_interrupt:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	movb $0x20,%al
	outb %al,$0xA0		# EOI to interrupt controller #1
	jmp 1f			# give port chance to breathe
1:	jmp 1f
1:	xorl %edx,%edx
	movl %edx,_hd_timeout
	xchgl _do_hd,%edx
	testl %edx,%edx
	jne 1f
	movl $_unexpected_hd_interrupt,%edx
1:	outb %al,$0x20
	call *%edx		# "interesting" way of handling intr.
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret

_floppy_interrupt:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	movb $0x20,%al
	outb %al,$0x20		# EOI to interrupt controller #1
	xorl %eax,%eax
	xchgl _do_floppy,%eax
	testl %eax,%eax
	jne 1f
	movl $_unexpected_floppy_interrupt,%eax
1:	call *%eax		# "interesting" way of handling intr.
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret

_parallel_interrupt:
	pushl %eax
	movb $0x20,%al
	outb %al,$0x20
	popl %eax
	iret
