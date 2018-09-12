/*
 *  linux/fs/super.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * super.c contains code to handle the super-block tables.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include <errno.h>
#include <sys/stat.h>

int sync_dev(int dev);
void wait_for_keypress(void);

/* set_bit uses setb, as gas doesn't recognize setc */
//测试指定bit位的值 并返回原来的值
#define set_bit(bitnr,addr) ({ \
register int __res __asm__("ax"); \
__asm__("bt %2,%3;setb %%al":"=a" (__res):"a" (0),"r" (bitnr),"m" (*(addr))); \
__res; })

struct super_block super_block[NR_SUPER];
/* this is initialized in init/main.c */
int ROOT_DEV = 0;

static void lock_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sb->s_lock = 1;
	sti();
}

static void free_super(struct super_block * sb)
{
	cli();
	sb->s_lock = 0;
	wake_up(&(sb->s_wait));
	sti();
}

static void wait_on_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sti();
}

//在数组中找到对应设备号的超级块
struct super_block * get_super(int dev)
{
	struct super_block * s;

	if (!dev)
		return NULL;
	s = 0+super_block;
	while (s < NR_SUPER+super_block)
		//数组中找到
		if (s->s_dev == dev) {
			wait_on_super(s);
			if (s->s_dev == dev)
				return s;
			s = 0+super_block;
		} else
			s++;
	return NULL;
}

void put_super(int dev)
{
	struct super_block * sb;
	int i;

	if (dev == ROOT_DEV) {
		printk("root diskette changed: prepare for armageddon\n\r");
		return;
	}
	//从数组中中对应的超级块
	if (!(sb = get_super(dev)))
		return;
	// 如果该文件系统所挂载的点 还没处理过 则返回
	if (sb->s_imount) {
		printk("Mounted disk changed - tssk, tssk\n\r");
		return;
	}
	lock_super(sb);
	//设置sb的设备号字段=0
	sb->s_dev = 0;
	//写回超级快的inode map和数据块map所占用的缓冲块
	for(i=0;i<I_MAP_SLOTS;i++)
		brelse(sb->s_imap[i]);
	for(i=0;i<Z_MAP_SLOTS;i++)
		brelse(sb->s_zmap[i]);
	free_super(sb);
	return;
}

//读取指定设备的超级块  先从数组中找
static struct super_block * read_super(int dev)
{
	struct super_block * s;
	struct buffer_head * bh;
	int i,block;

	if (!dev)
		return NULL;
	check_disk_change(dev);
	//数组中找到 直接返回
	if (s = get_super(dev))
		return s;
	//找到一个空闲的项
	for (s = 0+super_block ;; s++) {
		if (s >= NR_SUPER+super_block)
			return NULL;
		if (!s->s_dev)
			break;
	}
	//设置字段 
	s->s_dev = dev;
	s->s_isup = NULL;
	s->s_imount = NULL;
	s->s_time = 0;
	s->s_rd_only = 0;
	s->s_dirt = 0;
	lock_super(s);
	//读取inode=1
	if (!(bh = bread(dev,1))) {
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	*((struct d_super_block *) s) =
		*((struct d_super_block *) bh->b_data);
	brelse(bh);
	//检查magic
	if (s->s_magic != SUPER_MAGIC) {
		s->s_dev = 0;
		free_super(s);
		return NULL;
	}
	//初始化inode map指针数组
	for (i=0;i<I_MAP_SLOTS;i++)
		s->s_imap[i] = NULL;
	//初始化磁盘块map指针数组
	for (i=0;i<Z_MAP_SLOTS;i++)
		s->s_zmap[i] = NULL;
	block=2;
	//将位图读进内存
	for (i=0 ; i < s->s_imap_blocks ; i++)
		if (s->s_imap[i]=bread(dev,block))
			block++;
		else
			break;
	for (i=0 ; i < s->s_zmap_blocks ; i++)
		if (s->s_zmap[i]=bread(dev,block))
			block++;
		else
			break;
		//检查错误
	if (block != 2+s->s_imap_blocks+s->s_zmap_blocks) {
		for(i=0;i<I_MAP_SLOTS;i++)
			brelse(s->s_imap[i]);
		for(i=0;i<Z_MAP_SLOTS;i++)
			brelse(s->s_zmap[i]);
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	//0号inode不可用
	s->s_imap[0]->b_data[0] |= 1;
	s->s_zmap[0]->b_data[0] |= 1;
	free_super(s);
	return s;
}

//卸载设备
int sys_umount(char * dev_name)
{
	struct m_inode * inode;
	struct super_block * sb;
	int dev;
	//根据名字找到inode
	if (!(inode=namei(dev_name)))
		return -ENOENT;
	//取出设备号
	dev = inode->i_zone[0];
	//如果不是代表块设备 则返回
	if (!S_ISBLK(inode->i_mode)) {
		iput(inode);
		return -ENOTBLK;
	}
	//已经不需要inode了 释放之
	iput(inode);
	//不能卸载根设备
	if (dev==ROOT_DEV)
		return -EBUSY;
	//找不到对应的超级快 或者 没有挂载过 返回
	if (!(sb=get_super(dev)) || !(sb->s_imount))
		return -ENOENT;
	//挂载点 不是一个挂载点
	if (!sb->s_imount->i_mount)
		printk("Mounted inode has i_mount=0\n");
	//是否有进程引用此设备上的文件
	for (inode=inode_table+0 ; inode<inode_table+NR_INODE ; inode++)
		if (inode->i_dev==dev && inode->i_count)
				return -EBUSY;
	//开始真正的卸载
	//挂载点inode的mount标志复位
	sb->s_imount->i_mount=0;
	//释放挂载点inode
	iput(sb->s_imount);
	//超级块的挂载点置为null
	sb->s_imount = NULL;
	//释放设备的根inode
	iput(sb->s_isup);
	sb->s_isup = NULL;
	//释放超级块
	put_super(dev);
	//会同步buffer到磁盘
	sync_dev(dev);
	return 0;
}

//挂载文件系统
int sys_mount(char * dev_name, char * dir_name, int rw_flag)
{
	struct m_inode * dev_i, * dir_i;
	struct super_block * sb;
	int dev;
	//根据设备文件名 找到inode
	if (!(dev_i=namei(dev_name)))
		return -ENOENT;
	//取出设备号
	dev = dev_i->i_zone[0];
	//如果不是块设备文件 则返回
	if (!S_ISBLK(dev_i->i_mode)) {
		iput(dev_i);
		return -EPERM;
	}
	//现在不需要inode了
	iput(dev_i);
	//得到挂载点的inode
	if (!(dir_i=namei(dir_name)))
		return -ENOENT;
	//挂载点的引用计数不是1 或者 该挂载点的inode号是根inode号 则返回
	if (dir_i->i_count != 1 || dir_i->i_num == ROOT_INO) {
		iput(dir_i);
		return -EBUSY;
	}
	//不是目录 返回  挂载点只能是目录
	if (!S_ISDIR(dir_i->i_mode)) {
		iput(dir_i);
		return -EPERM;
	}
	//读进超级块
	if (!(sb=read_super(dev))) {
		iput(dir_i);
		return -EBUSY;
	}
	//已经挂载了 返回
	if (sb->s_imount) {
		iput(dir_i);
		return -EBUSY;
	}
	//挂载点已经挂载了其他文件系统 返回
	if (dir_i->i_mount) {
		iput(dir_i);
		return -EPERM;
	}
	//设置文件系统的挂载点
	sb->s_imount=dir_i;
	//设置挂载点inode的相关字段
	dir_i->i_mount=1;
	dir_i->i_dirt=1;		/* NOTE! we don't iput(dir_i) */
	return 0;			/* we do that in umount */
}

void mount_root(void)
{
	int i,free;
	struct super_block * p;
	struct m_inode * mi;

	if (32 != sizeof (struct d_inode))
		panic("bad i-node size");
	//file table 清0  整个系统的file table
	for(i=0;i<NR_FILE;i++)
		file_table[i].f_count=0;
	//跟设备是软驱时 要求插入软盘
	if (MAJOR(ROOT_DEV) == 2) {
		printk("Insert root floppy and press ENTER");
		wait_for_keypress();
	}
	//清0 所有super block数组项
	for(p = &super_block[0] ; p < &super_block[NR_SUPER] ; p++) {
		p->s_dev = 0;
		p->s_lock = 0;
		p->s_wait = NULL;
	}
	//读取根设备的super block到超级快数组
	if (!(p=read_super(ROOT_DEV)))
		panic("Unable to mount root");
	// 获取root inode
	if (!(mi=iget(ROOT_DEV,ROOT_INO)))
		panic("Unable to read root i-node");
	mi->i_count += 3 ;	/* NOTE! it is logically used 4 times, not 1 */
	//设置当前文件系统的挂载点和根节点
	p->s_isup = p->s_imount = mi;
	//设置0号进程的pwd和root的inode
	current->pwd = mi;
	current->root = mi;
	//下面就是统计下当前文件系统的free block数量
	free=0;
	i=p->s_nzones;
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_zmap[i>>13]->b_data))
			free++;
	printk("%d/%d free blocks\n\r",free,p->s_nzones);
	free=0;
	i=p->s_ninodes+1;
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_imap[i>>13]->b_data))
			free++;
	printk("%d/%d free inodes\n\r",free,p->s_ninodes);
}
