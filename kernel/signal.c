/*
 *  linux/kernel/signal.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <signal.h>
#include <errno.h>

/* 获取当前任务的信号屏蔽码 */
int sys_sgetmask()
{
	return current->blocked;
}

/* 设置当前任务的信号屏蔽位图 返回原有的位图 */
int sys_ssetmask(int newmask)
{
	int old=current->blocked;/*  */

	current->blocked = newmask & ~(1<<(SIGKILL-1)) & ~(1<<(SIGSTOP-1));/* SIGKILL  SIGSTOP 不能被屏蔽*/
	return old;
}

/* 检测并取得 收到但被屏蔽的信号 还未处理的信号位图将被放入set */
int sys_sigpending(sigset_t *set)/*  */
{
    /* fill in "set" with signals pending but blocked. */
    verify_area(set,4);/* 首先验证用户态传递来的set指向的地址处的4byte空间是否可写 */
	/* fs指向用户态数据段 */
    put_fs_long(current->blocked & current->signal, (unsigned long *)set);/* 收到但被屏蔽的信号 还未处理的信号位图将被放入set */
    return 0;
}

/* atomically swap in the new signal mask, and wait for a signal.
 *
 * we need to play some games with syscall restarting.  We get help
 * from the syscall library interface.  Note that we need to coordinate
 * the calling convention with the libc routine.
 *
 * "set" is just the sigmask as described in 1003.1-1988, 3.3.7.
 * 	It is assumed that sigset_t can be passed as a 32 bit quantity.
 *
 * "restart" holds a restart indication.  If it's non-zero, then we 
 * 	install the old mask, and return normally.  If it's zero, we store 
 * 	the current mask in old_mask and block until a signal comes in.
 */
 //自动更换成新的信号mask 并等待信号的到来
int sys_sigsuspend(int restart, unsigned long old_mask, unsigned long set)/*  */
{
    extern int sys_pause(void);/*  */

    if (restart) {/* 如果restart不为0 则表示让程序重新运行起来 */
	/* we're restarting */
	current->blocked = old_mask;/* 于是恢复之前保存的进程原来的mask */
	return -EINTR;/* 告诉程序我们被中断了，需要重启 */
    }
    /* we're not restarting.  do the work */
    *(&restart) = 1;/* 第一次调用 restart=0 */
    *(&old_mask) = current->blocked;/* 保存原来的mask */
    current->blocked = set;/* 临时设置mask为set */
    (void) sys_pause();		/*  */	/* return after a signal arrives */
    return -ERESTARTNOINTR;		/* handle the signal, and come back */
}

/* 复制sigaction数据到fs数据段to地址处 */
static inline void save_old(char * from,char * to)
{
	int i;/*  */

	verify_area(to, sizeof(struct sigaction));/* 验证用户空间是否够大 */
	for (i=0 ; i< sizeof(struct sigaction) ; i++) {/* 一次拷贝一个字节 */
		put_fs_byte(*from,to);/*  */
		from++;
		to++;/*  */
	}
}
/* 把fs段from地址处的sigaction复制到内核空间to地址处 */
static inline void get_new(char * from,char * to)
{
	int i;
/*  */
	for (i=0 ; i< sizeof(struct sigaction) ; i++)/* 一次一个byte */
		*(to++) = get_fs_byte(from++);/*  */
}

/* signal系统调用 */
int sys_signal(int signum, long handler, long restorer)
{
	struct sigaction tmp;

	if (signum<1 || signum>32 || signum==SIGKILL || signum==SIGSTOP)/* 信号有效性判断 */
		return -EINVAL;/*  */
	tmp.sa_handler = (void (*)(int)) handler;/* 设置handler */
	tmp.sa_mask = 0;/* 执行信号处理函数时的屏蔽码 */
	tmp.sa_flags = SA_ONESHOT | SA_NOMASK;/* 句柄使用一次就恢复默认|允许信号在自己的处理句柄中收到 */
	tmp.sa_restorer = (void (*)(void)) restorer;/* c库提供 */
	handler = (long) current->sigaction[signum-1].sa_handler;
	current->sigaction[signum-1] = tmp;/* 注册到任务task struct中 */
	return handler;/* 返回原来的sigaction */
}

/* sigaction系统调用 */
int sys_sigaction(int signum, const struct sigaction * action,
	struct sigaction * oldaction)
{
	struct sigaction tmp;

	if (signum<1 || signum>32 || signum==SIGKILL || signum==SIGSTOP)/* 有效性检查 */
		return -EINVAL;
	tmp = current->sigaction[signum-1];/* 原有的sigaction */
	get_new((char *) action,
		(char *) (signum-1+current->sigaction));/* 从用户数据段拷贝sigaciton到内核态 */
	if (oldaction)
		save_old((char *) &tmp,(char *) oldaction);/* oldaction不为null 则将原有的sigaction拷贝到oldaction指定的用户数据段地址中 */
	if (current->sigaction[signum-1].sa_flags & SA_NOMASK)/* 如果设置了SA_NOMASK 则允许信号句柄处理过程中收到自己，因此清空mask */
		current->sigaction[signum-1].sa_mask = 0;
	else
		current->sigaction[signum-1].sa_mask |= (1<<(signum-1));/* 否则设置将本信号屏蔽 */
	return 0;
}

/*
 * Routine writes a core dump image in the current directory.
 * Currently not implemented.
 */
