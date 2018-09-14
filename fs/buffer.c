/*
 *  linux/fs/buffer.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'buffer.c' implements the buffer-cache functions. Race-conditions have
 * been avoided by NEVER letting a interrupt change a buffer (except for the
 * data, of course), but instead letting the caller do it. NOTE! As interrupts
 * can wake up a caller, some cli-sti sequences are needed to check for
 * sleep-on-calls. These should be extremely quick, though (I hope).
 */

/*
 * NOTE! There is one discordant note here: checking floppies for
 * disk change. This is where it fits best, I think, as it should
 * invalidate changed floppy-disk-caches.
 */

#include <stdarg.h>
 
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/io.h>

extern int end; /* �ں˴��������λ�� */
struct buffer_head * start_buffer = (struct buffer_head *) &end;
struct buffer_head * hash_table[NR_HASH];
//free listͷָ��
static struct buffer_head * free_list;
static struct task_struct * buffer_wait = NULL;
int NR_BUFFERS = 0;

static inline void wait_on_buffer(struct buffer_head * bh)
{
	//���жϲ���������������Ӱ�� ��Ϊ�л���ʱ��tss��flags���滻���Լ���
	cli();
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
	sti();
}

//ͬ���豸���ڴ���ٻ���
int sys_sync(void)
{
	int i;
	struct buffer_head * bh;
	//����ͬ��inode�����е�inode����  �����ͬ����buffer��
	sync_inodes();		/* write out inodes into buffers */
	bh = start_buffer;
	//Ȼ���ÿ����Ļ�������д�ش���
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		//��buffer���иĶ�ʱ ������ ������������ȵȴ�����
		wait_on_buffer(bh);
		if (bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

//��ָ���豸����ͬ��
int sync_dev(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		//������ָ�����豸 ����д�ش�������
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	//ͬ��inode�����ٻ���
	sync_inodes();
	//Ȼ������һ�� ��Ϊͬ��inode��ʹ��buffer����
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

//��ָ���豸�ڸ��ٻ����е�bufferȫ����Ч
void inline invalidate_buffers(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev)
			bh->b_uptodate = bh->b_dirt = 0;
	}
}

/*
 * This routine checks whether a floppy has been changed, and
 * invalidates all buffer-cache-entries in that case. This
 * is a relatively slow routine, so we have to try to minimize using
 * it. Thus it is called only upon a 'mount' or 'open'. This
 * is the best way of combining speed and utility, I think.
 * People changing diskettes in the middle of an operation deserve
 * to loose :-)
 *
 * NOTE! Although currently this is only for floppies, the idea is
 * that any additional removable block-device will use this routine,
 * and that mount/open needn't know that floppies/whatever are
 * special.
 */
 //�������Ƿ���� ��������� ��ʹ�ö�Ӧbuffer��Ч
void check_disk_change(int dev)
{
	int i;
	//ֻ֧������
	if (MAJOR(dev) != 2)
		return;
	//����û�и��� �˳�
	if (!floppy_change(dev & 0x03))
		return;
	//�������ͷų�����
	for (i=0 ; i<NR_SUPER ; i++)
		if (super_block[i].s_dev == dev)
			put_super(super_block[i].s_dev);
		//Ȼ����Чinode��buffer
	invalidate_inodes(dev);
	invalidate_buffers(dev);
}

#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH)
//hash��ͷָ������
#define hash(dev,block) hash_table[_hashfn(dev,block)]

static inline void remove_from_queues(struct buffer_head * bh)
{
/* remove from hash-queue */
	if (bh->b_next)
		bh->b_next->b_prev = bh->b_prev;
	if (bh->b_prev)
		bh->b_prev->b_next = bh->b_next;
	if (hash(bh->b_dev,bh->b_blocknr) == bh)
		hash(bh->b_dev,bh->b_blocknr) = bh->b_next;
/* remove from free list */
	if (!(bh->b_prev_free) || !(bh->b_next_free))
		panic("Free block list corrupted");
	bh->b_prev_free->b_next_free = bh->b_next_free;
	bh->b_next_free->b_prev_free = bh->b_prev_free;
	if (free_list == bh)
		free_list = bh->b_next_free;
}

static inline void insert_into_queues(struct buffer_head * bh)
{
/* put at end of free list */
	bh->b_next_free = free_list;
	bh->b_prev_free = free_list->b_prev_free;
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
/* put the buffer in new hash-queue if it has a device */
	bh->b_prev = NULL;
	bh->b_next = NULL;
	//���buffer�ж�Ӧ�豸 ��ŵ�hash����
	if (!bh->b_dev)
		return;
	bh->b_next = hash(bh->b_dev,bh->b_blocknr);
	//hashͷָ���²����buffer
	hash(bh->b_dev,bh->b_blocknr) = bh;
	bh->b_next->b_prev = bh;
}

//��hash�����Ҷ�Ӧ�豸��block�ŵ�buffer
static struct buffer_head * find_buffer(int dev, int block)
{		
	struct buffer_head * tmp;

	for (tmp = hash(dev,block) ; tmp != NULL ; tmp = tmp->b_next)
		if (tmp->b_dev==dev && tmp->b_blocknr==block)
			return tmp;
	return NULL;
}

/*
 * Why like this, I hear you say... The reason is race-conditions.
 * As we don't lock buffers (unless we are readint them, that is),
 * something might happen to it while we sleep (ie a read-error
 * will force it bad). This shouldn't really happen currently, but
 * the code is ready.
 */
 //����hash����ָ��dev��block�ŵ�buffer �ҵ�����������
struct buffer_head * get_hash_table(int dev, int block)
{
	struct buffer_head * bh;

	for (;;) {
		//�Ҳ��� ֱ���˳�
		if (!(bh=find_buffer(dev,block)))
			return NULL;
		//��������
		bh->b_count++;
		wait_on_buffer(bh);
		//˯�������� ����Ŷ���һ��
		if (bh->b_dev == dev && bh->b_blocknr == block)
			return bh;
		bh->b_count--;
	}
}

/*
 * Ok, this is getblk, and it isn't very clear, again to hinder
 * race-conditions. Most of the code is seldom used, (ie repeating),
 * so it should be much more efficient than it looks.
 *
 * The algoritm is changed: hopefully better, and an elusive bug removed.
 */
#define BADNESS(bh) (((bh)->b_dirt<<1)+(bh)->b_lock)
//����Ӧdev��block��buffer�Ƿ��ڻ������� �����������Ҫ����һ��������
//�����û�ж�Ӧ��buffer��hash�� ������Ҫ��һ��count=0��buffer
struct buffer_head * getblk(int dev,int block)
{
	struct buffer_head * tmp, * bh;

repeat:
	//��hash���ҵ� ֱ�ӷ���
	if (bh = get_hash_table(dev,block))
		return bh;
	//���� ��free list����
	tmp = free_list;
	do {
		//���ڱ����� ����
		if (tmp->b_count)
			continue;
		// ��ͼ�ҵ�һ��badness��С��buffer
		if (!bh || BADNESS(tmp)<BADNESS(bh)) {
			bh = tmp;
			//badness=0 ��ζ�Ÿɾ���û������ ���ҵ� ����break
			if (!BADNESS(tmp))
				break;
		}
/* and repeat until we find something good */
	} while ((tmp = tmp->b_next_free) != free_list);
	//���л���� �������� ��˯�ߵȴ�
	if (!bh) {
		sleep_on(&buffer_wait);
		goto repeat;
	}
	//�ȴ����� �����ж��Ƿ������������� ��buffer ����� ��������
	wait_on_buffer(bh);
	if (bh->b_count)
		goto repeat;
	//����ҵ���buffer�����  ��Ҫͬ��������
	while (bh->b_dirt) {
		sync_dev(bh->b_dev);
		wait_on_buffer(bh);
		//ͬ�������� Ҫ����Ƿ����±��������� ����� ��Ҫ������
		if (bh->b_count)
			goto repeat;
	}
/* NOTE!! While we slept waiting for this block, somebody else might */
/* already have added "this" block to the cache. check it */
//�����˯�߹����� ���ܱ�Ľ��� �Ѿ�����Ӧ��buffer���뵽hash�� ����ҲҪ������һ��
//ֱ�Ӵ�hash��ȡbuffer
	if (find_buffer(dev,block))
		goto repeat;
/* OK, FINALLY we know that this buffer is the only one of it's kind, */
/* and that it's unused (b_count=0), unlocked (b_lock=0), and clean */
//���ڵõ�һ������ָ���� û�б�ռ�õ� ���Ǹɾ���
//����count=1
	bh->b_count=1; 
//�ɾ�
	bh->b_dirt=0;
//��Ч
	bh->b_uptodate=0;
//��remove
	remove_from_queues(bh);
//��Ӧ�豸
	bh->b_dev=dev;
	//������Ӧ�Ĵ��̿�
	bh->b_blocknr=block;
	//�ٲ���  ���뵽free listβ��
	insert_into_queues(bh);
	return bh;
}

	//�ͷ�buffer ����count--
void brelse(struct buffer_head * buf)
{
	if (!buf)
		return;
	wait_on_buffer(buf);
	//�ͷ�buffer ����count--
	if (!(buf->b_count--))
		panic("Trying to free free buffer");
	//�����ͷ���buffer ���ѵȴ�����buffer�Ľ���
	wake_up(&buffer_wait);
}

/*
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 */
 //��ȡһ��block��buffer��
struct buffer_head * bread(int dev,int block)
{
	struct buffer_head * bh;

	//�ȳ��Ի�ȡһ��buffer  �������������
	if (!(bh=getblk(dev,block)))
		panic("bread: getblk returned NULL\n");
	//�����Ч ����������� ֱ�ӷ���buffer
	if (bh->b_uptodate)
		return bh;
	//����������������
	ll_rw_block(READ,bh);
	//�ȴ��̻�lock buffer ���ǵȽ���
	wait_on_buffer(bh);
	//������ ����Ƿ��Ѿ�uptodate ����� �򷵻�buffer
	if (bh->b_uptodate)
		return bh;
	//�����̴���? �����ͷ�֮ǰ�����buffer
	brelse(bh);
	return NULL;
}

//����1024�ֽ�
#define COPYBLK(from,to) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"movsl\n\t" \
	::"c" (BLOCK_SIZE/4),"S" (from),"D" (to) \
	:"cx","di","si")

