/*
 *  linux/fs/inode.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/system.h>

extern int *blk_size[];

//没有slab 静态定义的inode数组
struct m_inode inode_table[NR_INODE]={{0,},};

static void read_inode(struct m_inode * inode);
static void write_inode(struct m_inode * inode);

static inline void wait_on_inode(struct m_inode * inode)
{
	cli();
	//上锁了  我们等待解锁
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	sti();
}

static inline void lock_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	inode->i_lock=1;
	sti();
}

static inline void unlock_inode(struct m_inode * inode)
{
	inode->i_lock=0;
	wake_up(&inode->i_wait);
}

//释放指定dev上的inode
void invalidate_inodes(int dev)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		if (inode->i_dev == dev) {
			if (inode->i_count)
				printk("inode in use on removed disk\n\r");
			inode->i_dev = inode->i_dirt = 0; //inode的设备号设置为0
		}
	}
}

//同步内存中的inode到磁盘上
void sync_inodes(void)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		if (inode->i_dirt && !inode->i_pipe)
			//不是管道节点 且是dirty的 写入缓冲区  buffer.c会写回磁盘
			write_inode(inode);
	}
}

//文件数据块 映射到磁盘块  create代表如果对应磁盘上不存在块 则创建一个
//文件块按照文件内容顺序，但是在磁盘中可能是分散了的，因此文件中的逻辑快和磁盘中的块号 需要一个查表过程  通过inode中的数组查找
static int _bmap(struct m_inode * inode,int block,int create)
{
	struct buffer_head * bh;
	int i;

	if (block<0)
		panic("_bmap: block<0");
	if (block >= 7+512+512*512)
		//超出最多的块数 panic
		panic("_bmap: block>big");
	if (block<7) { //直接块
		if (create && !inode->i_zone[block])
			//对应磁盘块为0 且create置位 则创建一个block， 并将磁盘块设置到逻辑块字段中
			if (inode->i_zone[block]=new_block(inode->i_dev)) {
				inode->i_ctime=CURRENT_TIME;
				inode->i_dirt=1;
			}
		return inode->i_zone[block];
	}
	block -= 7;
	if (block<512) { 
		//一次间接块
		if (create && !inode->i_zone[7])
			if (inode->i_zone[7]=new_block(inode->i_dev)) {
				inode->i_dirt=1;
				inode->i_ctime=CURRENT_TIME;
			}
		if (!inode->i_zone[7])
			return 0;
		//读进1级块
		if (!(bh = bread(inode->i_dev,inode->i_zone[7])))
			return 0;
		i = ((unsigned short *) (bh->b_data))[block];
		// 1级块指向的磁盘块号为0 则创建一个磁盘块 并将磁盘块号设置到1级块项中
		if (create && !i)
			if (i=new_block(inode->i_dev)) {
				((unsigned short *) (bh->b_data))[block]=i;
				bh->b_dirt=1;
			}
		brelse(bh);
		return i;
	}
	block -= 512;
	if (create && !inode->i_zone[8])
		if (inode->i_zone[8]=new_block(inode->i_dev)) {
			inode->i_dirt=1;
			inode->i_ctime=CURRENT_TIME;
		}
	if (!inode->i_zone[8])
		return 0;
	if (!(bh=bread(inode->i_dev,inode->i_zone[8])))
		return 0;
	i = ((unsigned short *)bh->b_data)[block>>9];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block>>9]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	if (!i)
		return 0;
	if (!(bh=bread(inode->i_dev,i)))
		return 0;
	i = ((unsigned short *)bh->b_data)[block&511];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block&511]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	return i;
}

//取出文件逻辑块对应的磁盘上的块号
int bmap(struct m_inode * inode,int block)
{
	return _bmap(inode,block,0);
}

//和上面一样 但是create=1 
int create_block(struct m_inode * inode, int block)
{
	return _bmap(inode,block,1);
}

//inode计数减1 如果是pipe的inode 则唤醒等待进程  
// 如果是块设备inode则刷新设备  入股inode计数为0 则释放inode所占用的磁盘逻辑快 并释放inode
void iput(struct m_inode * inode)
{
	if (!inode)
		return;
	wait_on_inode(inode);
	if (!inode->i_count)
		panic("iput: trying to free free inode");
	if (inode->i_pipe) {
		//如果是pipe inode 则会唤醒等待进程
		wake_up(&inode->i_wait);
		wake_up(&inode->i_wait2);
		if (--inode->i_count)
			return;
		free_page(inode->i_size);
		inode->i_count=0;
		inode->i_dirt=0;
		inode->i_pipe=0;
		return;
	}
	if (!inode->i_dev) {
		inode->i_count--;
		return;
	}
	//如果是代表设备的inode 则sync设备(/dev/hdxx)
	if (S_ISBLK(inode->i_mode)) {
		sync_dev(inode->i_zone[0]);
		wait_on_inode(inode);
	}
repeat:
	//计数不为0 返回
	if (inode->i_count>1) { 
		inode->i_count--;
		return;
	}
	// inode连接数为0 则释放inode和所占磁盘块
	if (!inode->i_nlinks) {
		//释放inode占用的磁盘块
		truncate(inode);
		free_inode(inode);
		return;
	}
	//写回inode
	if (inode->i_dirt) {
		write_inode(inode);	/* we can sleep - so do again */
		wait_on_inode(inode);
		goto repeat;
	}
	//走到这 代表count=1 则--代表已经释放 
	inode->i_count--;
	return;
}

//从数组中找到一个inode(count=0)
struct m_inode * get_empty_inode(void)
{
	struct m_inode * inode;
	static struct m_inode * last_inode = inode_table;
	int i;

	do {
		inode = NULL;
		for (i = NR_INODE; i ; i--) {
			if (++last_inode >= inode_table + NR_INODE)
				last_inode = inode_table;
			if (!last_inode->i_count) {
				//计数=0 则进一步判断是否可以用
				inode = last_inode;
				if (!inode->i_dirt && !inode->i_lock)
					break;
			}
		}
		//没找到inode
		if (!inode) {
			for (i=0 ; i<NR_INODE ; i++)
				printk("%04x: %6d\t",inode_table[i].i_dev,
					inode_table[i].i_num);
			panic("No free inodes in mem");
		}
		wait_on_inode(inode);
		//对于dirty的inode 要写回
		while (inode->i_dirt) {
			write_inode(inode);
			wait_on_inode(inode);
		}
	} while (inode->i_count);
	memset(inode,0,sizeof(*inode));
	//这里会设置count=1
	inode->i_count = 1;
	return inode;
}

struct m_inode * get_pipe_inode(void)
{
	struct m_inode * inode;

	if (!(inode = get_empty_inode()))
		return NULL;
	//i_size指向申请的内存
	if (!(inode->i_size=get_free_page())) {
		inode->i_count = 0;
		return NULL;
	}
	inode->i_count = 2;	/* sum of readers/writers */
	PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
	//代表pipe的inode
	inode->i_pipe = 1;
	return inode;
}

//从设备中读取指定inode号的inode到数组中  返回其指针
struct m_inode * iget(int dev,int nr)
{
	struct m_inode * inode, * empty;

	if (!dev)
		panic("iget with dev==0");
	empty = get_empty_inode();
	inode = inode_table;
	while (inode < NR_INODE+inode_table) {
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode++;
			continue;
		}
		wait_on_inode(inode);
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode = inode_table;
			continue;
		}
		//在数组中找到了对应的inode 则增加引用计数
		inode->i_count++;
		if (inode->i_mount) {
			int i;
			//如果是挂载点 则找到所挂载的文件系统的超级快
			for (i = 0 ; i<NR_SUPER ; i++)
				if (super_block[i].s_imount==inode)
					break;
			if (i >= NR_SUPER) {
				printk("Mounted inode hasn't got sb\n");
				if (empty)
					iput(empty);
				return inode;
			}
			iput(inode);
			dev = super_block[i].s_dev;
			nr = ROOT_INO;
			inode = inode_table;
			continue;
		}
		//在数组中找到inode 先put临时申请的inode 然后将找到的inode返回
		if (empty)
			iput(empty);
		return inode;
	}
	//走到这里代表 没有在数组中找到inode
	if (!empty)
		return (NULL);
	inode=empty;
	inode->i_dev = dev;
	inode->i_num = nr;
	//读进inode
	read_inode(inode);
	return inode;
}

static void read_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	//先读超级块
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to read inode without dev");
	//inode所在磁盘块号=启动快+超级快+inodemap 块数+磁盘块map块数+(inode号-1)/没磁盘块含有的inode数
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	//读进inode所在磁盘块
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	//定位inode
	*(struct d_inode *)inode =
		((struct d_inode *)bh->b_data)
			[(inode->i_num-1)%INODES_PER_BLOCK];
	brelse(bh);
	//如果是块设备节点  则还要设置inode的文件的最大长度值  
	if (S_ISBLK(inode->i_mode)) {
		int i = inode->i_zone[0];
		if (blk_size[MAJOR(i)])
			//代表的快设备的空间是多少
			inode->i_size = 1024*blk_size[MAJOR(i)][MINOR(i)];
		else
			inode->i_size = 0x7fffffff;
	}
	unlock_inode(inode);
}

//inode写回
static void write_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	//无需写回
	if (!inode->i_dirt || !inode->i_dev) {
		unlock_inode(inode);
		return;
	}
	//和write inode一致 找到inode所在buffer的位置
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to write inode without device");
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	//inode复制到buffer
	((struct d_inode *)bh->b_data)
		[(inode->i_num-1)%INODES_PER_BLOCK] =
			*(struct d_inode *)inode;
	//设置buffer的dirty=1
	bh->b_dirt=1;
	//inode的dirty=0
	inode->i_dirt=0;
	brelse(bh);
	unlock_inode(inode);
}
