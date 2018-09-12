/*
 *  linux/fs/bitmap.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* bitmap.c contains the code that handles the inode and block bitmaps */
#include <string.h>

#include <linux/sched.h>
#include <linux/kernel.h>

#define clear_block(addr) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"stosl" \
	::"a" (0),"c" (BLOCK_SIZE/4),"D" ((long) (addr)):"cx","di")

#define set_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btsl %2,%3\n\tsetb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

#define clear_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btrl %2,%3\n\tsetnb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

#define find_first_zero(addr) ({ \
int __res; \
__asm__("cld\n" \
	"1:\tlodsl\n\t" \
	"notl %%eax\n\t" \
	"bsfl %%eax,%%edx\n\t" \
	"je 2f\n\t" \
	"addl %%edx,%%ecx\n\t" \
	"jmp 3f\n" \
	"2:\taddl $32,%%ecx\n\t" \
	"cmpl $8192,%%ecx\n\t" \
	"jl 1b\n" \
	"3:" \
	:"=c" (__res):"c" (0),"S" (addr):"ax","dx","si"); \
__res;})
  
//释放设备上的数据块block    block是在磁盘中的号 不是文件中的块号
//会清空磁盘上的bitmap
//成功返回1
int free_block(int dev, int block)
{
	struct super_block * sb;
	struct buffer_head * bh;
	//读取超级块
	if (!(sb = get_super(dev)))
		panic("trying to free block on nonexistent device");
	if (block < sb->s_firstdatazone || block >= sb->s_nzones)
		panic("trying to free block not in datazone");
	//先从缓冲区中找
	bh = get_hash_table(dev,block);
	//找到
	if (bh) {
		if (bh->b_count > 1) {
			//块还有人用
			brelse(bh);
			//失败 返回0 
			return 0;
		}
		//否则设置b_dirt=0
		bh->b_dirt=0;
		bh->b_uptodate=0;
		//然后释放块 
		if (bh->b_count)
			brelse(bh);
	}
	//修改位图
	block -= sb->s_firstdatazone - 1 ;
	if (clear_bit(block&8191,sb->s_zmap[block/8192]->b_data)) {
		printk("block (%04x:%d) ",dev,block+sb->s_firstdatazone-1);
		printk("free_block: bit already cleared\n");
	}
	//位图所在块 变成脏的
	sb->s_zmap[block/8192]->b_dirt = 1;
	return 1;
}

//向设备申请一个块
int new_block(int dev)
{
	struct buffer_head * bh;
	struct super_block * sb;
	int i,j;

	if (!(sb = get_super(dev)))
		panic("trying to get new block from nonexistant device");
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_zmap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (i>=8 || !bh || j>=8192)
		return 0;
	if (set_bit(j,bh->b_data))
		panic("new_block: bit already set");
	bh->b_dirt = 1;
	//找一个磁盘上的逻辑块号  通过bitmap
	j += i*8192 + sb->s_firstdatazone-1;
	//大于总逻辑块 返回
	if (j >= sb->s_nzones)
		return 0;
	//从buffer中取得一个缓冲块， 关联到磁盘上j号 磁盘块
	if (!(bh=getblk(dev,j)))
		panic("new_block: cannot get block");
	if (bh->b_count != 1)
		panic("new block: count is != 1");
	//清空缓冲块
	clear_block(bh->b_data);
	bh->b_uptodate = 1;
	bh->b_dirt = 1;
	//释放缓冲块
	brelse(bh);
	return j;
}

//如果可释放 则清空对应位图的bit 然后清空inode结构
void free_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;

	if (!inode)
		return;
	//
	if (!inode->i_dev) {
		memset(inode,0,sizeof(*inode));
		return;
	}
	//还有引用 则不能释放 panic
	if (inode->i_count>1) {
		printk("trying to free inode with count=%d\n",inode->i_count);
		panic("free_inode");
	}
	if (inode->i_nlinks)
		panic("trying to free inode with links");
	//获取inode所在设备的超级块
	if (!(sb = get_super(inode->i_dev)))
		panic("trying to free inode on nonexistent device");
	//无效inode号
	if (inode->i_num < 1 || inode->i_num > sb->s_ninodes)
		panic("trying to free inode 0 or nonexistant inode");
	//位图所在块的buffer不存在 返回
	if (!(bh=sb->s_imap[inode->i_num>>13]))
		panic("nonexistent imap in superblock");
	//清空inode在位图中的bit
	if (clear_bit(inode->i_num&8191,bh->b_data))
		printk("free_inode: bit already cleared.\n\r");
	//设置inode所在位图的buffer的dirty=1
	bh->b_dirt = 1;
	//清空inode结构
	memset(inode,0,sizeof(*inode));
}

struct m_inode * new_inode(int dev)
{
	struct m_inode * inode;
	struct super_block * sb;
	struct buffer_head * bh;
	int i,j;
	//数组中找不到空闲inode 返回
	if (!(inode=get_empty_inode()))
		return NULL;
	//获取设备的超级快
	if (!(sb = get_super(dev)))
		panic("new_inode with unknown device");
	j = 8192;
	//找inode map中的空闲bit
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_imap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (!bh || j >= 8192 || j+i*8192 > sb->s_ninodes) {
		iput(inode);
		return NULL;
	}
	///找到后 置位
	if (set_bit(j,bh->b_data))
		panic("new_inode: bit already set");
	bh->b_dirt = 1;
	//填充inode
	inode->i_count=1;
	inode->i_nlinks=1;
	inode->i_dev=dev;
	inode->i_uid=current->euid;
	inode->i_gid=current->egid;
	inode->i_dirt=1;
	//inode号
	inode->i_num = j + i*8192;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	return inode;
}