/*
 * bread_page reads four buffers into memory at the desired address. It's
 * a function of its own, as there is some speed to be got by reading them
 * all at the same time, not waiting for one to be read, and then another
 * etc.
 */
 //һ�ζ�4k �ĸ�����鵽ָ����ַ
void bread_page(unsigned long address,int dev,int b[4] /* block������ */)
{
	struct buffer_head * bh[4];
	int i;
	//ѭ��4��
	for (i=0 ; i<4 ; i++)
		if (b[i]) {
			//��ȡ��Ӧdev��block�ŵ�buffer
			if (bh[i] = getblk(dev,b[i]))
				//��Ч ��Ӵ��̶�
				if (!bh[i]->b_uptodate)
					ll_rw_block(READ,bh[i]);
		} else
			bh[i] = NULL;
	for (i=0 ; i<4 ; i++,address += BLOCK_SIZE)
		//��buffer������ָ����ַ
		if (bh[i]) {
			wait_on_buffer(bh[i]);
			if (bh[i]->b_uptodate)
				COPYBLK((unsigned long) bh[i]->b_data,address);
			brelse(bh[i]);
		}
}

/*
 * Ok, breada can be used as bread, but additionally to mark other
 * blocks for reading as well. End the argument list with a negative
 * number.
 */
struct buffer_head * breada(int dev,int first, ...)
{
	va_list args;
	struct buffer_head * bh, *tmp;

