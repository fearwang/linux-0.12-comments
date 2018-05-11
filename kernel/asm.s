/*
 *  linux/kernel/asm.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * asm.s contains the low-level code for most hardware faults.
 * page_exception is handled by the mm, so that isn't here. This
 * file also handles (hopefully) fpu-exceptions due to TS-bit, as
 * the fpu must be properly saved/resored. This hasn't been tested.
 */

// 全局声明  c文件中实现的函数
.globl _divide_error,_debug,_nmi,_int3,_overflow,_bounds,_invalid_op
.globl _double_fault,_coprocessor_segment_overrun
.globl _invalid_TSS,_segment_not_present,_stack_segment
.globl _general_protection,_coprocessor_error,_irq13,_reserved
.globl _alignment_check

_divide_error:
	pushl $_do_divide_error   //首先压栈后面要调用的c函数地址
no_error_code:                    // 处理无出错号的情况
	xchgl %eax,(%esp)         // c函数地址放到eax中，原eax值放到堆栈中c函数地址所占用的内存中
	pushl %ebx
	pushl %ecx
	pushl %edx
	pushl %edi
	pushl %esi
	pushl %ebp 
	push %ds
	push %es
	push %fs                  // 压栈通用寄存器和段寄存器
	pushl $0		# "error code"   //无出错码  压入0
	lea 44(%esp),%edx          // 将调用调用_divide_error(或其他异常处理)时sp的值取出来，放到edx中，我们压栈11次 所以加上44 
					//是cpu自动保存的eip的地址
	pushl %edx			// edx压栈
	movl $0x10,%edx         //0x10是内核数据段选择符，初始化下面的ds es fs
	mov %dx,%ds
	mov %dx,%es
	mov %dx,%fs
	call *%eax         //eax中保存的是即将调用的c函数，调用它
	addl $8,%esp		//返回后跳过两次压栈的值，分别是sp和出错码，是c函数的入参
	pop %fs
	pop %es
	pop %ds
	popl %ebp
	popl %esi
	popl %edi
	popl %edx
	popl %ecx
	popl %ebx
	popl %eax		//出栈
	iret

_debug: //调试中断
	pushl $_do_int3		# _do_debug  //和其他无出错码的异常处理一样的流程  他们的c函数原型也应该是一致的  
	jmp no_error_code			// 至少前2个参数是一样的  

_nmi://非屏蔽中断
	pushl $_do_nmi
	jmp no_error_code

_int3://断点指令引起
	pushl $_do_int3
	jmp no_error_code

_overflow://溢出出错处理
	pushl $_do_overflow
	jmp no_error_code

_bounds://边界检查出错
	pushl $_do_bounds
	jmp no_error_code

_invalid_op://无效操作指令
	pushl $_do_invalid_op
	jmp no_error_code

_coprocessor_segment_overrun://
	pushl $_do_coprocessor_segment_overrun
	jmp no_error_code

_reserved://
	pushl $_do_reserved
	jmp no_error_code

_irq13://linux设置的数学协处理器硬件中断
	pushl %eax
	xorb %al,%al
	outb %al,$0xF0
	movb $0x20,%al
	outb %al,$0x20
	jmp 1f
1:	jmp 1f
1:	outb %al,$0xA0
	popl %eax
	jmp _coprocessor_error

//下面是带有出错码的异常处理  x86的sp指向的是栈顶元素 而不是栈顶元素的下一个空位置

_double_fault:
	pushl $_do_double_fault		//入栈c函数地址
error_code:
	xchgl %eax,4(%esp)		# error code <-> %eax   esp+4是保存的出错码  和eax交换
	xchgl %ebx,(%esp)		# &function <-> %ebx	esp保存的是c函数地址 和ebx交换
	pushl %ecx
	pushl %edx
	pushl %edi
	pushl %esi
	pushl %ebp
	push %ds
	push %es
	push %fs
	pushl %eax			# error code 出错号入栈
	lea 44(%esp),%eax		# offset	// 
	pushl %eax					// esp入栈   是cpu自动保存的eip的地址
	movl $0x10,%eax		//选择内核数据段
	mov %ax,%ds
	mov %ax,%es
	mov %ax,%fs
	call *%ebx
	addl $8,%esp
	pop %fs
	pop %es
	pop %ds
	popl %ebp
	popl %esi
	popl %edi
	popl %edx
	popl %ecx
	popl %ebx
	popl %eax
	iret

_invalid_TSS:
	pushl $_do_invalid_TSS
	jmp error_code

_segment_not_present:
	pushl $_do_segment_not_present
	jmp error_code

_stack_segment:
	pushl $_do_stack_segment
	jmp error_code

_general_protection:
	pushl $_do_general_protection
	jmp error_code

_alignment_check:
	pushl $_do_alignment_check
	jmp error_code

