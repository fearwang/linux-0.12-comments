/*
 *  linux/fs/namei.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * Some corrections by tytso.
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <const.h>
#include <sys/stat.h>

static struct m_inode * _namei(const char * filename, struct m_inode * base,
	int follow_links);

#define ACC_MODE(x) ("\004\002\006\377"[(x)&O_ACCMODE])

/*
 * comment out this line if you want names > NAME_LEN chars to be
 * truncated. Else they will be disallowed.
 */
/* #define NO_TRUNCATE */

#define MAY_EXEC 1
#define MAY_WRITE 2
#define MAY_READ 4

/*
 *	permission()
 *
 * is used to check for read/write/execute permissions on a file.
 * I don't know if we should look at just the euid or both euid and
 * uid, but that should be easily changed.
 */
 //判断是否有访问权限
static int permission(struct m_inode * inode,int mask)
{
	int mode = inode->i_mode;

/* special case: not even root can read/write a deleted file */
//对于被删除的文件  直接返回0
	if (inode->i_dev && !inode->i_nlinks)
		return 0;
	//根据进程的id对比文件的uid gid 来决定和mode中的哪些bit比较
	else if (current->euid==inode->i_uid)
		mode >>= 6;
	else if (in_group_p(inode->i_gid))
		mode >>= 3;
	//如果比较通过 或者 是root 则允许访问 返回1
	if (((mode & mask & 0007) == mask) || suser())
		return 1;
	return 0;
}

/*
 * ok, we cannot use strncmp, as the name is not in our data space.
 * Thus we'll have to use match. No big problem. Match also makes
 * some sanity tests.
 *
 * NOTE! unlike strncmp, match returns 1 for success, 0 for failure.
 */
//指定长度字符串比较 相等返回1， 因为buffer在用户空间 这里特殊处理了
static int match(int len,const char * name,struct dir_entry * de)
{
	register int same __asm__("ax");

	if (!de || !de->inode || len > NAME_LEN)
		return 0;
	/* "" means "." ---> so paths like "/usr/lib//libc.a" work */
	if (!len && (de->name[0]=='.') && (de->name[1]=='\0'))
		return 1;
	if (len < NAME_LEN && de->name[len])
		return 0;
	__asm__("cld\n\t"
		"fs ; repe ; cmpsb\n\t"
		"setz %%al"
		:"=a" (same)
		:"0" (0),"S" ((long) name),"D" ((long) de->name),"c" (len)
		:"cx","di","si");
	return same;
}

/*
 *	find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 *
 * This also takes care of the few special cases due to '..'-traversal
 * over a pseudo-root and a mount point.
 */
 //查找指定目录中指定文件名的inode