	va_start(args,first);
	//��ȡһ��block��Ӧ��buffer
	if (!(bh=getblk(dev,first)))
		panic("bread: getblk returned NULL\n");
	//��Ч �������̶�
	if (!bh->b_uptodate)
		ll_rw_block(READ,bh);
	//�����  ������ָ����block��
	while ((first=va_arg(args,int))>=0) {
		tmp=getblk(dev,first);
		if (tmp) {
			//��Ч �������̶�
			if (!tmp->b_uptodate)
				//bug Ӧ����tmp
				ll_rw_block(READA,bh);
			//���� ���ǲ����������� ��� cnt--  Ԥ��
			tmp->b_count--;
		}
	}
	va_end(args);
	//���ص�һ�����Ӧ��buffer
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return (NULL);
}

//buffer�������������buffer 1k֮�⣬�������������buffer head
void buffer_init(long buffer_end)
{
	struct buffer_head * h = start_buffer; /* �ں˴�������ݽ����ġ���ַ ?*/
	void * b;
	int i;
	//buffer end=1mʱ��������endΪ640k
	if (buffer_end == 1<<20)
		b = (void *) (640*1024);
	else
		b = (void *) buffer_end;
	//��Ҫ����bios rom����ʾ���棬Ȼ�����buffer��1k��С�������������
	while ( (b -= BLOCK_SIZE) >= ((void *) (h+1)) ) { /* buffers����ڴ��ǰ����buffer head��ռ�ã����ڹ���buffer */
		h->b_dev = 0;    /* 1k��ǰ��1k�������ʼ��ַ��buffer_headռ���ڴ�Ľ�����ַ֮ǰ�Ƿ����㹻�ռ�����һ��bufer_head */
		h->b_dirt = 0;
		h->b_count = 0;
		h->b_lock = 0;
		h->b_uptodate = 0;
		h->b_wait = NULL;
		h->b_next = NULL;
		h->b_prev = NULL;
		h->b_data = (char *) b; /* ָ��1k buffer����ʼ��ַ */
		h->b_prev_free = h-1;
		h->b_next_free = h+1; 
		h++; /* ��һ��buffer_head�ĵ�ַ */
		NR_BUFFERS++;
		if (b == (void *) 0x100000) /* ����bios?  640k-1024k��bios���Դ�ռ��*/
			b = (void *) 0xA0000; /* ָ��640k��ַ�� */
	}
	h--; /* hָ�����һ����Ч��buffer head  ��ǰ���whileѭ���������Ҫ-- */
	free_list = start_buffer; /* free listָ��ͷһ������� */
	free_list->b_prev_free = h; /* ��һ��buffer_headָ���1k�������ڵ�ַ�������һ��������prevָ��h(��ָ���ַ��С��1k������)���γɻ������� */
	h->b_next_free = free_list; /* �γɻ������� */
	for (i=0;i<NR_HASH;i++)
		hash_table[i]=NULL;
}	
