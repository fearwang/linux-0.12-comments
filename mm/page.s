/*
 *  linux/mm/page.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * page.s contains the low-level page-exception code.
 * the real work is done in mm.c
 */

//ȱҳ�жϣ�int14

��ջ���
/*
  ԭSS
  ԭESP
  EFLAGS
  CS
  EIP
  ERROR_CODE	
*/

.globl _page_fault

_page_fault:
	xchgl %eax,(%esp) //ȡ��ջ���Ĵ�����
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%edx //�����ں����ݶ�ѡ���
	mov %dx,%ds
	mov %dx,%es
	mov %dx,%fs
	movl %cr2,%edx  //����쳣�����Ե�ַ
	pushl %edx      //������ �� ���Ե�ַѹջ
	pushl %eax
	testl $1,%eax   //�ж�Pλ�Ƿ�Ϊ1
	jne 1f			
	call _do_no_page   //ȱҳ
	jmp 2f
1:	call _do_wp_page    //д�����쳣
2:	addl $8,%esp
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret
