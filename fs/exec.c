/*
 *  linux/fs/exec.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * #!-checking implemented by tytso.
 */

/*
 * Demand-loading implemented 01.12.91 - no need to read anything but
 * the header into memory. The inode of the executable is put into
 * "current->executable", and page faults do the actual loading. Clean.
 *
 * Once more I can proudly say that linux stood up to being changed: it
 * was less than 2 hours work to get demand-loading completely implemented.
 */

#include <signal.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <a.out.h>

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/segment.h>

extern int sys_exit(int exit_code);
extern int sys_close(int fd);

/*
 * MAX_ARG_PAGES defines the number of pages allocated for arguments
 * and envelope for the new program. 32 should suffice, this gives
 * a maximum env+arg of 128kB !
 */
 //分配给环境变量和参数的最大页面数量
#define MAX_ARG_PAGES 32

//给进程选择一个库文件 以替换当前的库文件
int sys_uselib(const char * library)
{
	struct m_inode * inode;
	unsigned long base;

	//判断是否是普通进程
	if (get_limit(0x17) != TASK_SIZE)
		return -EINVAL;
	//如果入参制定了库文件 则得到inode
	if (library) {
		if (!(inode=namei(library)))		/* get library inode */
			return -ENOENT;
	} else
		inode = NULL;
/* we should check filetypes (headers etc), but we don't */
//先释放当前的库文件inode
	iput(current->library);
//
	current->library = NULL;
//得到进程的基地址
	base = get_base(current->ldt[2]);
//库文件偏移地址
	base += LIBRARY_OFFSET;
//释放库文件映射区域的页表
	free_page_tables(base,LIBRARY_SIZE);
	current->library = inode;
	return 0;
}

/*
 * create_tables() parses the env- and arg-strings in new user
 * memory and creates the pointer tables from them, and puts their
 * addresses on the "stack", returning the new stack pointer value.
 */
 //在新任务栈中创建环境变量和参数指针表   参数和环境变量拷贝到用户空间
static unsigned long * create_tables(char * p,int argc,int envc)
{
	unsigned long *argv,*envp;
	unsigned long * sp;
	//4byte对齐
	sp = (unsigned long *) (0xfffffffc & (unsigned long) p);
	//预留指定个数的指针 加一个null的空间
	sp -= envc+1;
	envp = sp;
	sp -= argc+1;
	argv = sp;
	//将argc argv  envp指针压入栈中 用户栈
	put_fs_long((unsigned long)envp,--sp);
	put_fs_long((unsigned long)argv,--sp);
	put_fs_long((unsigned long)argc,--sp);
	//拷贝参数
	while (argc-->0) {
		put_fs_long((unsigned long) p,argv++);
		while (get_fs_byte(p++)) /* nothing */ ;
	}
	put_fs_long(0,argv);
	//拷贝环境变量
	while (envc-->0) {
		put_fs_long((unsigned long) p,envp++);
		while (get_fs_byte(p++)) /* nothing */ ;
	}
	put_fs_long(0,envp);
	return sp;
}

/*
 * count() counts the number of arguments/envelopes
 */
 //计算参数个数
static int count(char ** argv)
{
	int i=0;
	char ** tmp;

//在内核中 fs指向用户堆栈  用户态数据段
	if (tmp = argv)
		while (get_fs_long((unsigned long *) (tmp++)))
			i++;

	return i;
}

/*
 * 'copy_string()' copies argument/envelope strings from user
 * memory to free pages in kernel mem. These are in a format ready
 * to be put directly into the top of new user memory.
 *
 * Modified by TYT, 11/24/91 to add the from_kmem argument, which specifies
 * whether the string and the string array are from user or kernel segments:
 * 
 * from_kmem     argv *        argv **
 *    0          user space    user space
 *    1          kernel space  user space
 *    2          kernel space  kernel space
 * 
 * We do this by playing games with the fs segment register.  Since it
 * it is expensive to load a segment register, we try to avoid calling
 * set_fs() unless we absolutely have to.
 */
 //复制指定个数的参数字符串到 参数和环境空间中
static unsigned long copy_strings(int argc,char ** argv,unsigned long *page,
		unsigned long p, int from_kmem /* 先考虑from_Kem=0 所有字符串都来自用户空间 */)
{
	char *tmp, *pag;
	int len, offset = 0;
	unsigned long old_fs, new_fs;
	//p 字符串个数为0 则退出
	if (!p)
		return 0;	/* bullet-proofing */
	//得到ds 和 fs 分别指向用户data和内核data
	new_fs = get_ds();
	old_fs = get_fs();
	//如果字符串和字符指针来自内核 则重新设置fs指向内核data段
	if (from_kmem==2)
		set_fs(new_fs);
	while (argc-- > 0) {
		if (from_kmem == 1)
			set_fs(new_fs);
		//代码是不是有问题 (unsigned long *)(argv + argc)
		if (!(tmp = (char *)get_fs_long(((unsigned long *)argv)+argc)))
			panic("argc is wrong");
		if (from_kmem == 1)
			set_fs(old_fs);
		len=0;		/* remember zero-padding */
		do {
			//计算当前字符串长度
			len++;
		} while (get_fs_byte(tmp++));
		//空间不够了
		if (p-len < 0) {	/* this shouldn't happen - 128kB */
			set_fs(old_fs);
			return 0;
		}
		while (len) {
			--p; --tmp; --len;
			if (--offset < 0) {
				offset = p % PAGE_SIZE;
				if (from_kmem==2)
					set_fs(old_fs);
				if (!(pag = (char *) page[p/PAGE_SIZE]) &&
				    !(pag = (char *) page[p/PAGE_SIZE] =
				      (unsigned long *) get_free_page())) 
					return 0;
				if (from_kmem==2)
					set_fs(new_fs);

			}
			*(pag + offset) = get_fs_byte(tmp);
		}
	}
	if (from_kmem==2)
		set_fs(old_fs);
	return p;
}

