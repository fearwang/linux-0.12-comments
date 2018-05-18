/*
 *  linux/kernel/exit.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define DEBUG_PROC_TREE

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <asm/segment.h>

int sys_pause(void);
int sys_close(int fd);

/* 释放指定进程的task_struct占用的页面和任务槽  在此之前应该已经释放了进程的地址空间对应的页面 */
void release(struct task_struct * p)
{
	int i;

	if (!p)
		return;
	if (p == current) {/* 不能释放自己 */
		printk("task releasing itself\n\r");
		return;
	}
	for (i=1 ; i<NR_TASKS ; i++)/* 循环任务槽数组 */
		if (task[i]==p) {/* 找到匹配的任务 */
			task[i]=NULL;/* 任务槽置为null */
			/* Update links 更新任务树状连接关系 */
			if (p->p_osptr)/* 如果要释放的进程有哥哥进程 */
				p->p_osptr->p_ysptr = p->p_ysptr;/* 则让其哥哥进程的弟弟进程指针指向要释放的进程的弟弟进程 */
			if (p->p_ysptr)
				p->p_ysptr->p_osptr = p->p_osptr;/* 如果要释放的进程有弟弟进程 则让其弟弟进程的哥哥进程指针指向要释放的进程的哥哥进程(可能是null) */
			else
				p->p_pptr->p_cptr = p->p_osptr;/* 如果要释放的进程是最新的子进程 则将要释放的进程的父进程的孩子指针更新为要释放的进程的哥哥进程 */
			free_page((long)p);/* 入参是物理地址  这里利用了内核空间物理地址和虚拟地址一一映射 */
			schedule();/* 出发任务调度  任务调度里面不可能找到这个要释放的进程了 因此p代表的进程 到此终止*/
			return;
		}
	panic("trying to release non-existent task");
}

#ifdef DEBUG_PROC_TREE
/*
 * Check to see if a task_struct pointer is present in the task[] array
 * Return 0 if found, and 1 if not found.
 */
int bad_task_ptr(struct task_struct *p)/* 检查任务数组中是否包含p指向的task struct，存在返回0  否则返回1 */
{
	int 	i;

	if (!p)/*  */
		return 0;
	for (i=0 ; i<NR_TASKS ; i++)/*  */
		if (task[i] == p)/*  */
			return 0;
	return 1;/*  */
}
	
/*
 * This routine scans the pid tree and make sure the rep invarient still
 * holds.  Used for debugging only, since it's very slow....
 *
 * It looks a lot scarier than it really is.... we're doing 鎛othing more
 * than verifying the doubly-linked list found鎖n p_ysptr and p_osptr, 
 * and checking it corresponds with the process tree defined by p_cptr and 
 * p_pptr;
 */
void audit_ptree()/* 检查进程树 确定进程之间组成的连接关系正确 */
{
	int	i;

	for (i=1 ; i<NR_TASKS ; i++) {/* 循环处理任务数组 */
		if (!task[i])/* 跳过空闲项 */
			continue;

		/* 下面依次检查父进程，最年轻的子进程，自己的哥哥进程 弟弟进程 是否存在于任务数组 */
		if (bad_task_ptr(task[i]->p_pptr))/*  */
			printk("Warning, pid %d's parent link is bad\n",
				task[i]->pid);
		if (bad_task_ptr(task[i]->p_cptr))/*  */
			printk("Warning, pid %d's child link is bad\n",
				task[i]->pid);
		if (bad_task_ptr(task[i]->p_ysptr))/*  */
			printk("Warning, pid %d's ys link is bad\n",
				task[i]->pid);
		if (bad_task_ptr(task[i]->p_osptr))/*  */
			printk("Warning, pid %d's os link is bad\n",
				task[i]->pid);
		/* 下面依次检查父进程，最年轻的子进程，自己的哥哥进程 弟弟进程 是否指向了自己 */
		if (task[i]->p_pptr == task[i])
			printk("Warning, pid %d parent link points to self\n");
		if (task[i]->p_cptr == task[i])/*  */
			printk("Warning, pid %d child link points to self\n");
		if (task[i]->p_ysptr == task[i])/*  */
			printk("Warning, pid %d ys link points to self\n");
		if (task[i]->p_osptr == task[i])/*  */
			printk("Warning, pid %d os link points to self\n");

		/*  */
		if (task[i]->p_osptr) {
			if (task[i]->p_pptr != task[i]->p_osptr->p_pptr)/*  */
				printk(
			"Warning, pid %d older sibling %d parent is %d\n",
				task[i]->pid, task[i]->p_osptr->pid,/*  */
				task[i]->p_osptr->p_pptr->pid);
			if (task[i]->p_osptr->p_ysptr != task[i])
				printk(
		"Warning, pid %d older sibling %d has mismatched ys link\n",
				task[i]->pid, task[i]->p_osptr->pid);/*  */
		}
		if (task[i]->p_ysptr) {/*  */
			if (task[i]->p_pptr != task[i]->p_ysptr->p_pptr)
				printk(
			"Warning, pid %d younger sibling %d parent is %d\n",
				task[i]->pid, task[i]->p_osptr->pid,/*  */
				task[i]->p_osptr->p_pptr->pid);
			if (task[i]->p_ysptr->p_osptr != task[i])/*  */
				printk(
		"Warning, pid %d younger sibling %d has mismatched os link\n",
				task[i]->pid, task[i]->p_ysptr->pid);/*  */
		}
		if (task[i]->p_cptr) {
			if (task[i]->p_cptr->p_pptr != task[i])/*  */
				printk(
			"Warning, pid %d youngest child %d has mismatched parent link\n",
				task[i]->pid, task[i]->p_cptr->pid);
			if (task[i]->p_cptr->p_ysptr)
				printk(
			"Warning, pid %d youngest child %d has non-NULL ys link\n",
				task[i]->pid, task[i]->p_cptr->pid);
		}
	}
}
#endif /* DEBUG_PROC_TREE */

/* 给指定任务p 发送信号sig 权限为priv */
static inline int send_sig(long sig,struct task_struct * p,int priv)
{
	if (!p)
		return -EINVAL; /* 参数无效错误 */
	if (!priv && (current->euid!=p->euid) && !suser())/* 如果没有priv权限 而且 当前进程和p的euid不同 且 不是超级用户 */
		return -EPERM; /* 则返回没有权限错误 */
	if ((sig == SIGKILL) || (sig == SIGCONT)) {/* 发送这些信号意味着进程要投入运行 */
		if (p->state == TASK_STOPPED)/* 因此修改进程state 为 running */
			p->state = TASK_RUNNING;
		p->exit_code = 0;
		p->signal &= ~( (1<<(SIGSTOP-1)) | (1<<(SIGTSTP-1)) |/* 并且清除相关让进程stop的信号 */
				(1<<(SIGTTIN-1)) | (1<<(SIGTTOU-1)) );
	} 
	/* If the signal will be ignored, don't even post it */
	if ((int) p->sigaction[sig-1].sa_handler == 1)/* 如果此信号的处理方式是忽略 那么更本不回去发送此信号 */
		return 0;
	/* Depends on order SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU */
	if ((sig >= SIGSTOP) && (sig <= SIGTTOU)) /* 这些信号意味着要让进程停止  */
		p->signal &= ~(1<<(SIGCONT-1)); /* 因此我们需要清除可能设置过的SIGCONT */
	/* Actually deliver the signal */
	p->signal |= (1<<(sig-1)); /* 真正的发送信号 */
	return 0;
}

int session_of_pgrp(int pgrp)/* 根据进程组号 获取 进程组所属的会话号 */
{
	struct task_struct **p;

 	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p)/* 找到属于此进程组的进程 */
		if ((*p)->pgrp == pgrp)
			return((*p)->session);/* 返回找到进程的session id */
	return -1;
}

