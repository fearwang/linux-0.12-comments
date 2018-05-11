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

// ret_from_system_callʱ�������Ĵ����ڶ�ջ�е�ƫ��ֵ
EAX		= 0x00	// ϵͳ���÷���ֵ
EBX		= 0x04
ECX		= 0x08
EDX		= 0x0C
ORIG_EAX	= 0x10  //�������ϵͳ����  �������ж� ��ֵ��-1   �����ǵ��ú�
FS		= 0x14
ES		= 0x18
DS		= 0x1C
// cpu�Զ���ջ
EIP		= 0x20
CS		= 0x24
EFLAGS		= 0x28
OLDESP		= 0x2C
OLDSS		= 0x30

// task struct�и���������offset
state	= 0		# these are offsets into the task-struct.
counter	= 4
priority = 8
signal	= 12
sigaction = 16		# MUST be 16 (=len of sigaction)
blocked = (33*16)

// sigaction��Ա������offset
# offsets within sigaction
sa_handler = 0
sa_mask = 4
sa_flags = 8
sa_restorer = 12

nr_system_calls = 82 // һ��82��ϵͳ����
  
ENOSYS = 38  //ϵͳ���ú� ������

/*
 * Ok, I get parallel printer interrupts while using the floppy for some
 * strange reason. Urgel. Now I just ignore them.
 */
.globl _system_call,_sys_fork,_timer_interrupt,_sys_execve
.globl _hd_interrupt,_floppy_interrupt,_parallel_interrupt
.globl _device_not_available, _coprocessor_error

.align 2
bad_sys_call:          //ϵͳ���úų���ʱ ����-38
	pushl $-ENOSYS
	jmp ret_from_sys_call
.align 2
reschedule:
	pushl $ret_from_sys_call  //schedule����ʱ ��$ret_from_sys_call����ִ��
	jmp _schedule
	
.align 2 //ϵͳ���������
_system_call:   //cpu�Զ���ջ ss esp eflags  cs eip
	push %ds
	push %es
	push %fs
	pushl %eax		# save the orig_eax  �����ú�
	pushl %edx		
	pushl %ecx		# push %ebx,%ecx,%edx as parameters
	pushl %ebx		# to the system call
	movl $0x10,%edx		# set up ds,es to kernel space
	mov %dx,%ds
	mov %dx,%es
	movl $0x17,%edx		# fs points to local data space
	mov %dx,%fs
	cmpl _NR_syscalls,%eax   //���úŴ���
	jae bad_sys_call
	call _sys_call_table(,%eax,4)
	pushl %eax		//ϵͳ���÷���ֵ��ջ
2:
	//�����̲��ǿ�����״̬����ʱ��Ƭ������  ��ִ�����µ���
	movl _current,%eax
	cmpl $0,state(%eax)		# state
	jne reschedule
	cmpl $0,counter(%eax)		# counter
	je reschedule 

	//reschedule���غ󽫴��������ִ��


	// ret from syscall��ʱ�����ź�  ����syscall ʱ���ж�Ҳ����������
ret_from_sys_call:
	movl _current,%eax		//eax���浱ǰ�����
	cmpl _task,%eax			# task[0] cannot have signals  ����0 ��ֱ�ӷ��� �������ź�
	je 3f
	cmpw $0x0f,CS(%esp)		# was old code segment supervisor ? ���ǲ��Ǵ��û�̬�ߵ�����  ������������жϴ������ߵ����ֱ���˳� �������ź�
	jne 3f
	cmpw $0x17,OLDSS(%esp)		# was stack segment = 0x17 ?
	jne 3f
	movl signal(%eax),%ebx		//ȡ��������ź�λͼ
	movl blocked(%eax),%ecx		//ȡ����λͼ
	notl %ecx			//����λͼȡ��
	andl %ebx,%ecx			//&��������λͼ
	bsfl %ecx,%ecx			//�ӵ�λ��ʼɨ�� �����Ƿ���1��λ
				//���� ��cx������λ��ƫ��ֵ
	je 3f				//��û�����˳�
	btrl %ecx,%ebx			// ��λɨ�赽Ϊ1��bit
	movl %ebx,signal(%eax)			//����������ź�λͼ
	incl %ecx			//�ź�ֵ����Ϊ��1��ʼ��ֵ
	pushl %ecx			//�ź�ֵ��ջ
	call _do_signal			//�����źŴ�����
	popl %ecx		//������ջ���ź�ֵ
	testl %eax, %eax		//�ж�do_signal�ķ���ֵ ��Ϊ0����ת��2b  ��2ִ�����������ж��ź� 
					//��Ҫ����������������
	jne 2b		# see if we need to switch tasks, or do more signals

	//��ϵͳ���÷��� 
3:	popl %eax
	popl %ebx
	popl %ecx
	popl %edx
	addl $4, %esp	# skip orig_eax
	pop %fs
	pop %es
	pop %ds
	iret  //�Զ�����cpu��ջ�ļĴ��� 

.align 2 //Э�����������жϴ�����
_coprocessor_error:
	push %ds
	push %es
	push %fs
	pushl $-1		# fill in -1 for orig_eax  ��������ϵͳ����
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	movl $0x10,%eax  // ds esָ���ں����ݶ�
	mov %ax,%ds   
	mov %ax,%es
	movl $0x17,%eax  
	mov %ax,%fs // fsָ���û�̬���ݶ�
	pushl $ret_from_sys_call  //���ص�ַ��ջ
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
//ʱ���жϴ�����
_timer_interrupt:
	push %ds		# save ds,es and put kernel data space
	push %es		# into them. %fs is used by _system_call
	push %fs
	pushl $-1		# fill in -1 for orig_eax
	pushl %edx		# we save %eax,%ecx,%edx as gcc doesn't
	pushl %ecx		# save those across function calls. %ebx
	pushl %ebx		# is saved as we use that in ret_sys_call  ������Щ�Ĵ����������Լ�Ҫ���� gcc����������  ����ret_sys_call��ʹ����Щ�Ĵ���
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs       // ���úöμĴ���  es ds�ں˶�   fs->����ľֲ����ݶ�
	incl _jiffies			// ÿ��ʱ���ж�������_jiffies
	movb $0x20,%al		# EOI to interrupt controller #1   ����8295a �����ж�
	outb %al,$0x20
	
	movl CS(%esp),%eax // �õ�ǰ��Ȩ����Ϊ��������do timer  ��ִ��ϵͳ���õĴ���ε�cpl ����ִ�������л� ��ʱ��
	andl $3,%eax		# %eax is CPL (0 or 3, 0=supervisor)
	pushl %eax
	call _do_timer		# 'do_timer(long CPL)' does everything from
	addl $4,%esp		# task switching to accounting ...  //����do timer���
	jmp ret_from_sys_call   # ���д����߼�����֤����ret_from_sys_call��ʱ�� ��ջ�ϵĲ�����һ�µ�


//sys_execveϵͳ����
.align 2
_sys_execve:
	lea EIP(%esp),%eax // eaxָ���ջ�б������ϵͳ���õĴ����ַeipָ�봦
	pushl %eax		//ָ����ջ ��Ϊ����
	call _do_execve
	addl $4,%esp    //������ջ
	ret

.align 2
_sys_fork:
	call _find_empty_process //Ϊ�½���ȡ�ý��̺�
	testl %eax,%eax		//eax�з��ؽ��̺� �����ظ������˳�
	js 1f
	push %gs
	pushl %esi
	pushl %edi
	pushl %ebp
	pushl %eax
	call _copy_process
	addl $20,%esp //������ջ�Ĳ���
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