static unsigned long change_ldt(unsigned long text_size,unsigned long * page)
{
	unsigned long code_limit,data_limit,code_base,data_base;
	int i;

	code_limit = TASK_SIZE;
	data_limit = TASK_SIZE;
	code_base = get_base(current->ldt[1]);
	data_base = code_base;
	set_base(current->ldt[1],code_base);
	set_limit(current->ldt[1],code_limit);
	set_base(current->ldt[2],data_base);
	set_limit(current->ldt[2],data_limit);
/* make sure fs points to the NEW data segment */
	__asm__("pushl $0x17\n\tpop %%fs"::);
//库文件占地址空间最后4M
	data_base += data_limit - LIBRARY_SIZE;
	for (i=MAX_ARG_PAGES-1 ; i>=0 ; i--) {
		//然后将参数和库文件空间中已经存放数据的页 放到数据段末端
		data_base -= PAGE_SIZE;
		if (page[i])
			put_dirty_page(page[i],data_base);
	}
	return data_limit;
}

/*
 * 'do_execve()' executes a new program.
 *
 * NOTE! We leave 4MB free at the top of the data-area for a loadable
 * library.
 */
int do_execve(unsigned long * eip /* 调用系统调用的进程的代码指针 */,long tmp /* 汇编调用本函数的返回地址 无用 */,char * filename,
	char ** argv, char ** envp /* 这三个是用户空间传来的  含义明确 */)
{
	struct m_inode * inode;
	struct buffer_head * bh;
	struct exec ex;
	unsigned long page[MAX_ARG_PAGES];
	int i,argc,envc;
	int e_uid, e_gid;
	int retval;
	int sh_bang = 0;
	//p指向参数空间的最后一个4byte处
	unsigned long p=PAGE_SIZE*MAX_ARG_PAGES-4;

	//cs是否指向用户段
	if ((0xffff & eip[1]) != 0x000f)
		panic("execve called from supervisor mode");
	//清01 page数组
	for (i=0 ; i<MAX_ARG_PAGES ; i++)	/* clear page-table */
		page[i]=0;
	//得到要执行文件的inode
	if (!(inode=namei(filename)))		/* get executables inode */
		return -ENOENT;
	//计算参数 和 环境变量的个数
	argc = count(argv);
	envc = count(envp);
	
restart_interp:
	//不是常规文件 无法执行
	if (!S_ISREG(inode->i_mode)) {	/* must be regular file */
		retval = -EACCES;
		goto exec_error2;
	}
	i = inode->i_mode;
	//set user id 和 set group id 置位的话 则uid egid 设置为文件的uid gid
	e_uid = (i & S_ISUID) ? inode->i_uid : current->euid;
	e_gid = (i & S_ISGID) ? inode->i_gid : current->egid;
	//取出rwx
	if (current->euid == inode->i_uid)
		i >>= 6;
	else if (in_group_p(inode->i_gid))
		i >>= 3;
	if (!(i & 1) && /* 当前进程无权限 */
		//该文件没有x属性 且 不是root进程
	    !((inode->i_mode & 0111) && suser())) {
	    //出错返回
		retval = -ENOEXEC;
		goto exec_error2;
	}
	//然后读取可执行文件的 第一个块进内存
	if (!(bh = bread(inode->i_dev,inode->i_zone[0]))) {
		retval = -EACCES;
		goto exec_error2;
	}
	//取出文件头部
	ex = *((struct exec *) bh->b_data);	/* read exec-header */
	if ((bh->b_data[0] == '#') && (bh->b_data[1] == '!') && (!sh_bang)) {
		//处理sh脚本文件
		/*
		 * This section does the #! interpretation.
		 * Sorta complicated, but hopefully it will work.  -TYT
		 */

		char buf[128], *cp, *interp, *i_name, *i_arg;
		unsigned long old_fs;
		//处理解释执行sh的解释器 和 参数(如果有的话)
		strncpy(buf, bh->b_data+2, 127);
		brelse(bh);
		iput(inode);
		buf[127] = '\0';
		if (cp = strchr(buf, '\n')) {
			*cp = '\0';
			for (cp = buf; (*cp == ' ') || (*cp == '\t'); cp++);
		}
		if (!cp || *cp == '\0') {
			retval = -ENOEXEC; /* No interpreter name found */
			goto exec_error1;
		}
		interp = i_name = cp;
		i_arg = 0;
		for ( ; *cp && (*cp != ' ') && (*cp != '\t'); cp++) {
 			if (*cp == '/')
				i_name = cp+1;
		}
		if (*cp) {
			*cp++ = '\0';
			i_arg = cp;
		}
		/*
		 * OK, we've parsed out the interpreter name and
		 * (optional) argument.
		 */
		if (sh_bang++ == 0) {
			//
			p = copy_strings(envc, envp, page, p, 0);
			p = copy_strings(--argc, argv+1, page, p, 0);
		}
		/*
		 * Splice in (1) the interpreter's name for argv[0]
		 *           (2) (optional) argument to interpreter
		 *           (3) filename of shell script
		 *
		 * This is done in reverse order, because of how the
		 * user environment and arguments are stored.
		 */
		p = copy_strings(1, &filename, page, p, 1);
		argc++;
		if (i_arg) {
			p = copy_strings(1, &i_arg, page, p, 2);
			argc++;
		}
		p = copy_strings(1, &i_name, page, p, 2);
		argc++;
		if (!p) {
			retval = -ENOMEM;
			goto exec_error1;
		}
		/*
		 * OK, now restart the process with the interpreter's inode.
		 */
		 //使用解释器的inode 重启进程
		old_fs = get_fs();
		set_fs(get_ds());
		//得到解释器inode 然后exec这个解释器的inode
		if (!(inode=namei(interp))) { /* get executables inode */
			set_fs(old_fs);
			retval = -ENOENT;
			goto exec_error1;
		}
		set_fs(old_fs);
		goto restart_interp;
	}
	//此时文件头已经复制出来了 可以先释放buffer
	brelse(bh);
	//判断不饿能支持运行的情况
	if (N_MAGIC(ex) != ZMAGIC || ex.a_trsize || ex.a_drsize ||
		ex.a_text+ex.a_data+ex.a_bss>0x3000000 ||
		inode->i_size < ex.a_text+ex.a_data+ex.a_syms+N_TXTOFF(ex)) {
		retval = -ENOEXEC;
		goto exec_error2;
	}
	//代码开始处 没有一页对齐 
	if (N_TXTOFF(ex) != BLOCK_SIZE) {
		printk("%s: N_TXTOFF != BLOCK_SIZE. See a.out.h.", filename);
		retval = -ENOEXEC;
		goto exec_error2;
	}
	//对于bin 之前没有处理过参数和环境变量  需要处理下
	if (!sh_bang) {
		p = copy_strings(envc,envp,page,p,0);
		p = copy_strings(argc,argv,page,p,0);
		if (!p) {
			retval = -ENOMEM;
			goto exec_error2;
		}
	}
/* OK, This is the point of no return */
/* note that current->library stays unchanged by an exec */
//释放当前的可执行文件inode
	if (current->executable)
		iput(current->executable);
	//设置为要执行的inode
	current->executable = inode;
	//清空信号
	current->signal = 0;
	//复位信号处理句柄
	for (i=0 ; i<32 ; i++) {
		current->sigaction[i].sa_mask = 0;
		current->sigaction[i].sa_flags = 0;
		if (current->sigaction[i].sa_handler != SIG_IGN)
			current->sigaction[i].sa_handler = NULL;
	}
	//关闭文件
	for (i=0 ; i<NR_OPEN ; i++)
		if ((current->close_on_exec>>i)&1)
			sys_close(i);
		//清空close_on_exec标志
	current->close_on_exec = 0;
		//释放所有页表
	free_page_tables(get_base(current->ldt[1]),get_limit(0x0f));
	free_page_tables(get_base(current->ldt[2]),get_limit(0x17));
	if (last_task_used_math == current)
		last_task_used_math = NULL;
	current->used_math = 0;
	//重新设置ldt  包括我们申请的环境参数page 会设置好页表
	//p从128k计算的offset 改成了从进程地址空间开始的offset  即已经转换为栈指针
	p += change_ldt(ex.a_text,page);
	p -= LIBRARY_SIZE + MAX_ARG_PAGES*PAGE_SIZE;
	//p从128k计算的offset 改成了从进程地址空间开始的offset  即已经转换为栈指针
	//  |...|128k|4M|

	//p入参时指向的时环境和参数的其实地址  这个函数 简历两个字符串指针数组argv argv
	p = (unsigned long) create_tables((char *)p,argc,envc);
	//p返回后指向argc argv envp

	//设置brk  代码长度 + 数据长度
	current->brk = ex.a_bss +
		(current->end_data = ex.a_data +
		(current->end_code = ex.a_text));
	//重新设置用户堆栈开始地址 为指针所在页面
	current->start_stack = p & 0xfffff000;
	current->suid = current->euid = e_uid;
	current->sgid = current->egid = e_gid;
	///设置用户态的ip sp
	eip[0] = ex.a_entry;		/* eip, magic happens :-) */
	eip[3] = p;			/* stack pointer */
	return 0;
exec_error2:
	iput(inode);
exec_error1:
	for (i=0 ; i<MAX_ARG_PAGES ; i++)
		free_page(page[i]);
	return(retval);
}
