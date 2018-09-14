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

//û��slab ��̬�����inode����
struct m_inode inode_table[NR_INODE]={{0,},};

static void read_inode(struct m_inode * inode);
static void write_inode(struct m_inode * inode);

static inline void wait_on_inode(struct m_inode * inode)
{
	cli();
	//������  ���ǵȴ�����
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

//�ͷ�ָ��dev�ϵ�inode
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
			inode->i_dev = inode->i_dirt = 0; //inode���豸������Ϊ0
		}
	}
}

//ͬ���ڴ��е�inode��������
void sync_inodes(void)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		if (inode->i_dirt && !inode->i_pipe)
			//���ǹܵ��ڵ� ����dirty�� д�뻺����  buffer.c��д�ش���
			write_inode(inode);
	}
}

//�ļ����ݿ� ӳ�䵽���̿�  create���������Ӧ�����ϲ����ڿ� �򴴽�һ��
//�ļ��鰴���ļ�����˳�򣬵����ڴ����п����Ƿ�ɢ�˵ģ�����ļ��е��߼���ʹ����еĿ�� ��Ҫһ��������  ͨ��inode�е��������
static int _bmap(struct m_inode * inode,int block,int create)
{
	struct buffer_head * bh;
	int i;

	if (block<0)
		panic("_bmap: block<0");
	if (block >= 7+512+512*512)
		//�������Ŀ��� panic
		panic("_bmap: block>big");
	if (block<7) { //ֱ�ӿ�
		if (create && !inode->i_zone[block])
			//��Ӧ���̿�Ϊ0 ��create��λ �򴴽�һ��block�� �������̿����õ��߼����ֶ���
			if (inode->i_zone[block]=new_block(inode->i_dev)) {
				inode->i_ctime=CURRENT_TIME;
				inode->i_dirt=1;
			}
		return inode->i_zone[block];
	}
	block -= 7;
	if (block<512) { 
		//һ�μ�ӿ�
		if (create && !inode->i_zone[7])
			if (inode->i_zone[7]=new_block(inode->i_dev)) {
				inode->i_dirt=1;
				inode->i_ctime=CURRENT_TIME;
			}
		if (!inode->i_zone[7])
			return 0;
		//����1����
		if (!(bh = bread(inode->i_dev,inode->i_zone[7])))
			return 0;
		i = ((unsigned short *) (bh->b_data))[block];
		// 1����ָ��Ĵ��̿��Ϊ0 �򴴽�һ�����̿� �������̿�����õ�1��������
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

//ȡ���ļ��߼����Ӧ�Ĵ����ϵĿ��
int bmap(struct m_inode * inode,int block)
{
	return _bmap(inode,block,0);
}

//������һ�� ����create=1 
int create_block(struct m_inode * inode, int block)
{
	return _bmap(inode,block,1);
}

//inode������1 �����pipe��inode ���ѵȴ�����  
// ����ǿ��豸inode��ˢ���豸  ���inode����Ϊ0 ���ͷ�inode��ռ�õĴ����߼��� ���ͷ�inode
void iput(struct m_inode * inode)
{
	if (!inode)
		return;
	wait_on_inode(inode);
	if (!inode->i_count)
		panic("iput: trying to free free inode");
	if (inode->i_pipe) {
		//�����pipe inode ��ỽ�ѵȴ�����
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
	//����Ǵ����豸��inode ��sync�豸(/dev/hdxx)
	if (S_ISBLK(inode->i_mode)) {
		sync_dev(inode->i_zone[0]);
		wait_on_inode(inode);
	}
repeat:
	//������Ϊ0 ����
	if (inode->i_count>1) { 
		inode->i_count--;
		return;
	}
	// inode������Ϊ0 ���ͷ�inode����ռ���̿�
	if (!inode->i_nlinks) {
		//�ͷ�inodeռ�õĴ��̿�
		truncate(inode);
		free_inode(inode);
		return;
	}
	//д��inode
	if (inode->i_dirt) {
		write_inode(inode);	/* we can sleep - so do again */
		wait_on_inode(inode);
		goto repeat;
	}
	//�ߵ��� ����count=1 ��--�����Ѿ��ͷ� 
	inode->i_count--;
	return;
}

//���������ҵ�һ��inode(count=0)
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
				//����=0 ���һ���ж��Ƿ������
				inode = last_inode;
				if (!inode->i_dirt && !inode->i_lock)
					break;
			}
		}
		//û�ҵ�inode
		if (!inode) {
			for (i=0 ; i<NR_INODE ; i++)
				printk("%04x: %6d\t",inode_table[i].i_dev,
					inode_table[i].i_num);
			panic("No free inodes in mem");
		}
		wait_on_inode(inode);
		//����dirty��inode Ҫд��
		while (inode->i_dirt) {
			write_inode(inode);
			wait_on_inode(inode);
		}
	} while (inode->i_count);
	memset(inode,0,sizeof(*inode));
	//���������count=1
	inode->i_count = 1;
	return inode;
}

struct m_inode * get_pipe_inode(void)
{
	struct m_inode * inode;

	if (!(inode = get_empty_inode()))
		return NULL;
	//i_sizeָ��������ڴ�
	if (!(inode->i_size=get_free_page())) {
		inode->i_count = 0;
		return NULL;
	}
	inode->i_count = 2;	/* sum of readers/writers */
	PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
	//����pipe��inode
	inode->i_pipe = 1;
	return inode;
}

//���豸�ж�ȡָ��inode�ŵ�inode��������  ������ָ��
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
		//���������ҵ��˶�Ӧ��inode ���������ü���
		inode->i_count++;
		if (inode->i_mount) {
			int i;
			//����ǹ��ص� ���ҵ������ص��ļ�ϵͳ�ĳ�����
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
		//���������ҵ�inode ��put��ʱ�����inode Ȼ���ҵ���inode����
		if (empty)
			iput(empty);
		return inode;
	}
	//�ߵ�������� û�����������ҵ�inode
	if (!empty)
		return (NULL);
	inode=empty;
	inode->i_dev = dev;
	inode->i_num = nr;
	//����inode
	read_inode(inode);
	return inode;
}

static void read_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	//�ȶ�������
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to read inode without dev");
	//inode���ڴ��̿��=������+������+inodemap ����+���̿�map����+(inode��-1)/û���̿麬�е�inode��
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	//����inode���ڴ��̿�
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	//��λinode
	*(struct d_inode *)inode =
		((struct d_inode *)bh->b_data)
			[(inode->i_num-1)%INODES_PER_BLOCK];
	brelse(bh);
	//����ǿ��豸�ڵ�  ��Ҫ����inode���ļ�����󳤶�ֵ  
	if (S_ISBLK(inode->i_mode)) {
		int i = inode->i_zone[0];
		if (blk_size[MAJOR(i)])
			//����Ŀ��豸�Ŀռ��Ƕ���
			inode->i_size = 1024*blk_size[MAJOR(i)][MINOR(i)];
		else
			inode->i_size = 0x7fffffff;
	}
	unlock_inode(inode);
}

//inodeд��
static void write_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	//����д��
	if (!inode->i_dirt || !inode->i_dev) {
		unlock_inode(inode);
		return;
	}
	//��write inodeһ�� �ҵ�inode����buffer��λ��
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to write inode without device");
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	//inode���Ƶ�buffer
	((struct d_inode *)bh->b_data)
		[(inode->i_num-1)%INODES_PER_BLOCK] =
			*(struct d_inode *)inode;
	//����buffer��dirty=1
	bh->b_dirt=1;
	//inode��dirty=0
	inode->i_dirt=0;
	brelse(bh);
	unlock_inode(inode);
}
