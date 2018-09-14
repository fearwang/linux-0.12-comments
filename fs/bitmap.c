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
  
//�ͷ��豸�ϵ����ݿ�block    block���ڴ����еĺ� �����ļ��еĿ��
//����մ����ϵ�bitmap
//�ɹ�����1
int free_block(int dev, int block)
{
	struct super_block * sb;
	struct buffer_head * bh;
	//��ȡ������
	if (!(sb = get_super(dev)))
		panic("trying to free block on nonexistent device");
	if (block < sb->s_firstdatazone || block >= sb->s_nzones)
		panic("trying to free block not in datazone");
	//�ȴӻ���������
	bh = get_hash_table(dev,block);
	//�ҵ�
	if (bh) {
		if (bh->b_count > 1) {
			//�黹������
			brelse(bh);
			//ʧ�� ����0 
			return 0;
		}
		//��������b_dirt=0
		bh->b_dirt=0;
		bh->b_uptodate=0;
		//Ȼ���ͷſ� 
		if (bh->b_count)
			brelse(bh);
	}
	//�޸�λͼ
	block -= sb->s_firstdatazone - 1 ;
	if (clear_bit(block&8191,sb->s_zmap[block/8192]->b_data)) {
		printk("block (%04x:%d) ",dev,block+sb->s_firstdatazone-1);
		printk("free_block: bit already cleared\n");
	}
	//λͼ���ڿ� ������
	sb->s_zmap[block/8192]->b_dirt = 1;
	return 1;
}

//���豸����һ����
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
	//��һ�������ϵ��߼����  ͨ��bitmap
	j += i*8192 + sb->s_firstdatazone-1;
	//�������߼��� ����
	if (j >= sb->s_nzones)
		return 0;
	//��buffer��ȡ��һ������飬 ������������j�� ���̿�
	if (!(bh=getblk(dev,j)))
		panic("new_block: cannot get block");
	if (bh->b_count != 1)
		panic("new block: count is != 1");
	//��ջ����
	clear_block(bh->b_data);
	bh->b_uptodate = 1;
	bh->b_dirt = 1;
	//�ͷŻ����
	brelse(bh);
	return j;
}

//������ͷ� ����ն�Ӧλͼ��bit Ȼ�����inode�ṹ
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
	//�������� �����ͷ� panic
	if (inode->i_count>1) {
		printk("trying to free inode with count=%d\n",inode->i_count);
		panic("free_inode");
	}
	if (inode->i_nlinks)
		panic("trying to free inode with links");
	//��ȡinode�����豸�ĳ�����
	if (!(sb = get_super(inode->i_dev)))
		panic("trying to free inode on nonexistent device");
	//��Чinode��
	if (inode->i_num < 1 || inode->i_num > sb->s_ninodes)
		panic("trying to free inode 0 or nonexistant inode");
	//λͼ���ڿ��buffer������ ����
	if (!(bh=sb->s_imap[inode->i_num>>13]))
		panic("nonexistent imap in superblock");
	//���inode��λͼ�е�bit
	if (clear_bit(inode->i_num&8191,bh->b_data))
		printk("free_inode: bit already cleared.\n\r");
	//����inode����λͼ��buffer��dirty=1
	bh->b_dirt = 1;
	//���inode�ṹ
	memset(inode,0,sizeof(*inode));
}

struct m_inode * new_inode(int dev)
{
	struct m_inode * inode;
	struct super_block * sb;
	struct buffer_head * bh;
	int i,j;
	//�������Ҳ�������inode ����
	if (!(inode=get_empty_inode()))
		return NULL;
	//��ȡ�豸�ĳ�����
	if (!(sb = get_super(dev)))
		panic("new_inode with unknown device");
	j = 8192;
	//��inode map�еĿ���bit
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_imap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	if (!bh || j >= 8192 || j+i*8192 > sb->s_ninodes) {
		iput(inode);
		return NULL;
	}
	///�ҵ��� ��λ
	if (set_bit(j,bh->b_data))
		panic("new_inode: bit already set");
	bh->b_dirt = 1;
	//���inode
	inode->i_count=1;
	inode->i_nlinks=1;
	inode->i_dev=dev;
	inode->i_uid=current->euid;
	inode->i_gid=current->egid;
	inode->i_dirt=1;
	//inode��
	inode->i_num = j + i*8192;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	return inode;
}