static struct buffer_head * find_entry(struct m_inode ** dir,
	const char * name, int namelen, struct dir_entry ** res_dir)
{
	int entries;
	int block,i;
	struct buffer_head * bh;
	struct dir_entry * de;
	struct super_block * sb;

#ifdef NO_TRUNCATE
	if (namelen > NAME_LEN)
		return NULL;
#else
	if (namelen > NAME_LEN)
		namelen = NAME_LEN;
#endif
//目录中dentry的数目
	entries = (*dir)->i_size / (sizeof (struct dir_entry));
	*res_dir = NULL;
/* check for '..', as we might have to do some "magic" for it */
	if (namelen==2 && get_fs_byte(name)=='.' && get_fs_byte(name+1)=='.') {
/* '..' in a pseudo-root results in a faked '.' (just change namelen) */
//当前目录是当前进程的根目录 则..等于.
		if ((*dir) == current->root)
			namelen=1;
		//当前目录是一个文件系统的根目录
		else if ((*dir)->i_num == ROOT_INO) {
/* '..' over a mount-point results in 'dir' being exchanged for the mounted
   directory-inode. NOTE! We set mounted, so that we can iput the new dir */
			sb=get_super((*dir)->i_dev);
			if (sb->s_imount) {
				iput(*dir);
				//dir重新设置为挂载点的inode 完成偷梁换柱
				(*dir)=sb->s_imount;
				(*dir)->i_count++;
			}
		}
	}
	if (!(block = (*dir)->i_zone[0]))
		return NULL;
	if (!(bh = bread((*dir)->i_dev,block)))
		return NULL;
	i = 0;
	de = (struct dir_entry *) bh->b_data;
	//循环查找匹配的 dentry ，会陆续读入数据块
	while (i < entries) {
		if ((char *)de >= BLOCK_SIZE+bh->b_data) {
			brelse(bh);
			bh = NULL;
			//根据在目录数据区的块号，求出在块设备上的块号
			if (!(block = bmap(*dir,i/DIR_ENTRIES_PER_BLOCK)) ||
			    !(bh = bread((*dir)->i_dev,block))) {
				i += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			de = (struct dir_entry *) bh->b_data;
		}
		if (match(namelen,name,de)) {
			//找到则通过指针返回
			*res_dir = de;
			return bh;
		}
		de++;
		i++;
	}
	brelse(bh);
	return NULL;
}

/*
 *	add_entry()
 *
 * adds a file entry to the specified directory, using the same
 * semantics as find_entry(). It returns NULL if it failed.
 *
 * NOTE!! The inode part of 'de' is left at 0 - which means you
 * may not sleep between calling this and putting something into
 * the entry, as someone else might have used it while you slept.
 */
 //指定目录中添加一个dentry 类似find_entry
static struct buffer_head * add_entry(struct m_inode * dir,
	const char * name, int namelen, struct dir_entry ** res_dir)
{
	int block,i;
	struct buffer_head * bh;
	struct dir_entry * de;

	*res_dir = NULL;
#ifdef NO_TRUNCATE
	if (namelen > NAME_LEN)
		return NULL;
#else
	if (namelen > NAME_LEN)
		namelen = NAME_LEN;
#endif
	if (!namelen)
		return NULL;
	if (!(block = dir->i_zone[0]))
		return NULL;
	if (!(bh = bread(dir->i_dev,block)))
		return NULL;
	i = 0;
	de = (struct dir_entry *) bh->b_data;
	//循环查找是否有空闲的dentry(因为可能释放)
	while (1) {
		if ((char *)de >= BLOCK_SIZE+bh->b_data) {
			brelse(bh);
			bh = NULL;
			//超过1块都没有空闲的  搞一个新的 可能是新创建的
			block = create_block(dir,i/DIR_ENTRIES_PER_BLOCK);
			if (!block)
				return NULL;
			//将新搞到的块 读进来
			if (!(bh = bread(dir->i_dev,block))) {
				i += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			de = (struct dir_entry *) bh->b_data;
		}
		//在末尾添加一个dentry，没有空闲的
		if (i*sizeof(struct dir_entry) >= dir->i_size) {
			de->inode=0;
			dir->i_size = (i+1)*sizeof(struct dir_entry);
			dir->i_dirt = 1;
			dir->i_ctime = CURRENT_TIME;
		}
		//找到空闲的dentry 
		if (!de->inode) {
			//设置目录的修改时间
			dir->i_mtime = CURRENT_TIME;
			for (i=0; i < NAME_LEN ; i++)
				//拷贝名字到dentry
				de->name[i]=(i<namelen)?get_fs_byte(name+i):0;
			//buffer dirty
			bh->b_dirt = 1;
			*res_dir = de;
			return bh;
		}
		de++;
		i++;
	}
	brelse(bh);
	return NULL;
}

//返回符号链接所指向的文件的inode
static struct m_inode * follow_link(struct m_inode * dir, struct m_inode * inode)
{
	unsigned short fs;
	struct buffer_head * bh;

	//没指定dir 从root开始
	if (!dir) {
		dir = current->root;
		dir->i_count++;
	}
	//没有指定符号链接的inode 返回
	if (!inode) {
		iput(dir);
		return NULL;
	}
	//指定的inode 并不是一个符号链接 直接返回自己
	if (!S_ISLNK(inode->i_mode)) {
		iput(dir);
		return inode;
	}
	__asm__("mov %%fs,%0":"=r" (fs));
	//
	if (fs != 0x17 || !inode->i_zone[0] ||
	   !(bh = bread(inode->i_dev, inode->i_zone[0]))) {
		iput(dir);
		iput(inode);
		return NULL;
	}
	//到这里，bh中应该就是符号链接指向的文件的路径
	iput(inode);
	__asm__("mov %0,%%fs"::"r" ((unsigned short) 0x10));
	//软连接 不是可以跨越文件系统的么， 怎么这里还指定了dir
	inode = _namei(bh->b_data,dir,0);
	__asm__("mov %0,%%fs"::"r" (fs));
	brelse(bh);
	return inode;
}

/*
 *	get_dir()
 *
 * Getdir traverses the pathname until it hits the topmost directory.
 * It returns NULL on failure.
 */
 //从指定的目录开始搜索指定路径名的目录的i节点, 
 // 一致搜索到最后一层 /a/b/c/d/x ->x是文件，我们取出d的inode
static struct m_inode * get_dir(const char * pathname, struct m_inode * inode)
{
	char c;
	const char * thisname;
	struct buffer_head * bh;
	int namelen,inr;
	struct dir_entry * de;
	struct m_inode * dir;

	if (!inode) {
		inode = current->pwd;
		inode->i_count++;
	}
	//从根目录开始所搜
	if ((c=get_fs_byte(pathname))=='/') {
		iput(inode);
		inode = current->root;
		//跳过'/'
		pathname++;
		inode->i_count++;
	}
	while (1) {
		thisname = pathname;
		//不是目录 或者 没有权限  返回
		if (!S_ISDIR(inode->i_mode) || !permission(inode,MAY_EXEC)) {
			iput(inode);
			return NULL;
		}
		//一直到到下一个'/'
		for(namelen=0;(c=get_fs_byte(pathname++))&&(c!='/');namelen++)
			/* nothing */ ;
		//如果已经到pathname的最后了，且最后一个字节不是'/' 则直接返回当前的inode
		if (!c)
			return inode;
		//否则还需继续在当前dentry中寻找，pathname中的name对应的inode
		if (!(bh = find_entry(&inode,thisname,namelen,&de))) {
			iput(inode);
			return NULL;
		}
		//取出找到的dentry的inode号
		inr = de->inode;
		brelse(bh);
		
		dir = inode;
		//根据inode号 得到inode结构体
		if (!(inode = iget(dir->i_dev,inr))) {
			iput(dir);
			return NULL;
		}
		//处理符号链接
		if (!(inode = follow_link(dir,inode)))
			return NULL;
	}
}

/*
 *	dir_namei()
 *
 * dir_namei() returns the inode of the directory of the
 * specified name, and the name within that directory.
 */
 //找到pathname对应的最末端的目录，和文件名(去除多余部分 只留末端的目录中的文件名)
 // /a/b/c/1 返回的是c的inode 和字符串'1'
static struct m_inode * dir_namei(const char * pathname,
	int * namelen, const char ** name, struct m_inode * base)
{
	char c;
	const char * basename;
	struct m_inode * dir;
	//返回pathname对应的目录的dentry
	if (!(dir = get_dir(pathname,base)))
		return NULL;
	basename = pathname;
	//然后再/a/b/c/v这样的路径中 取出v
	while (c=get_fs_byte(pathname++))
		if (c=='/')
			basename=pathname;
	*namelen = pathname-basename-1;
	*name = basename;
	return dir;
}

//取指定路径名称的inode
struct m_inode * _namei(const char * pathname, struct m_inode * base,
	int follow_links)
{
	const char * basename;
	int inr,namelen;
	struct m_inode * inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	//先找到末端的目录的inode，basename指向末端目录中的文件名
	if (!(base = dir_namei(pathname,&namelen,&basename,base)))
		return NULL;
	//对于 /usr/ 返回目录的inode
	if (!namelen)			/* special case: '/usr/' etc */
		return base;
	//pathname最后一部分是不是以'/'结尾 则找到最后一部分对应的dentry
	bh = find_entry(&base,basename,namelen,&de);
	if (!bh) {
		iput(base);
		return NULL;
	}
	//找到的dentry中取出inode号
	inr = de->inode;
	brelse(bh);
	//根据inode号 取出inode
	if (!(inode = iget(base->i_dev,inr))) {
		iput(base);
		return NULL;
	}
	if (follow_links)
		inode = follow_link(base,inode);
	else
		iput(base);
	//修改inode的访问时间  并使其dirty
	inode->i_atime=CURRENT_TIME;
	inode->i_dirt=1;
	return inode;
}

//不跟随符号链接 找pathname对应的inode
struct m_inode * lnamei(const char * pathname)
{
	return _namei(pathname, NULL, 0);
}

/*
 *	namei()
 *
 * is used by most simple commands to get the inode of a specified name.
 * Open, link etc use their own routines, but this is enough for things
 * like 'chmod' etc.
 */
 //跟随符号链接 找pathname对应的inode
struct m_inode * namei(const char * pathname)
{
	return _namei(pathname,NULL,1);
}

/*
 *	open_namei()
 *
 * namei for open - this is in fact almost the whole open-routine.
 */
int open_namei(const char * pathname, int flag, int mode,
	struct m_inode ** res_inode)
{
	const char * basename;
	int inr,dev,namelen;
	struct m_inode * dir, *inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	if ((flag & O_TRUNC) && !(flag & O_ACCMODE))
		flag |= O_WRONLY;
	mode &= 0777 & ~current->umask;
	mode |= I_REGULAR;
	//找到指定文件路径所在目录的dentry  找不到则认为 文件所在目录不存在 return
	if (!(dir = dir_namei(pathname,&namelen,&basename,NULL)))
		return -ENOENT;
	if (!namelen) {			/* special case: '/usr/' etc */
		if (!(flag & (O_ACCMODE|O_CREAT|O_TRUNC))) {
			*res_inode=dir;
			return 0;
		}
		iput(dir);
		return -EISDIR;
	}
	//在找到的目录中查找指定的name
	bh = find_entry(&dir,basename,namelen,&de);
	if (!bh) {//包含文件的dentry的buffer为null 代表文件不存在  此时只能是创建文件
		if (!(flag & O_CREAT)) {
			//不是创建文件 返回
			iput(dir);
			return -ENOENT;
		}
		//对父目录没有写权限  无法创建文件
		if (!permission(dir,MAY_WRITE)) {
			iput(dir);
			return -EACCES;
		}
		//创建文件  则new inode
		inode = new_inode(dir->i_dev);
		if (!inode) {
			iput(dir);
			return -ENOSPC;
		}
		//设置并填充到父目录中
		inode->i_uid = current->euid;
		inode->i_mode = mode;
		inode->i_dirt = 1;
		bh = add_entry(dir,basename,namelen,&de);
		//add失败处理
		if (!bh) {
			inode->i_nlinks--;
			iput(inode);
			iput(dir);
			return -ENOSPC;
		}
		//如果是创建文件  则之前find entry得到的dentry是空的，这里设置inode号
		de->inode = inode->i_num;
		bh->b_dirt = 1;
		brelse(bh);
		iput(dir);
		*res_inode = inode;
		return 0;
	}
	//走到这里 代表文件存在  取出inode号 所在设备号
	inr = de->inode;
	dev = dir->i_dev;
	brelse(bh);
	if (flag & O_EXCL) {
		iput(dir);
		return -EEXIST;
	}
	//处理符号链接
	if (!(inode = follow_link(dir,iget(dev,inr))))
		return -EACCES;
	//权限处理
	if ((S_ISDIR(inode->i_mode) && (flag & O_ACCMODE)) ||
	    !permission(inode,ACC_MODE(flag))) {
		iput(inode);
		return -EPERM;
	}
	//到这里 打开成功 修改inode的访问时间
	inode->i_atime = CURRENT_TIME;
	//截断操作
	if (flag & O_TRUNC)
		truncate(inode);
	*res_inode = inode;
	return 0;
}

int sys_mknod(const char * filename, int mode, int dev)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;
	
	if (!suser())
		return -EPERM;
	//父目录不存在
	if (!(dir = dir_namei(filename,&namelen,&basename, NULL)))
		return -ENOENT;
	//说明 /usr/
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	//没有父目录的写权限
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	//find entry
	bh = find_entry(&dir,basename,namelen,&de);
	//已经存在指定名称的文件
	if (bh) {
		brelse(bh);
		iput(dir);
		return -EEXIST;
	}
	//new inode
	inode = new_inode(dir->i_dev);
	if (!inode) {
		iput(dir);
		return -ENOSPC;
	}
	//设置
	inode->i_mode = mode;
	if (S_ISBLK(mode) || S_ISCHR(mode))
		inode->i_zone[0] = dev;
	inode->i_mtime = inode->i_atime = CURRENT_TIME;
	inode->i_dirt = 1;
	//添加到父目录
	bh = add_entry(dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		inode->i_nlinks=0;
		iput(inode);
		return -ENOSPC;
	}
	//设置新增的dentry的inode 号字段
	de->inode = inode->i_num;
	bh->b_dirt = 1;
	iput(dir);
	iput(inode);
	brelse(bh);
	return 0;
}

int sys_mkdir(const char * pathname, int mode)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh, *dir_block;
	struct dir_entry * de;

	if (!(dir = dir_namei(pathname,&namelen,&basename, NULL)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	bh = find_entry(&dir,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		return -EEXIST;
	}
	inode = new_inode(dir->i_dev);
	if (!inode) {
		iput(dir);
		return -ENOSPC;
	}
	inode->i_size = 32;  //数据区的长度 对应于 . 和 ..
	inode->i_dirt = 1;
	inode->i_mtime = inode->i_atime = CURRENT_TIME;
	//申请一个block做数据区
	if (!(inode->i_zone[0]=new_block(inode->i_dev))) {
		iput(dir);
		inode->i_nlinks--;
		iput(inode);
		return -ENOSPC;
	}
	inode->i_dirt = 1;
	//读进来block
	if (!(dir_block=bread(inode->i_dev,inode->i_zone[0]))) {
		iput(dir);
		inode->i_nlinks--;
		iput(inode);
		return -ERROR;
	}
	//设置..和 .的数据
	de = (struct dir_entry *) dir_block->b_data;
	de->inode=inode->i_num;
	strcpy(de->name,".");
	de++;
	de->inode = dir->i_num;
	strcpy(de->name,"..");
	inode->i_nlinks = 2;
	dir_block->b_dirt = 1;
	brelse(dir_block);
	inode->i_mode = I_DIRECTORY | (mode & 0777 & ~current->umask);
	inode->i_dirt = 1;
	//新创建的目录的deenrty添加到自己的父目录中
	bh = add_entry(dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		inode->i_nlinks=0;
		iput(inode);
		return -ENOSPC;
	}
	//设置新添加的dentry
	de->inode = inode->i_num;
	bh->b_dirt = 1;
	dir->i_nlinks++;
	dir->i_dirt = 1;
	iput(dir);
	iput(inode);
	brelse(bh);
	return 0;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
 //检查目录是否为空
static int empty_dir(struct m_inode * inode)
{
	int nr,block;
	int len;
	struct buffer_head * bh;
	struct dir_entry * de;

	len = inode->i_size / sizeof (struct dir_entry);
	if (len<2 || !inode->i_zone[0] ||
	    !(bh=bread(inode->i_dev,inode->i_zone[0]))) {
	    	printk("warning - bad directory on dev %04x\n",inode->i_dev);
		return 0;
	}
	de = (struct dir_entry *) bh->b_data;
	if (de[0].inode != inode->i_num || !de[1].inode || 
	    strcmp(".",de[0].name) || strcmp("..",de[1].name)) {
	    	printk("warning - bad directory on dev %04x\n",inode->i_dev);
		return 0;
	}
	nr = 2;
	de += 2;
	while (nr<len) {
		if ((void *) de >= (void *) (bh->b_data+BLOCK_SIZE)) {
			brelse(bh);
			block=bmap(inode,nr/DIR_ENTRIES_PER_BLOCK);
			if (!block) {
				nr += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			if (!(bh=bread(inode->i_dev,block)))
				return 0;
			de = (struct dir_entry *) bh->b_data;
		}
		if (de->inode) {
			brelse(bh);
			return 0;
		}
		de++;
		nr++;
	}
	brelse(bh);
	return 1;
}

//删除目录
int sys_rmdir(const char * name)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	if (!(dir = dir_namei(name,&namelen,&basename, NULL)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	bh = find_entry(&dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		return -ENOENT;
	}
	if (!(inode = iget(dir->i_dev, de->inode))) {
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
	if ((dir->i_mode & S_ISVTX) && current->euid &&
	    inode->i_uid != current->euid) {
		iput(dir);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
	if (inode->i_dev != dir->i_dev || inode->i_count>1) {
		iput(dir);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
	if (inode == dir) {	/* we may not delete ".", but "../dir" is ok */
		iput(inode);
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		iput(dir);
		brelse(bh);
		return -ENOTDIR;
	}
	if (!empty_dir(inode)) {
		iput(inode);
		iput(dir);
		brelse(bh);
		return -ENOTEMPTY;
	}
	if (inode->i_nlinks != 2)
		printk("empty directory has nlink!=2 (%d)",inode->i_nlinks);
	de->inode = 0;
	bh->b_dirt = 1;
	brelse(bh);
	inode->i_nlinks=0;
	inode->i_dirt=1;
	dir->i_nlinks--;
	dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	dir->i_dirt=1;
	iput(dir);
	iput(inode);
	return 0;
}

int sys_unlink(const char * name)
{
	const char * basename;
	int namelen;
	struct m_inode * dir, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	if (!(dir = dir_namei(name,&namelen,&basename, NULL)))
		return -ENOENT;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EPERM;
	}
	//文件不存在
	bh = find_entry(&dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		return -ENOENT;
	}
	//获取文件对应的inode
	if (!(inode = iget(dir->i_dev, de->inode))) {
		iput(dir);
		brelse(bh);
		return -ENOENT;
	}
	//权限判断
	if ((dir->i_mode & S_ISVTX) && !suser() &&
	    current->euid != inode->i_uid &&
	    current->euid != dir->i_uid) {
		iput(dir);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
	if (S_ISDIR(inode->i_mode)) {
		iput(inode);
		iput(dir);
		brelse(bh);
		return -EPERM;
	}
	if (!inode->i_nlinks) {
		printk("Deleting nonexistent file (%04x:%d), %d\n",
			inode->i_dev,inode->i_num,inode->i_nlinks);
		inode->i_nlinks=1;
	}
	//释放demtry
	de->inode = 0;
	bh->b_dirt = 1;
	brelse(bh);
	inode->i_nlinks--;
	inode->i_dirt = 1;
	inode->i_ctime = CURRENT_TIME;
	//iput如果发现link数为0 则会删除文件 释放磁盘空间
	iput(inode);
	iput(dir);
	return 0;
}

int sys_symlink(const char * oldname, const char * newname)
{
	struct dir_entry * de;
	struct m_inode * dir, * inode;
	struct buffer_head * bh, * name_block;
	const char * basename;
	int namelen, i;
	char c;

	dir = dir_namei(newname,&namelen,&basename, NULL);
	if (!dir)
		return -EACCES;
	if (!namelen) {
		iput(dir);
		return -EPERM;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		return -EACCES;
	}
	if (!(inode = new_inode(dir->i_dev))) {
		iput(dir);
		return -ENOSPC;
	}
	inode->i_mode = S_IFLNK | (0777 & ~current->umask);
	inode->i_dirt = 1;
	if (!(inode->i_zone[0]=new_block(inode->i_dev))) {
		iput(dir);
		inode->i_nlinks--;
		iput(inode);
		return -ENOSPC;
	}
	inode->i_dirt = 1;
	if (!(name_block=bread(inode->i_dev,inode->i_zone[0]))) {
		iput(dir);
		inode->i_nlinks--;
		iput(inode);
		return -ERROR;
	}
	i = 0;
	while (i < 1023 && (c=get_fs_byte(oldname++)))
		name_block->b_data[i++] = c;
	name_block->b_data[i] = 0;
	name_block->b_dirt = 1;
	brelse(name_block);
	inode->i_size = i;
	inode->i_dirt = 1;
	bh = find_entry(&dir,basename,namelen,&de);
	if (bh) {
		inode->i_nlinks--;
		iput(inode);
		brelse(bh);
		iput(dir);
		return -EEXIST;
	}
	bh = add_entry(dir,basename,namelen,&de);
	if (!bh) {
		inode->i_nlinks--;
		iput(inode);
		iput(dir);
		return -ENOSPC;
	}
	de->inode = inode->i_num;
	bh->b_dirt = 1;
	brelse(bh);
	iput(dir);
	iput(inode);
	return 0;
}

//创建硬链接
int sys_link(const char * oldname, const char * newname)
{
	struct dir_entry * de;
	struct m_inode * oldinode, * dir;
	struct buffer_head * bh;
	const char * basename;
	int namelen;

	oldinode=namei(oldname);
	if (!oldinode)
		return -ENOENT;
	if (S_ISDIR(oldinode->i_mode)) {
		iput(oldinode);
		return -EPERM;
	}
	
	dir = dir_namei(newname,&namelen,&basename, NULL);
	//父目录不存在
	if (!dir) {
		iput(oldinode);
		return -EACCES;
	}
	if (!namelen) {
		iput(oldinode);
		iput(dir);
		return -EPERM;
	}
	//不能跨设备建立硬链接
	if (dir->i_dev != oldinode->i_dev) {
		iput(dir);
		iput(oldinode);
		return -EXDEV;
	}
	if (!permission(dir,MAY_WRITE)) {
		iput(dir);
		iput(oldinode);
		return -EACCES;
	}
	//新路径已经存在文件
	bh = find_entry(&dir,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir);
		iput(oldinode);
		return -EEXIST;
	}
	//可以正式创建一个dentry了
	bh = add_entry(dir,basename,namelen,&de);
	if (!bh) {
		iput(dir);
		iput(oldinode);
		return -ENOSPC;
	}
	//指向同一个inode
	de->inode = oldinode->i_num;
	bh->b_dirt = 1;
	brelse(bh);
	iput(dir);
	//增加硬链接数
	oldinode->i_nlinks++;
	oldinode->i_ctime = CURRENT_TIME;
	oldinode->i_dirt = 1;
	iput(oldinode);
	return 0;
}
