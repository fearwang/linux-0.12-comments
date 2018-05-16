/*
 *  linux/kernel/panic.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * This function is used through-out the kernel (includeinh mm and fs)
 * to indicate a major problem.
 */
#include <linux/kernel.h>
#include <linux/sched.h>

void sys_sync(void);	/* it's really int */
/* 显示内核出现重大错误 并运行文件系统同步函数 */
volatile void panic(const char * s)
{
	printk("Kernel panic: %s\n\r",s);/*  */
	if (current == task[0])/* 说明交换任务出错  不执行sync */
		printk("In swapper task - not syncing\n\r");
	else
		sys_sync();/* 否则死循环之前sync */
	for(;;); /* 现在内核不支持抢占 因此在内核态除非主动调用schedule 否则将永远在这里循环 */
			/* 即使来了中断 处理完中断还是会回到这里 */
}