int kill_pg(int pgrp, int sig, int priv)/* 向进程组发送信号 只要向其中一个进程发送成功 返回0 */
{
	struct task_struct **p;/*  */
	int err,retval = -ESRCH;
	int found = 0;/*  */

	if (sig<1 || sig>32 || pgrp<=0)/* 入参判断有效性 */
		return -EINVAL;
 	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p)/* 找到属于pgrp的进程 */
		if ((*p)->pgrp == pgrp) {/*  */
			if (sig && (err = send_sig(sig,*p,priv)))/* 尝试发送信号 */
				retval = err;
			else
				found++; /* 成功则found++ */
		}
	return(found ? 0 : retval);
}

/* 向进程发送信号 */
int kill_proc(int pid, int sig, int priv)/*  */
{
 	struct task_struct **p;

	if (sig<1 || sig>32)/*  */
		return -EINVAL;/*  */
	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p)/*  */
		if ((*p)->pid == pid)/*  */
			return(sig ? send_sig(sig,*p,priv) : 0);/* 找到目标进程 并发送信号 */
	return(-ESRCH);
}

/*
 * POSIX specifies that kill(-1,sig) is unspecified, but what we have
 * is probably wrong.  Should make it like BSD or SYSV.
 */
int sys_kill(int pid,int sig)
{
	struct task_struct **p = NR_TASKS + task;/*  */
	int err, retval = 0;

	if (!pid) /* pid = 0 代表要发送信号给当前进程所在进程组的所有进程*/
		return(kill_pg(current->pid,sig,0));/* 意味着当前进程就是进程组组长? 即pid=pgrp? */
	if (pid == -1) {/* pid=-1代表信号发送给除了0号进程的所有进程*/
		while (--p > &FIRST_TASK)/*  */
			if (err = send_sig(sig,*p,0))/* 发送给FIRST_TASK以外的所有进程 */
				retval = err;/*  */
		return(retval);/*  */
	}
	if (pid < 0) /* pid < -1代表发送给进程组为-pid的所有进程 */
		return(kill_pg(-pid,sig,0));/*  */
	/* Normal kill */
	return(kill_proc(pid,sig,0));
}

/*
 * Determine if a process group is "orphaned", according to the POSIX
 * definition in 2.2.2.52.  Orphaned process groups are not to be affected
 * by terminal-generated stop signals.  Newly orphaned process groups are 
 * to receive a SIGHUP and a SIGCONT.
 * 
 * "I ask you, have you ever known what it is to be an orphan?"
 */
int is_orphaned_pgrp(int pgrp)/*  */
{
	struct task_struct **p;/*  */

	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {/*  */
		if (!(*p) ||
		    ((*p)->pgrp != pgrp) || /*  */
		    ((*p)->state == TASK_ZOMBIE) ||
		    ((*p)->p_pptr->pid == 1))
			continue;
		if (((*p)->p_pptr->pgrp != pgrp) &&/*  */
		    ((*p)->p_pptr->session == (*p)->session))/*  */
			return 0;/*  */
	}
	return(1);	/* (sighing) "Often!" */
}

static int has_stopped_jobs(int pgrp)
{/* 判断进程组中是否含有 停止状态的作业 */
	struct task_struct ** p;
/* 就是判断属于此进程组的进程是否有处于stop状态的 如果有则返回1 */
	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {/*  */
		if ((*p)->pgrp != pgrp)
			continue;/*  */
		if ((*p)->state == TASK_STOPPED)/*  */
			return(1);
	}/*  */
	return(0);
}