int core_dump(long signr)/*  */
{
	return(0);	/* We didn't do a dump */
}
/* 信号处理句柄插入到用户堆栈中 在系统调用结束返回后 立即执行信号处理句柄 然后继续执行用户程序 */
int do_signal(long signr,long eax /* 系统调用返回值 */,long ebx, long ecx, long edx, long orig_eax,
	long fs, long es, long ds,
	long eip, long cs, long eflags,
	unsigned long * esp, long ss)
{
	unsigned long sa_handler;
	long old_eip=eip; /* 用户程序地址 保存下来 后面eip会被设置成信号句柄地址 */
	struct sigaction * sa = current->sigaction + signr - 1;/* 根据信号值得到sigaction */
	int longs;

	unsigned long * tmp_esp;/*  */

#ifdef notdef
	printk("pid: %d, signr: %x, eax=%d, oeax = %d, int=%d\n", 
		current->pid, signr, eax, orig_eax, 
		sa->sa_flags & SA_INTERRUPT);
#endif
	if ((orig_eax != -1) &&/* 来这里之前是一个系统调用 orig_eax存的是调用号*/
		/* eax中保存的是来这前的系统调用的返回值 是ERESTARTSYS或ERESTARTNOINTR 则代表处理完信号后要重新回到该系统调用 */
	    ((eax == -ERESTARTSYS) || (eax == -ERESTARTNOINTR))) {
		if ((eax == -ERESTARTSYS) && ((sa->sa_flags & SA_INTERRUPT) ||/* SA_INTERRUPT表示被信号中断后不重启系统调用 */
		    signr < SIGCONT || signr > SIGTTOU))/*  */
			*(&eax) = -EINTR; /* 修改系统调用返回值为EINTR */
		else {/* 否则就恢复eax在调用系统调用之前的值 即调用号， 然后ip-2，即返回用户程序时 重新执行被信号中断的系统调用 */
			*(&eax) = orig_eax;/*  */
			*(&eip) = old_eip -= 2;/* 为啥-2啊 应该-1才是int 0x80把 */
		}
	}

	/* 获取信号处理句柄 */
	sa_handler = (unsigned long) sa->sa_handler;/*  */
	if (sa_handler==1) /* SIG_IGN 忽略 不对信号处理 */
		return(1);   /* Ignore, see if there are more signals... 返回不为0 ret_from_sys_call会从新回到ret_from_sys_call再看看是否还有信号要处理 */
	if (!sa_handler) { /* SIG_DFL 根据具体信号进行默认处理 */
		switch (signr) {/*  */
		case SIGCONT:
		case SIGCHLD:
			return(1);  /* Ignore, ... */

		case SIGSTOP:/* 以下四种信号 将进程state设置为TASK_STOPPED */
		case SIGTSTP:
		case SIGTTIN:
		case SIGTTOU:
			current->state = TASK_STOPPED;
			current->exit_code = signr;
			/* 如果进程父进程没有设置 SA_NOCLDSTOP 标志位 即接收SIGCHLD  我们就像父进程发送SIGCHLD信号*/
			if (!(current->p_pptr->sigaction[SIGCHLD-1].sa_flags & 
					SA_NOCLDSTOP))/*  */
				current->p_pptr->signal |= (1<<(SIGCHLD-1));/* 像父进程发送SIGCHLD  其实就是set bit 父进程对应的 */
			return(1);  /* Reschedule another event */

		/* 以下信号则产生coredump 并将退出码或上0x80 */
		case SIGQUIT:
		case SIGILL:
		case SIGTRAP:
		case SIGIOT:
		case SIGFPE:
		case SIGSEGV:
			if (core_dump(signr))
				do_exit(signr|0x80);
			/* fall through */
		default:
			do_exit(signr);/* 以信号值作为退出码 */
		}
	}
	/*
	 * OK, we're invoking a handler 
	 */
	 /* 走到这里 则此信号设置了处理句柄 */
	if (sa->sa_flags & SA_ONESHOT)/* 如果设置了one shot 则之前设置的句柄 清空 恢复成默认 */
		sa->sa_handler = NULL;/* sig ign就是0 */
	*(&eip) = sa_handler;/* 否则的话将eip设置为句柄地址 */
	longs = (sa->sa_flags & SA_NOMASK)?7:8;/* 根据是否允在信号处理时收到信号自己 如果设置了SA_NOMASK 则不需要压入mask 否则需要压入mask*/
	*(&esp) -= longs;/* 用户堆栈预留对应空间 */
	verify_area(esp,longs*4);/* 验证是否有可写 */
	tmp_esp=esp;/* 保存新的临时esp */
	put_fs_long((long) sa->sa_restorer,tmp_esp++);/* 依次在堆栈预留的空间中存入restore，mask，eax，ecx，edx，eflags，old eip */
	put_fs_long(signr,tmp_esp++);/*  */
	if (!(sa->sa_flags & SA_NOMASK))/*  */
		put_fs_long(current->blocked,tmp_esp++);/*  */
	put_fs_long(eax,tmp_esp++);/*  */
	put_fs_long(ecx,tmp_esp++);/*  */
	put_fs_long(edx,tmp_esp++);/*  */
	put_fs_long(eflags,tmp_esp++);/*  */
	put_fs_long(old_eip,tmp_esp++);/*  */
	current->blocked |= sa->sa_mask;/*  */
	return(0);		/* Continue, execute handler  返回0 则ret from syscall不会在处理更多信号  直接退出到用户程序*/
}
