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
 //��������������Ͳ��������ҳ������
#define MAX_ARG_PAGES 32

//������ѡ��һ�����ļ� ���滻��ǰ�Ŀ��ļ�
int sys_uselib(const char * library)
{
	struct m_inode * inode;
	unsigned long base;

	//�ж��Ƿ�����ͨ����
	if (get_limit(0x17) != TASK_SIZE)
		return -EINVAL;
	//�������ƶ��˿��ļ� ��õ�inode
	if (library) {
		if (!(inode=namei(library)))		/* get library inode */
			return -ENOENT;
	} else
		inode = NULL;
/* we should check filetypes (headers etc), but we don't */
//���ͷŵ�ǰ�Ŀ��ļ�inode
	iput(current->library);
//
	current->library = NULL;
//�õ����̵Ļ���ַ
	base = get_base(current->ldt[2]);
//���ļ�ƫ�Ƶ�ַ
	base += LIBRARY_OFFSET;
//�ͷſ��ļ�ӳ�������ҳ��
	free_page_tables(base,LIBRARY_SIZE);
	current->library = inode;
	return 0;
}

/*
 * create_tables() parses the env- and arg-strings in new user
 * memory and creates the pointer tables from them, and puts their
 * addresses on the "stack", returning the new stack pointer value.
 */
 //��������ջ�д������������Ͳ���ָ���   �����ͻ��������������û��ռ�
static unsigned long * create_tables(char * p,int argc,int envc)
{
	unsigned long *argv,*envp;
	unsigned long * sp;
	//4byte����
	sp = (unsigned long *) (0xfffffffc & (unsigned long) p);
	//Ԥ��ָ��������ָ�� ��һ��null�Ŀռ�
	sp -= envc+1;
	envp = sp;
	sp -= argc+1;
	argv = sp;
	//��argc argv  envpָ��ѹ��ջ�� �û�ջ
	put_fs_long((unsigned long)envp,--sp);
	put_fs_long((unsigned long)argv,--sp);
	put_fs_long((unsigned long)argc,--sp);
	//��������
	while (argc-->0) {
		put_fs_long((unsigned long) p,argv++);
		while (get_fs_byte(p++)) /* nothing */ ;
	}
	put_fs_long(0,argv);
	//������������
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
 //�����������
static int count(char ** argv)
{
	int i=0;
	char ** tmp;

//���ں��� fsָ���û���ջ  �û�̬���ݶ�
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
 //����ָ�������Ĳ����ַ����� �����ͻ����ռ���
static unsigned long copy_strings(int argc,char ** argv,unsigned long *page,
		unsigned long p, int from_kmem /* �ȿ���from_Kem=0 �����ַ����������û��ռ� */)
{
	char *tmp, *pag;
	int len, offset = 0;
	unsigned long old_fs, new_fs;
	//p �ַ�������Ϊ0 ���˳�
	if (!p)
		return 0;	/* bullet-proofing */
	//�õ�ds �� fs �ֱ�ָ���û�data���ں�data
	new_fs = get_ds();
	old_fs = get_fs();
	//����ַ������ַ�ָ�������ں� ����������fsָ���ں�data��
	if (from_kmem==2)
		set_fs(new_fs);
	while (argc-- > 0) {
		if (from_kmem == 1)
			set_fs(new_fs);
		//�����ǲ��������� (unsigned long *)(argv + argc)
		if (!(tmp = (char *)get_fs_long(((unsigned long *)argv)+argc)))
			panic("argc is wrong");
		if (from_kmem == 1)
			set_fs(old_fs);
		len=0;		/* remember zero-padding */
		do {
			//���㵱ǰ�ַ�������
			len++;
		} while (get_fs_byte(tmp++));
		//�ռ䲻����
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
//���ļ�ռ��ַ�ռ����4M
	data_base += data_limit - LIBRARY_SIZE;
	for (i=MAX_ARG_PAGES-1 ; i>=0 ; i--) {
		//Ȼ�󽫲����Ϳ��ļ��ռ����Ѿ�������ݵ�ҳ �ŵ����ݶ�ĩ��
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
int do_execve(unsigned long * eip /* ����ϵͳ���õĽ��̵Ĵ���ָ�� */,long tmp /* �����ñ������ķ��ص�ַ ���� */,char * filename,
	char ** argv, char ** envp /* ���������û��ռ䴫����  ������ȷ */)
{
	struct m_inode * inode;
	struct buffer_head * bh;
	struct exec ex;
	unsigned long page[MAX_ARG_PAGES];
	int i,argc,envc;
	int e_uid, e_gid;
	int retval;
	int sh_bang = 0;
	//pָ������ռ�����һ��4byte��
	unsigned long p=PAGE_SIZE*MAX_ARG_PAGES-4;

	//cs�Ƿ�ָ���û���
	if ((0xffff & eip[1]) != 0x000f)
		panic("execve called from supervisor mode");
	//��01 page����
	for (i=0 ; i<MAX_ARG_PAGES ; i++)	/* clear page-table */
		page[i]=0;
	//�õ�Ҫִ���ļ���inode
	if (!(inode=namei(filename)))		/* get executables inode */
		return -ENOENT;
	//������� �� ���������ĸ���
	argc = count(argv);
	envc = count(envp);
	
restart_interp:
	//���ǳ����ļ� �޷�ִ��
	if (!S_ISREG(inode->i_mode)) {	/* must be regular file */
		retval = -EACCES;
		goto exec_error2;
	}
	i = inode->i_mode;
	//set user id �� set group id ��λ�Ļ� ��uid egid ����Ϊ�ļ���uid gid
	e_uid = (i & S_ISUID) ? inode->i_uid : current->euid;
	e_gid = (i & S_ISGID) ? inode->i_gid : current->egid;
	//ȡ��rwx
	if (current->euid == inode->i_uid)
		i >>= 6;
	else if (in_group_p(inode->i_gid))
		i >>= 3;
	if (!(i & 1) && /* ��ǰ������Ȩ�� */
		//���ļ�û��x���� �� ����root����
	    !((inode->i_mode & 0111) && suser())) {
	    //������
		retval = -ENOEXEC;
		goto exec_error2;
	}
	//Ȼ���ȡ��ִ���ļ��� ��һ������ڴ�
	if (!(bh = bread(inode->i_dev,inode->i_zone[0]))) {
		retval = -EACCES;
		goto exec_error2;
	}
	//ȡ���ļ�ͷ��
	ex = *((struct exec *) bh->b_data);	/* read exec-header */
	if ((bh->b_data[0] == '#') && (bh->b_data[1] == '!') && (!sh_bang)) {
		//����sh�ű��ļ�
		/*
		 * This section does the #! interpretation.
		 * Sorta complicated, but hopefully it will work.  -TYT
		 */

		char buf[128], *cp, *interp, *i_name, *i_arg;
		unsigned long old_fs;
		//�������ִ��sh�Ľ����� �� ����(����еĻ�)
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
		 //ʹ�ý�������inode ��������
		old_fs = get_fs();
		set_fs(get_ds());
		//�õ�������inode Ȼ��exec�����������inode
		if (!(inode=namei(interp))) { /* get executables inode */
			set_fs(old_fs);
			retval = -ENOENT;
			goto exec_error1;
		}
		set_fs(old_fs);
		goto restart_interp;
	}
	//��ʱ�ļ�ͷ�Ѿ����Ƴ����� �������ͷ�buffer
	brelse(bh);
	//�жϲ�����֧�����е����
	if (N_MAGIC(ex) != ZMAGIC || ex.a_trsize || ex.a_drsize ||
		ex.a_text+ex.a_data+ex.a_bss>0x3000000 ||
		inode->i_size < ex.a_text+ex.a_data+ex.a_syms+N_TXTOFF(ex)) {
		retval = -ENOEXEC;
		goto exec_error2;
	}
	//���뿪ʼ�� û��һҳ���� 
	if (N_TXTOFF(ex) != BLOCK_SIZE) {
		printk("%s: N_TXTOFF != BLOCK_SIZE. See a.out.h.", filename);
		retval = -ENOEXEC;
		goto exec_error2;
	}
	//����bin ֮ǰû�д���������ͻ�������  ��Ҫ������
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
//�ͷŵ�ǰ�Ŀ�ִ���ļ�inode
	if (current->executable)
		iput(current->executable);
	//����ΪҪִ�е�inode
	current->executable = inode;
	//����ź�
	current->signal = 0;
	//��λ�źŴ�����
	for (i=0 ; i<32 ; i++) {
		current->sigaction[i].sa_mask = 0;
		current->sigaction[i].sa_flags = 0;
		if (current->sigaction[i].sa_handler != SIG_IGN)
			current->sigaction[i].sa_handler = NULL;
	}
	//�ر��ļ�
	for (i=0 ; i<NR_OPEN ; i++)
		if ((current->close_on_exec>>i)&1)
			sys_close(i);
		//���close_on_exec��־
	current->close_on_exec = 0;
		//�ͷ�����ҳ��
	free_page_tables(get_base(current->ldt[1]),get_limit(0x0f));
	free_page_tables(get_base(current->ldt[2]),get_limit(0x17));
	if (last_task_used_math == current)
		last_task_used_math = NULL;
	current->used_math = 0;
	//��������ldt  ������������Ļ�������page �����ú�ҳ��
	//p��128k�����offset �ĳ��˴ӽ��̵�ַ�ռ俪ʼ��offset  ���Ѿ�ת��Ϊջָ��
	p += change_ldt(ex.a_text,page);
	p -= LIBRARY_SIZE + MAX_ARG_PAGES*PAGE_SIZE;
	//p��128k�����offset �ĳ��˴ӽ��̵�ַ�ռ俪ʼ��offset  ���Ѿ�ת��Ϊջָ��
	//  |...|128k|4M|

	//p���ʱָ���ʱ�����Ͳ�������ʵ��ַ  ������� ���������ַ���ָ������argv argv
	p = (unsigned long) create_tables((char *)p,argc,envc);
	//p���غ�ָ��argc argv envp

	//����brk  ���볤�� + ���ݳ���
	current->brk = ex.a_bss +
		(current->end_data = ex.a_data +
		(current->end_code = ex.a_text));
	//���������û���ջ��ʼ��ַ Ϊָ������ҳ��
	current->start_stack = p & 0xfffff000;
	current->suid = current->euid = e_uid;
	current->sgid = current->egid = e_gid;
	///�����û�̬��ip sp
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