/* 进程退出函数 */
volatile void do_exit(long code)
{
	struct task_struct *p;
	int i;
	/* 首先释放当前进程数据段和代码段占据的内存页 */
	/* 数据段和代码段的base相同?  入参是线性空间的基地址和长度*/
	free_page_tables(get_base(current->ldt[1]),get_limit(0x0f));/*  */
	free_page_tables(get_base(current->ldt[2]),get_limit(0x17));/*  */
	for (i=0 ; i<NR_OPEN ; i++)/*  */
		if (current->filp[i])/* 关闭打开的所有文件 */
			sys_close(i);/*  */
	/* 对owd root exec文件 库文件的i节点进行同步操作 */
	iput(current->pwd);
	current->pwd = NULL;/*  */
	iput(current->root);
	current->root = NULL;/*  */
	iput(current->executable);/*  */
	current->executable = NULL;
	iput(current->library);/*  */
	current->library = NULL;/*  */
	current->state = TASK_ZOMBIE;/* 设置进程状态为TASK_ZOMBIE  此状态无法再回到runnin等状态了 */
	current->exit_code = code;/* 设置进程退出码 */
	/* 
	 * Check to see if any process groups have become orphaned
	 * as a result of our exiting, and if they have any stopped
	 * jobs, send them a SIGUP and then a SIGCONT.  (POSIX 3.2.2.2)
	 *
	 * Case i: Our father is in a different pgrp than we are
	 * and we were the only connection outside, so our pgrp
	 * is about to become orphaned.
 	 */
 	 /* 关于孤儿进程组的操作 先不看 */
	if ((current->p_pptr->pgrp != current->pgrp) &&
	    (current->p_pptr->session == current->session) &&
	    is_orphaned_pgrp(current->pgrp) &&
	    has_stopped_jobs(current->pgrp)) {/*  */
		kill_pg(current->pgrp,SIGHUP,1);/*  */
		kill_pg(current->pgrp,SIGCONT,1);/*  */
	}
	/* Let father know we died */
	current->p_pptr->signal |= (1<<(SIGCHLD-1));/* 通知父进程当前进程终止了 */
	
	/*
	 * This loop does two things:
	 * 
  	 * A.  Make init inherit all the child processes
	 * B.  Check to see if any process groups have become orphaned
	 *	as a result of our exiting, and if they have any stopped
	 *	jons, send them a SIGUP and then a SIGCONT.  (POSIX 3.2.2.2)
	 */
	if (p = current->p_cptr) {/* 如果当前进程有子进程 */
		while (1) {
			p->p_pptr = task[1];/* 则循环将子进程的父进程设置为1号init进程 */
			if (p->state == TASK_ZOMBIE)/* 如果该子进程已经zombie 则发送sigchld给init进程 */
				task[1]->signal |= (1<<(SIGCHLD-1));/*  */
			/*
			 * process group orphan check
			 * Case ii: Our child is in a different pgrp 
			 * than we are, and it was the only connection
			 * outside, so the child pgrp is now orphaned.
			 */
			if ((p->pgrp != current->pgrp) &&/*  */
			    (p->session == current->session) &&/*  */
			    is_orphaned_pgrp(p->pgrp) &&
			    has_stopped_jobs(p->pgrp)) {/*  */
				kill_pg(p->pgrp,SIGHUP,1);
				kill_pg(p->pgrp,SIGCONT,1);/*  */
			}
			if (p->p_osptr) {
				p = p->p_osptr;/* 处理下一个子进程 下一个哥哥进程*/
				continue;
			}
			/*
			 * This is it; link everything into init's children 
			 * and leave 
			 走到这里所有子进程都已经处理 p指向最老的子进程
			 */
			p->p_osptr = task[1]->p_cptr;/* 下面几行将当前子进程全部插入init进程的子进程链表中   */
			task[1]->p_cptr->p_ysptr = p;/* init进程的最年轻子进程的弟弟进程是当前要退出的进程的最老的子进程 */
			task[1]->p_cptr = current->p_cptr;/* init进程的子进程重新指向当前要退出的进程的最年轻子进程 */
			current->p_cptr = 0;/*  */
			break;
		}
	}
	if (current->leader) {/* 处理和session相关的 */
		struct task_struct **p;/*  */
		struct tty_struct *tty;
/*  */
		if (current->tty >= 0) {/*  */
			tty = TTY_TABLE(current->tty);/*  */
			if (tty->pgrp>0)/*  */
				kill_pg(tty->pgrp, SIGHUP, 1);/*  */
			tty->pgrp = 0;/*  */
			tty->session = 0;/*  */
		}
	 	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p)/*  */
			if ((*p)->session == current->session)/*  */
				(*p)->tty = -1;
	}
	if (last_task_used_math == current)/*  */
		last_task_used_math = NULL;
#ifdef DEBUG_PROC_TREE
	audit_ptree();
#endif
	schedule();/* 执行任务切换  此时任务数组中task struct仍然存在 但是已经无法投入运行了 等待被回收 release */
}

int sys_exit(int error_code)/* 低8位可能用于&0x80 */
{
	do_exit((error_code&0xff)<<8);
}

/* waitpid系统调用 stat_addr是用户传进来的用来保存状态的内存空间地址*/
int sys_waitpid(pid_t pid,unsigned long * stat_addr, int options) /* 等待的是自己的子进程 */
{
	int flag;
	struct task_struct *p;/*  */
	unsigned long oldblocked;
	verify_area(stat_addr,4);/* 验证用户穿进来的空间 如果是只读页 则会分配一页并取消写保护*/
repeat:
	flag=0;
	for (p = current->p_cptr ; p ; p = p->p_osptr) {/* 从最年轻的子进程开始循环 */
		if (pid>0) {/* pid>0代表要等待指定的子进程 */
			if (p->pid != pid)/*  */
				continue;/*  */
		} else if (!pid) {/* pid=0等待组号等于当前进程组号的任何子进程 */
			if (p->pgrp != current->pgrp)/*  */
				continue;/*  */
		} else if (pid != -1) {/* 等待组号为-pid的子进程 */
			if (p->pgrp != -pid)/*  */
				continue;/*  */
		}
		switch (p->state) {/*  */
			case TASK_STOPPED:/* 找到一个子进程已经stop */
				if (!(options & WUNTRACED) || /* 但是WUNTRACED没有置位 或者 进程退出码=0 则继续扫描 */
				    !p->exit_code)
					continue;/*  */
				put_fs_long((p->exit_code << 8) | 0x7f,
					stat_addr);/* 退出码左移后或上0x7f  存入用户传进来的buffer*/
				p->exit_code = 0;/*  */
				return p->pid;/* 返回stop的子进程 */
			case TASK_ZOMBIE:/* 找到一个已经退出的子进程  */
				current->cutime += p->utime;/* 将其时间更新到父进程 */
				current->cstime += p->stime;/*  */
				flag = p->pid;/*  */
				put_fs_long(p->exit_code, stat_addr); /* 退出码拷贝到用户提供的buffer */
				release(p);/* 收集已经zombie的子进程的最后一点信息 task struct */
#ifdef DEBUG_PROC_TREE
				audit_ptree();
#endif
				return flag;/* 最后返回退出的子进程pid */
			default:
				flag=1;/* 表示找打一个符合wait条件的子进程 但是它处于运行或睡眠状态 */
				continue;/* 找到符合条件的进程 但是处于运行或睡眠 则continue寻找下个子进程 */
		}
	}
	if (flag) {/* 符合等待的子进程处于运行或睡眠状态 */
		if (options & WNOHANG)/* WNOHANG置位 则立刻返回 */
			return 0;
		current->state=TASK_INTERRUPTIBLE;/* 否则将当前进程 设置为TASK_INTERRUPTIBLE */
		oldblocked = current->blocked;/* 且保留原blocked */
		current->blocked &= ~(1<<(SIGCHLD-1));/* 修改当前block 以便允许接收SIGCHLD */
		schedule();/* 切换任务 */
		current->blocked = oldblocked;/* 当任务重新被调度时 恢复block */
		if (current->signal & ~(current->blocked | (1<<(SIGCHLD-1))))/* 此时如果进程收到除SIGCHLD以外的信号 则以ERESTARTSYS返回  */
			return -ERESTARTSYS;/*  */
		else/* 否则重新到repeat处理 说明有sigchld信号来了 再找找看有没有zombie的子进程*/
			goto repeat;
	}
	return -ECHILD; /* 没有找到符合条件的子进程 */
}


