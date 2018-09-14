/*
 * This file contains the procedures for the handling of select
 *
 * Created for Linux based loosely upon Mathius Lattner's minix
 * patches by Peter MacDonald. Heavily edited by Linus.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <linux/sched.h>

#include <asm/segment.h>
#include <asm/system.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <const.h>
#include <errno.h>
#include <sys/time.h>
#include <signal.h>

/*
 * Ok, Peter made a complicated, but straightforward multiple_wait() function.
 * I have rewritten this, taking some shortcuts: This code may not be easy to
 * follow, but it should be free of race-conditions, and it's practical. If you
 * understand what I'm doing here, then you understand how the linux sleep/wakeup
 * mechanism works.
 *
 * Two very simple procedures, add_wait() and free_wait() make all the work. We
 * have to have interrupts disabled throughout the select, but that's not really
 * such a loss: sleeping automatically frees interrupts when we aren't in this
 * task.
 */

typedef struct {
	struct task_struct * old_task;
	struct task_struct ** wait_address;
} wait_entry;

typedef struct {
	int nr;
	wait_entry entry[NR_OPEN*3];
} select_table;

static void add_wait(struct task_struct ** wait_address, select_table * p)
{
	int i;

	if (!wait_address)
		return;
	for (i = 0 ; i < p->nr ; i++)
		if (p->entry[i].wait_address == wait_address)
			return;
		//entry指向队列头
	p->entry[p->nr].wait_address = wait_address;
		//oldtask指向当前队列上的头
	p->entry[p->nr].old_task = * wait_address;
	//等待队列头指向当前指针  即当前进程成为了头
	*wait_address = current;
	p->nr++;
}

static void free_wait(select_table * p)
{
	int i;
	struct task_struct ** tpp;
	//循环处理所有有效等待队列
	for (i = 0; i < p->nr ; i++) {
		//tpp是指向队列头
		tpp = p->entry[i].wait_address;
		//队列不空 且 头不是自己 则说明 在自己睡眠期间有其他任务进入队列
		while (*tpp && *tpp != current) {
			//则设置头进程 running
			(*tpp)->state = 0;
			//设置自己d状态
			current->state = TASK_UNINTERRUPTIBLE;
			//出发调度
			schedule();
		}
		//走到这里说明 自己是头了
		if (!*tpp)
			printk("free_wait: NULL");
		//让队列头指针指向自己的后一个任务
		if (*tpp = p->entry[i].old_task)
			//设置自己runing
			//这里有必要每次循环都设置自己running吗 因为自己可能会在多个队列上等待
			(**tpp).state = 0;
	}
	p->nr = 0;
}

static struct tty_struct * get_tty(struct m_inode * inode)
{
	int major, minor;

	if (!S_ISCHR(inode->i_mode))
		return NULL;
	if ((major = MAJOR(inode->i_zone[0])) != 5 && major != 4)
		return NULL;
	if (major == 5)
		minor = current->tty;
	else
		minor = MINOR(inode->i_zone[0]);
	if (minor < 0)
		return NULL;
	return TTY_TABLE(minor);
}

/*
 * The check_XX functions check out a file. We know it's either
 * a pipe, a character device or a fifo (fifo's not implemented)
 */
static int check_in(select_table * wait, struct m_inode * inode)
{
	struct tty_struct * tty;

	if (tty = get_tty(inode))
		if (!EMPTY(tty->secondary))
			return 1;
		else
			add_wait(&tty->secondary->proc_list, wait);
	else if (inode->i_pipe)
		if (!PIPE_EMPTY(*inode))
			return 1;
		else
			add_wait(&inode->i_wait, wait);
	return 0;
}

static int check_out(select_table * wait, struct m_inode * inode)
{
	struct tty_struct * tty;

	if (tty = get_tty(inode))
		if (!FULL(tty->write_q))
			return 1;
		else
			add_wait(&tty->write_q->proc_list, wait);
	else if (inode->i_pipe)
		if (!PIPE_FULL(*inode))
			return 1;
		else
			add_wait(&inode->i_wait, wait);
	return 0;
}

static int check_ex(select_table * wait, struct m_inode * inode)
{
	struct tty_struct * tty;

	if (tty = get_tty(inode))
		if (!FULL(tty->write_q))
			return 0;
		else
			return 0;
	else if (inode->i_pipe)
		if (inode->i_count < 2)
			return 1;
		else
			add_wait(&inode->i_wait,wait);
	return 0;
}

