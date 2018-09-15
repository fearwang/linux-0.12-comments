/*
 *  linux/fs/open.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <utime.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/kernel.h>

#include <asm/segment.h>

int sys_ustat(int dev, struct ustat * ubuf)
{
	return -ENOSYS;
}

//�����ļ��ķ��ʺ��޸�ʱ��
int sys_utime(char * filename, struct utimbuf * times)
{
	struct m_inode * inode;
	long actime,modtime;

	//ʱ�䱣����inode�� ���ҵ�inode
	if (!(inode=namei(filename)))
		return -ENOENT;
	if (times) {
		actime = get_fs_long((unsigned long *) &times->actime);
		modtime = get_fs_long((unsigned long *) &times->modtime);
	} else
		actime = modtime = CURRENT_TIME;
	//Ȼ������inode
	inode->i_atime = actime;
	inode->i_mtime = modtime;
	inode->i_dirt = 1;
	iput(inode);
	return 0;
}

/*
 * XXX should we use the real or effective uid?  BSD uses the real uid,
 * so as to make this call useful to setuid programs.
 */
 //����ļ��ķ���Ȩ��
int sys_access(const char * filename,int mode)
{
	struct m_inode * inode;
	int res, i_mode;

	mode &= 0007;
	if (!(inode=namei(filename)))
		return -EACCES;
	i_mode = res = inode->i_mode & 0777;
	iput(inode);
	if (current->uid == inode->i_uid)
		res >>= 6;
	else if (current->gid == inode->i_gid)
		res >>= 6;
	if ((res & 0007 & mode) == mode)
		return 0;
	/*
	 * XXX we are doing this test last because we really should be
	 * swapping the effective with the real user id (temporarily),
	 * and then calling suser() routine.  If we do call the
	 * suser() routine, it needs to be called last. 
	 */
	if ((!current->uid) &&
	    (!(mode & 1) || (i_mode & 0111)))
		return 0;
	return -EACCES;
}

//�л�pwd
int sys_chdir(const char * filename)
{
	struct m_inode * inode;

	if (!(inode = namei(filename)))
		return -ENOENT;
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}
	iput(current->pwd);
	current->pwd = inode;
	return (0);
}

//�л�root dir
int sys_chroot(const char * filename)
{
	struct m_inode * inode;

	if (!(inode=namei(filename)))
		return -ENOENT;
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}
	iput(current->root);
	current->root = inode;
	return (0);
}

//�޸��ļ�����Ȩ��  д��inode
int sys_chmod(const char * filename,int mode)
{
	struct m_inode * inode;

	if (!(inode=namei(filename)))
		return -ENOENT;
	if ((current->euid != inode->i_uid) && !suser()) {
		iput(inode);
		return -EACCES;
	}
	inode->i_mode = (mode & 07777) | (inode->i_mode & ~07777);
	inode->i_dirt = 1;
	iput(inode);
	return 0;
}

int sys_chown(const char * filename,int uid,int gid)
{
	struct m_inode * inode;

	if (!(inode=namei(filename)))
		return -ENOENT;
	if (!suser()) {
		iput(inode);
		return -EACCES;
	}
	inode->i_uid=uid;
	inode->i_gid=gid;
	inode->i_dirt=1;
	iput(inode);
	return 0;
}

//����ַ��豸  tty��� ����
static int check_char_dev(struct m_inode * inode, int dev, int flag)
{
	struct tty_struct *tty;
	int min;

	if (MAJOR(dev) == 4 || MAJOR(dev) == 5) {
		if (MAJOR(dev) == 5)
			min = current->tty;
		else
			min = MINOR(dev);
		if (min < 0)
			return -1;
		if ((IS_A_PTY_MASTER(min)) && (inode->i_count>1))
			return -1;
		tty = TTY_TABLE(min);
		if (!(flag & O_NOCTTY) &&
		    current->leader &&
		    current->tty<0 &&
		    tty->session==0) {
			current->tty = min;
			tty->session= current->session;
			tty->pgrp = current->pgrp;
		}
		if (flag & O_NONBLOCK) {
			TTY_TABLE(min)->termios.c_cc[VMIN] =0;
			TTY_TABLE(min)->termios.c_cc[VTIME] =0;
			TTY_TABLE(min)->termios.c_lflag &= ~ICANON;
		}
	}
	return 0;
}

//���ļ�
int sys_open(const char * filename,int flag,int mode)
{
	struct m_inode * inode;
	struct file * f;
	int i,fd;

	//����umask
	mode &= 0777 & ~current->umask;
	//�ҵ�һ�����е�fdָ��
	for(fd=0 ; fd<NR_OPEN ; fd++)
		if (!current->filp[fd])
			break;
		//�������̿��Դ򿪵��ļ����� �˳�
	if (fd>=NR_OPEN)
		return -EINVAL;
	//Ĭ��Ҫ��λclose on exec
	current->close_on_exec &= ~(1<<fd);
	// fָ��file tableͷ��
	f=0+file_table;
	//��ϵͳ��file table���ҿ��е�struct file
	for (i=0 ; i<NR_FILE ; i++,f++)
		if (!f->f_count) break;
	if (i>=NR_FILE)
		return -EINVAL;
	//�����ҵ���file�ļ���
	(current->filp[fd]=f)->f_count++;
	// �����Ĵ򿪲���
	if ((i=open_namei(filename,flag,mode,&inode))<0) {
		current->filp[fd]=NULL;
		f->f_count=0;
		return i;
	}
/* ttys are somewhat special (ttyxx major==4, tty major==5) */
	if (S_ISCHR(inode->i_mode))
		// ������ַ��豸 ����Ҫ��һЩ���⴦��
		if (check_char_dev(inode,inode->i_zone[0],flag)) {
			iput(inode);
			current->filp[fd]=NULL;
			f->f_count=0;
			return -EAGAIN;
		}
/* Likewise with block-devices: check for floppy_change */
	if (S_ISBLK(inode->i_mode))
		//����ǿ��豸 ������Ƭ�Ƿ������
		check_disk_change(inode->i_zone[0]);
	//���file�ṹ��
	f->f_mode = inode->i_mode;
	f->f_flags = flag;
	f->f_count = 1;
	f->f_inode = inode;
	f->f_pos = 0;
	return (fd);
}

int sys_creat(const char * pathname, int mode)
{
	return sys_open(pathname, O_CREAT | O_TRUNC, mode);
}

//ȥ��fd��Ӧ��file�ṹ��Ͷ�Ӧfile����ϵ�����file�����ü�Ϊ�� ���ͷ�inode
int sys_close(unsigned int fd)
{	
	struct file * filp;

	if (fd >= NR_OPEN)
		return -EINVAL;
	current->close_on_exec &= ~(1<<fd);
	if (!(filp = current->filp[fd]))
		return -EINVAL;
	current->filp[fd] = NULL;
	if (filp->f_count == 0)
		panic("Close: file count is 0");
	//���б��ָ��ָ���file�ṹ��  ֱ�ӷ���
	if (--filp->f_count)
		return (0);
	//������ͷ�file�ṹ���е�inode
	iput(filp->f_inode);
	return (0);
}
