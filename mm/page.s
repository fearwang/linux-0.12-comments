/*
 *  linux/mm/page.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * page.s contains the low-level page-exception code.
 * the real work is done in mm.c
 */

//È±Ò³ÖĞ¶Ï£¬int14

¶ÑÕ»Çé¿ö
/*
  Ô­SS
  Ô­ESP
  EFLAGS
  CS
  EIP
  ERROR_CODE	
*/

.globl _page_fault

_page_fault:
	xchgl %eax,(%esp) //È¡³öÕ»¶¥µÄ´íÎóÂë£
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%edx //ÉèÖÃÄÚºËÊı¾İ¶ÎÑ¡Ôñ·û
	mov %dx,%ds
	mov %dx,%es
	mov %dx,%fs
	movl %cr2,%edx  //»ñµÃÒì³£µÄÏßĞÔµØÖ·
	pushl %edx      //³ö´íÂë ºÍ ÏßĞÔµØÖ·Ñ¹Õ»
	pushl %eax
	testl $1,%eax   //ÅĞ¶ÏPÎ»ÊÇ·ñÎª1
	jne 1f			
	call _do_no_page   //È±Ò³
	jmp 2f
1:	call _do_wp_page    //Ğ´±£»¤Òì³£
2:	addl $8,%esp
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret
