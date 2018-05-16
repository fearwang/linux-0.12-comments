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

/* ��ȡ��ǰ������ź������� */
int sys_sgetmask()
{
	return current->blocked;
}

/* ���õ�ǰ������ź�����λͼ ����ԭ�е�λͼ */
int sys_ssetmask(int newmask)
{
	int old=current->blocked;/*  */

	current->blocked = newmask & ~(1<<(SIGKILL-1)) & ~(1<<(SIGSTOP-1));/* SIGKILL  SIGSTOP ���ܱ�����*/
	return old;
}

/* ��Ⲣȡ�� �յ��������ε��ź� ��δ������ź�λͼ��������set */
int sys_sigpending(sigset_t *set)/*  */
{
    /* fill in "set" with signals pending but blocked. */
    verify_area(set,4);/* ������֤�û�̬��������setָ��ĵ�ַ����4byte�ռ��Ƿ��д */
	/* fsָ���û�̬���ݶ� */
    put_fs_long(current->blocked & current->signal, (unsigned long *)set);/* �յ��������ε��ź� ��δ������ź�λͼ��������set */
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
 //�Զ��������µ��ź�mask ���ȴ��źŵĵ���
int sys_sigsuspend(int restart, unsigned long old_mask, unsigned long set)/*  */
{
    extern int sys_pause(void);/*  */

    if (restart) {/* ���restart��Ϊ0 ���ʾ�ó��������������� */
	/* we're restarting */
	current->blocked = old_mask;/* ���ǻָ�֮ǰ����Ľ���ԭ����mask */
	return -EINTR;/* ���߳������Ǳ��ж��ˣ���Ҫ���� */
    }
    /* we're not restarting.  do the work */
    *(&restart) = 1;/* ��һ�ε��� restart=0 */
    *(&old_mask) = current->blocked;/* ����ԭ����mask */
    current->blocked = set;/* ��ʱ����maskΪset */
    (void) sys_pause();		/*  */	/* return after a signal arrives */
    return -ERESTARTNOINTR;		/* handle the signal, and come back */
}

/* ����sigaction���ݵ�fs���ݶ�to��ַ�� */
static inline void save_old(char * from,char * to)
{
	int i;/*  */

	verify_area(to, sizeof(struct sigaction));/* ��֤�û��ռ��Ƿ񹻴� */
	for (i=0 ; i< sizeof(struct sigaction) ; i++) {/* һ�ο���һ���ֽ� */
		put_fs_byte(*from,to);/*  */
		from++;
		to++;/*  */
	}
}
/* ��fs��from��ַ����sigaction���Ƶ��ں˿ռ�to��ַ�� */
static inline void get_new(char * from,char * to)
{
	int i;
/*  */
	for (i=0 ; i< sizeof(struct sigaction) ; i++)/* һ��һ��byte */
		*(to++) = get_fs_byte(from++);/*  */
}

/* signalϵͳ���� */
int sys_signal(int signum, long handler, long restorer)
{
	struct sigaction tmp;

	if (signum<1 || signum>32 || signum==SIGKILL || signum==SIGSTOP)/* �ź���Ч���ж� */
		return -EINVAL;/*  */
	tmp.sa_handler = (void (*)(int)) handler;/* ����handler */
	tmp.sa_mask = 0;/* ִ���źŴ�����ʱ�������� */
	tmp.sa_flags = SA_ONESHOT | SA_NOMASK;/* ���ʹ��һ�ξͻָ�Ĭ��|�����ź����Լ��Ĵ��������յ� */
	tmp.sa_restorer = (void (*)(void)) restorer;/* c���ṩ */
	handler = (long) current->sigaction[signum-1].sa_handler;
	current->sigaction[signum-1] = tmp;/* ע�ᵽ����task struct�� */
	return handler;/* ����ԭ����sigaction */
}

/* sigactionϵͳ���� */
int sys_sigaction(int signum, const struct sigaction * action,
	struct sigaction * oldaction)
{
	struct sigaction tmp;

	if (signum<1 || signum>32 || signum==SIGKILL || signum==SIGSTOP)/* ��Ч�Լ�� */
		return -EINVAL;
	tmp = current->sigaction[signum-1];/* ԭ�е�sigaction */
	get_new((char *) action,
		(char *) (signum-1+current->sigaction));/* ���û����ݶο���sigaciton���ں�̬ */
	if (oldaction)
		save_old((char *) &tmp,(char *) oldaction);/* oldaction��Ϊnull ��ԭ�е�sigaction������oldactionָ�����û����ݶε�ַ�� */
	if (current->sigaction[signum-1].sa_flags & SA_NOMASK)/* ���������SA_NOMASK �������źž������������յ��Լ���������mask */
		current->sigaction[signum-1].sa_mask = 0;
	else
		current->sigaction[signum-1].sa_mask |= (1<<(signum-1));/* �������ý����ź����� */
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
/* �źŴ��������뵽�û���ջ�� ��ϵͳ���ý������غ� ����ִ���źŴ����� Ȼ�����ִ���û����� */
int do_signal(long signr,long eax /* ϵͳ���÷���ֵ */,long ebx, long ecx, long edx, long orig_eax,
	long fs, long es, long ds,
	long eip, long cs, long eflags,
	unsigned long * esp, long ss)
{
	unsigned long sa_handler;
	long old_eip=eip; /* �û������ַ �������� ����eip�ᱻ���ó��źž����ַ */
	struct sigaction * sa = current->sigaction + signr - 1;/* �����ź�ֵ�õ�sigaction */
	int longs;

	unsigned long * tmp_esp;/*  */

#ifdef notdef
	printk("pid: %d, signr: %x, eax=%d, oeax = %d, int=%d\n", 
		current->pid, signr, eax, orig_eax, 
		sa->sa_flags & SA_INTERRUPT);
#endif
	if ((orig_eax != -1) &&/* ������֮ǰ��һ��ϵͳ���� orig_eax����ǵ��ú�*/
		/* eax�б����������ǰ��ϵͳ���õķ���ֵ ��ERESTARTSYS��ERESTARTNOINTR ����������źź�Ҫ���»ص���ϵͳ���� */
	    ((eax == -ERESTARTSYS) || (eax == -ERESTARTNOINTR))) {
		if ((eax == -ERESTARTSYS) && ((sa->sa_flags & SA_INTERRUPT) ||/* SA_INTERRUPT��ʾ���ź��жϺ�����ϵͳ���� */
		    signr < SIGCONT || signr > SIGTTOU))/*  */
			*(&eax) = -EINTR; /* �޸�ϵͳ���÷���ֵΪEINTR */
		else {/* ����ͻָ�eax�ڵ���ϵͳ����֮ǰ��ֵ �����úţ� Ȼ��ip-2���������û�����ʱ ����ִ�б��ź��жϵ�ϵͳ���� */
			*(&eax) = orig_eax;/*  */
			*(&eip) = old_eip -= 2;/* Ϊɶ-2�� Ӧ��-1����int 0x80�� */
		}
	}

	/* ��ȡ�źŴ����� */
	sa_handler = (unsigned long) sa->sa_handler;/*  */
	if (sa_handler==1) /* SIG_IGN ���� �����źŴ��� */
		return(1);   /* Ignore, see if there are more signals... ���ز�Ϊ0 ret_from_sys_call����»ص�ret_from_sys_call�ٿ����Ƿ����ź�Ҫ���� */
	if (!sa_handler) { /* SIG_DFL ���ݾ����źŽ���Ĭ�ϴ��� */
		switch (signr) {/*  */
		case SIGCONT:
		case SIGCHLD:
			return(1);  /* Ignore, ... */

		case SIGSTOP:/* ���������ź� ������state����ΪTASK_STOPPED */
		case SIGTSTP:
		case SIGTTIN:
		case SIGTTOU:
			current->state = TASK_STOPPED;
			current->exit_code = signr;
			/* ������̸�����û������ SA_NOCLDSTOP ��־λ ������SIGCHLD  ���Ǿ��񸸽��̷���SIGCHLD�ź�*/
			if (!(current->p_pptr->sigaction[SIGCHLD-1].sa_flags & 
					SA_NOCLDSTOP))/*  */
				current->p_pptr->signal |= (1<<(SIGCHLD-1));/* �񸸽��̷���SIGCHLD  ��ʵ����set bit �����̶�Ӧ�� */
			return(1);  /* Reschedule another event */

		/* �����ź������coredump �����˳������0x80 */
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
			do_exit(signr);/* ���ź�ֵ��Ϊ�˳��� */
		}
	}
	/*
	 * OK, we're invoking a handler 
	 */
	 /* �ߵ����� ����ź������˴����� */
	if (sa->sa_flags & SA_ONESHOT)/* ���������one shot ��֮ǰ���õľ�� ��� �ָ���Ĭ�� */
		sa->sa_handler = NULL;/* sig ign����0 */
	*(&eip) = sa_handler;/* ����Ļ���eip����Ϊ�����ַ */
	longs = (sa->sa_flags & SA_NOMASK)?7:8;/* �����Ƿ������źŴ���ʱ�յ��ź��Լ� ���������SA_NOMASK ����Ҫѹ��mask ������Ҫѹ��mask*/
	*(&esp) -= longs;/* �û���ջԤ����Ӧ�ռ� */
	verify_area(esp,longs*4);/* ��֤�Ƿ��п�д */
	tmp_esp=esp;/* �����µ���ʱesp */
	put_fs_long((long) sa->sa_restorer,tmp_esp++);/* �����ڶ�ջԤ���Ŀռ��д���restore��mask��eax��ecx��edx��eflags��old eip */
	put_fs_long(signr,tmp_esp++);/*  */
	if (!(sa->sa_flags & SA_NOMASK))/*  */
		put_fs_long(current->blocked,tmp_esp++);/*  */
	put_fs_long(eax,tmp_esp++);/*  */
	put_fs_long(ecx,tmp_esp++);/*  */
	put_fs_long(edx,tmp_esp++);/*  */
	put_fs_long(eflags,tmp_esp++);/*  */
	put_fs_long(old_eip,tmp_esp++);/*  */
	current->blocked |= sa->sa_mask;/*  */
	return(0);		/* Continue, execute handler  ����0 ��ret from syscall�����ڴ�������ź�  ֱ���˳����û�����*/
}