int do_select(fd_set in, fd_set out, fd_set ex,
	fd_set *inp, fd_set *outp, fd_set *exp)
{
	int count;
	//等待表table
	select_table wait_table;
	int i;
	fd_set mask;

	mask = in | out | ex;
	//对传进来的fd判断有效性
	for (i = 0 ; i < NR_OPEN ; i++,mask >>= 1) {
		if (!(mask & 1))
			continue;
		if (!current->filp[i])
			return -EBADF;
		if (!current->filp[i]->f_inode)
			return -EBADF;
		if (current->filp[i]->f_inode->i_pipe)
			continue;
		if (S_ISCHR(current->filp[i]->f_inode->i_mode))
			continue;
		if (S_ISFIFO(current->filp[i]->f_inode->i_mode))
			continue;
		return -EBADF;
	}
repeat:
	wait_table.nr = 0;
	*inp = *outp = *exp = 0;
	count = 0;
	mask = 1;
	//循环检查 如果准备好了 则置对应bit count++
	for (i = 0 ; i < NR_OPEN ; i++, mask += mask) {
		if (mask & in)
			if (check_in(&wait_table,current->filp[i]->f_inode)) {
				*inp |= mask;
				count++;
			}
		if (mask & out)
			if (check_out(&wait_table,current->filp[i]->f_inode)) {
				*outp |= mask;
				count++;
			}
		if (mask & ex)
			if (check_ex(&wait_table,current->filp[i]->f_inode)) {
				*exp |= mask;
				count++;
			}
	}
	//没有描述符准备好 未超时 受到非阻塞信号
	if (!(current->signal & ~current->blocked) &&
	    (wait_table.nr || current->timeout) && !count) {
		current->state = TASK_INTERRUPTIBLE;
		//则切换出去
		schedule();
		//回来的时候唤醒等待队列上等待table上的本任务前后的任务
		free_wait(&wait_table);
		goto repeat;
	}

	//走到这里 说明count不为0 或者接收到信号 那么唤醒在等待队列上的任务()
	//可能部分fd没有准备好 所以任务已经在部分队列上等待了
	free_wait(&wait_table);
	return count;
}

/*
 * Note that we cannot return -ERESTARTSYS, as we change our input
 * parameters. Sad, but there you are. We could do some tweaking in
 * the library function ...
 */
int sys_select( unsigned long *buffer )
{
/* Perform the select(nd, in, out, ex, tv) system call. */
	int i;
	fd_set res_in, in = 0, *inp;
	fd_set res_out, out = 0, *outp;
	fd_set res_ex, ex = 0, *exp;
	fd_set mask;
	struct timeval *tvp;
	unsigned long timeout;

	mask = ~((~0) << get_fs_long(buffer++));
	inp = (fd_set *) get_fs_long(buffer++);
	outp = (fd_set *) get_fs_long(buffer++);
	exp = (fd_set *) get_fs_long(buffer++);
	tvp = (struct timeval *) get_fs_long(buffer);

	if (inp)
		in = mask & get_fs_long(inp);
	if (outp)
		out = mask & get_fs_long(outp);
	if (exp)
		ex = mask & get_fs_long(exp);
	timeout = 0xffffffff;
	if (tvp) {
		//设置timeout
		timeout = get_fs_long((unsigned long *)&tvp->tv_usec)/(1000000/HZ);
		timeout += get_fs_long((unsigned long *)&tvp->tv_sec) * HZ;
		timeout += jiffies;
	}
	current->timeout = timeout;
	//关中断
	cli();
	i = do_select(in, out, ex, &res_in, &res_out, &res_ex);
	if (current->timeout > jiffies)
		timeout = current->timeout - jiffies;
	else
		timeout = 0;
	sti();
	current->timeout = 0;
	if (i < 0)
		return i;
	if (inp) {
		verify_area(inp, 4);
		put_fs_long(res_in,inp);
	}
	if (outp) {
		verify_area(outp,4);
		put_fs_long(res_out,outp);
	}
	if (exp) {
		verify_area(exp,4);
		put_fs_long(res_ex,exp);
	}
	if (tvp) {
		verify_area(tvp, sizeof(*tvp));
		put_fs_long(timeout/HZ, (unsigned long *) &tvp->tv_sec);
		timeout %= HZ;
		timeout *= (1000000/HZ);
		put_fs_long(timeout, (unsigned long *) &tvp->tv_usec);
	}
	//没有描述符准备好 且受到了某个费阻塞信号 则返回ENITR
	if (!i && (current->signal & ~current->blocked))
		return -EINTR;
	return i;
}
