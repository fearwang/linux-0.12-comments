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

// ȫ������  c�ļ���ʵ�ֵĺ���
.globl _divide_error,_debug,_nmi,_int3,_overflow,_bounds,_invalid_op
.globl _double_fault,_coprocessor_segment_overrun
.globl _invalid_TSS,_segment_not_present,_stack_segment
.globl _general_protection,_coprocessor_error,_irq13,_reserved
.globl _alignment_check

_divide_error:
	pushl $_do_divide_error   //����ѹջ����Ҫ���õ�c������ַ
no_error_code:                    // �����޳���ŵ����
	xchgl %eax,(%esp)         // c������ַ�ŵ�eax�У�ԭeaxֵ�ŵ���ջ��c������ַ��ռ�õ��ڴ���
	pushl %ebx
	pushl %ecx
	pushl %edx
	pushl %edi
	pushl %esi
	pushl %ebp 
	push %ds
	push %es
	push %fs                  // ѹջͨ�üĴ����ͶμĴ���
	pushl $0		# "error code"   //�޳�����  ѹ��0
	lea 44(%esp),%edx          // �����õ���_divide_error(�������쳣����)ʱsp��ֵȡ�������ŵ�edx�У�����ѹջ11�� ���Լ���44 
					//��cpu�Զ������eip�ĵ�ַ
	pushl %edx			// edxѹջ
	movl $0x10,%edx         //0x10���ں����ݶ�ѡ�������ʼ�������ds es fs
	mov %dx,%ds
	mov %dx,%es
	mov %dx,%fs
	call *%eax         //eax�б�����Ǽ������õ�c������������
	addl $8,%esp		//���غ���������ѹջ��ֵ���ֱ���sp�ͳ����룬��c���������
	pop %fs
	pop %es
	pop %ds
	popl %ebp
	popl %esi
	popl %edi
	popl %edx
	popl %ecx
	popl %ebx
	popl %eax		//��ջ
	iret

_debug: //�����ж�
	pushl $_do_int3		# _do_debug  //�������޳�������쳣����һ��������  ���ǵ�c����ԭ��ҲӦ����һ�µ�  
	jmp no_error_code			// ����ǰ2��������һ����  

_nmi://�������ж�
	pushl $_do_nmi
	jmp no_error_code

_int3://�ϵ�ָ������
	pushl $_do_int3
	jmp no_error_code

_overflow://���������
	pushl $_do_overflow
	jmp no_error_code

_bounds://�߽������
	pushl $_do_bounds
	jmp no_error_code

_invalid_op://��Ч����ָ��
	pushl $_do_invalid_op
	jmp no_error_code

_coprocessor_segment_overrun://
	pushl $_do_coprocessor_segment_overrun
	jmp no_error_code

_reserved://
	pushl $_do_reserved
	jmp no_error_code

_irq13://linux���õ���ѧЭ������Ӳ���ж�
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

//�����Ǵ��г�������쳣����  x86��spָ�����ջ��Ԫ�� ������ջ��Ԫ�ص���һ����λ��

_double_fault:
	pushl $_do_double_fault		//��ջc������ַ
error_code:
	xchgl %eax,4(%esp)		# error code <-> %eax   esp+4�Ǳ���ĳ�����  ��eax����
	xchgl %ebx,(%esp)		# &function <-> %ebx	esp�������c������ַ ��ebx����
	pushl %ecx
	pushl %edx
	pushl %edi
	pushl %esi
	pushl %ebp
	push %ds
	push %es
	push %fs
	pushl %eax			# error code �������ջ
	lea 44(%esp),%eax		# offset	// 
	pushl %eax					// esp��ջ   ��cpu�Զ������eip�ĵ�ַ
	movl $0x10,%eax		//ѡ���ں����ݶ�
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

