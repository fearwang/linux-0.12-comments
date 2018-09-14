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
//����ָ��bitλ��ֵ ������ԭ����ֵ
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

//���������ҵ���Ӧ�豸�ŵĳ�����
struct super_block * get_super(int dev)
{
	struct super_block * s;

	if (!dev)
		return NULL;
	s = 0+super_block;
	while (s < NR_SUPER+super_block)
		//�������ҵ�
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
	//���������ж�Ӧ�ĳ�����
	if (!(sb = get_super(dev)))
		return;
	// ������ļ�ϵͳ�����صĵ� ��û����� �򷵻�
	if (sb->s_imount) {
		printk("Mounted disk changed - tssk, tssk\n\r");
		return;
	}
	lock_super(sb);
	//����sb���豸���ֶ�=0
	sb->s_dev = 0;
	//д�س������inode map�����ݿ�map��ռ�õĻ����
	for(i=0;i<I_MAP_SLOTS;i++)
		brelse(sb->s_imap[i]);
	for(i=0;i<Z_MAP_SLOTS;i++)
		brelse(sb->s_zmap[i]);
	free_super(sb);
	return;
}

//��ȡָ���豸�ĳ�����  �ȴ���������
static struct super_block * read_super(int dev)
{
	struct super_block * s;
	struct buffer_head * bh;
	int i,block;

	if (!dev)
		return NULL;
	check_disk_change(dev);
	//�������ҵ� ֱ�ӷ���
	if (s = get_super(dev))
		return s;
	//�ҵ�һ�����е���
	for (s = 0+super_block ;; s++) {
		if (s >= NR_SUPER+super_block)
			return NULL;
		if (!s->s_dev)
			break;
	}
	//�����ֶ� 
	s->s_dev = dev;
	s->s_isup = NULL;
	s->s_imount = NULL;
	s->s_time = 0;
	s->s_rd_only = 0;
	s->s_dirt = 0;
	lock_super(s);
	//��ȡinode=1
	if (!(bh = bread(dev,1))) {
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	*((struct d_super_block *) s) =
		*((struct d_super_block *) bh->b_data);
	brelse(bh);
	//���magic
	if (s->s_magic != SUPER_MAGIC) {
		s->s_dev = 0;
		free_super(s);
		return NULL;
	}
	//��ʼ��inode mapָ������
	for (i=0;i<I_MAP_SLOTS;i++)
		s->s_imap[i] = NULL;
	//��ʼ�����̿�mapָ������
	for (i=0;i<Z_MAP_SLOTS;i++)
		s->s_zmap[i] = NULL;
	block=2;
	//��λͼ�����ڴ�
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
		//������
	if (block != 2+s->s_imap_blocks+s->s_zmap_blocks) {
		for(i=0;i<I_MAP_SLOTS;i++)
			brelse(s->s_imap[i]);
		for(i=0;i<Z_MAP_SLOTS;i++)
			brelse(s->s_zmap[i]);
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	//0��inode������
	s->s_imap[0]->b_data[0] |= 1;
	s->s_zmap[0]->b_data[0] |= 1;
	free_super(s);
	return s;
}

//ж���豸
int sys_umount(char * dev_name)
{
	struct m_inode * inode;
	struct super_block * sb;
	int dev;
	//���������ҵ�inode
	if (!(inode=namei(dev_name)))
		return -ENOENT;
	//ȡ���豸��
	dev = inode->i_zone[0];
	//������Ǵ�����豸 �򷵻�
	if (!S_ISBLK(inode->i_mode)) {
		iput(inode);
		return -ENOTBLK;
	}
	//�Ѿ�����Ҫinode�� �ͷ�֮
	iput(inode);
	//����ж�ظ��豸
	if (dev==ROOT_DEV)
		return -EBUSY;
	//�Ҳ�����Ӧ�ĳ����� ���� û�й��ع� ����
	if (!(sb=get_super(dev)) || !(sb->s_imount))
		return -ENOENT;
	//���ص� ����һ�����ص�
	if (!sb->s_imount->i_mount)
		printk("Mounted inode has i_mount=0\n");
	//�Ƿ��н������ô��豸�ϵ��ļ�
	for (inode=inode_table+0 ; inode<inode_table+NR_INODE ; inode++)
		if (inode->i_dev==dev && inode->i_count)
				return -EBUSY;
	//��ʼ������ж��
	//���ص�inode��mount��־��λ
	sb->s_imount->i_mount=0;
	//�ͷŹ��ص�inode
	iput(sb->s_imount);
	//������Ĺ��ص���Ϊnull
	sb->s_imount = NULL;
	//�ͷ��豸�ĸ�inode
	iput(sb->s_isup);
	sb->s_isup = NULL;
	//�ͷų�����
	put_super(dev);
	//��ͬ��buffer������
	sync_dev(dev);
	return 0;
}

//�����ļ�ϵͳ
int sys_mount(char * dev_name, char * dir_name, int rw_flag)
{
	struct m_inode * dev_i, * dir_i;
	struct super_block * sb;
	int dev;
	//�����豸�ļ��� �ҵ�inode
	if (!(dev_i=namei(dev_name)))
		return -ENOENT;
	//ȡ���豸��
	dev = dev_i->i_zone[0];
	//������ǿ��豸�ļ� �򷵻�
	if (!S_ISBLK(dev_i->i_mode)) {
		iput(dev_i);
		return -EPERM;
	}
	//���ڲ���Ҫinode��
	iput(dev_i);
	//�õ����ص��inode
	if (!(dir_i=namei(dir_name)))
		return -ENOENT;
	//���ص�����ü�������1 ���� �ù��ص��inode���Ǹ�inode�� �򷵻�
	if (dir_i->i_count != 1 || dir_i->i_num == ROOT_INO) {
		iput(dir_i);
		return -EBUSY;
	}
	//����Ŀ¼ ����  ���ص�ֻ����Ŀ¼
	if (!S_ISDIR(dir_i->i_mode)) {
		iput(dir_i);
		return -EPERM;
	}
	//����������
	if (!(sb=read_super(dev))) {
		iput(dir_i);
		return -EBUSY;
	}
	//�Ѿ������� ����
	if (sb->s_imount) {
		iput(dir_i);
		return -EBUSY;
	}
	//���ص��Ѿ������������ļ�ϵͳ ����
	if (dir_i->i_mount) {
		iput(dir_i);
		return -EPERM;
	}
	//�����ļ�ϵͳ�Ĺ��ص�
	sb->s_imount=dir_i;
	//���ù��ص�inode������ֶ�
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
	//file table ��0  ����ϵͳ��file table
	for(i=0;i<NR_FILE;i++)
		file_table[i].f_count=0;
	//���豸������ʱ Ҫ���������
	if (MAJOR(ROOT_DEV) == 2) {
		printk("Insert root floppy and press ENTER");
		wait_for_keypress();
	}
	//��0 ����super block������
	for(p = &super_block[0] ; p < &super_block[NR_SUPER] ; p++) {
		p->s_dev = 0;
		p->s_lock = 0;
		p->s_wait = NULL;
	}
	//��ȡ���豸��super block������������
	if (!(p=read_super(ROOT_DEV)))
		panic("Unable to mount root");
	// ��ȡroot inode
	if (!(mi=iget(ROOT_DEV,ROOT_INO)))
		panic("Unable to read root i-node");
	mi->i_count += 3 ;	/* NOTE! it is logically used 4 times, not 1 */
	//���õ�ǰ�ļ�ϵͳ�Ĺ��ص�͸��ڵ�
	p->s_isup = p->s_imount = mi;
	//����0�Ž��̵�pwd��root��inode
	current->pwd = mi;
	current->root = mi;
	//�������ͳ���µ�ǰ�ļ�ϵͳ��free block����
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
