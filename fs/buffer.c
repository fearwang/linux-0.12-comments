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

extern int end; /* 内核代码结束的位置 */
struct buffer_head * start_buffer = (struct buffer_head *) &end;
struct buffer_head * hash_table[NR_HASH];
//free list头指针
static struct buffer_head * free_list;
static struct task_struct * buffer_wait = NULL;
int NR_BUFFERS = 0;

static inline void wait_on_buffer(struct buffer_head * bh)
{
	//关中断不会对其他进程造成影响 因为切换的时候tss的flags会替换成自己的
	cli();
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
	sti();
}

//同步设备和内存高速缓冲
int sys_sync(void)
{
	int i;
	struct buffer_head * bh;
	//首先同步inode数组中的inode数据  这个会同步到buffer中
	sync_inodes();		/* write out inodes into buffers */
	bh = start_buffer;
	//然后对每个脏的缓冲区进写回磁盘
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		//对buffer进行改动时 会上锁 因此这里我们先等待解锁
		wait_on_buffer(bh);
		if (bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

//对指定设备进行同步
int sync_dev(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		//仅仅对指定的设备 产生写回磁盘请求
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	//同步inode到高速缓冲
	sync_inodes();
	//然后再来一次 因为同步inode会使得buffer变脏
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

//让指定设备在高速缓冲中的buffer全部无效
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
 //检查磁盘是否更换 如果更换了 则使得对应buffer无效
void check_disk_change(int dev)
{
	int i;
	//只支持软盘
	if (MAJOR(dev) != 2)
		return;
	//软盘没有更换 退出
	if (!floppy_change(dev & 0x03))
		return;
	//否则先释放超级块
	for (i=0 ; i<NR_SUPER ; i++)
		if (super_block[i].s_dev == dev)
			put_super(super_block[i].s_dev);
		//然后无效inode和buffer
	invalidate_inodes(dev);
	invalidate_buffers(dev);
}

#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH)
//hash表头指针数组
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
	//如果buffer有对应设备 则放到hash表中
	if (!bh->b_dev)
		return;
	bh->b_next = hash(bh->b_dev,bh->b_blocknr);
	//hash头指向新插入的buffer
	hash(bh->b_dev,bh->b_blocknr) = bh;
	bh->b_next->b_prev = bh;
}

//从hash表中找对应设备和block号的buffer
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
 //利用hash表找指定dev和block号的buffer 找到则增加引用
struct buffer_head * get_hash_table(int dev, int block)
{
	struct buffer_head * bh;

	for (;;) {
		//找不到 直接退出
		if (!(bh=find_buffer(dev,block)))
			return NULL;
		//增加引用
		bh->b_count++;
		wait_on_buffer(bh);
		//睡眠醒来后 还有哦检查一次
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
//检查对应dev和block的buffer是否在缓冲区中 如果不在则需要申请一个并返回
//如果是没有对应的buffer在hash中 我们需要找一个count=0的buffer
struct buffer_head * getblk(int dev,int block)
{
	struct buffer_head * tmp, * bh;

repeat:
	//从hash中找到 直接返回
	if (bh = get_hash_table(dev,block))
		return bh;
	//否则 从free list中找
	tmp = free_list;
	do {
		//正在被引用 跳过
		if (tmp->b_count)
			continue;
		// 试图找到一个badness最小的buffer
		if (!bh || BADNESS(tmp)<BADNESS(bh)) {
			bh = tmp;
			//badness=0 意味着干净且没有上锁 则找到 我们break
			if (!BADNESS(tmp))
				break;
		}
/* and repeat until we find something good */
	} while ((tmp = tmp->b_next_free) != free_list);
	//所有缓冲块 都被引用 则睡眠等待
	if (!bh) {
		sleep_on(&buffer_wait);
		goto repeat;
	}
	//等待解锁 醒来判断是否又有人引用了 该buffer 如果是 则重新找
	wait_on_buffer(bh);
	if (bh->b_count)
		goto repeat;
	//如果找到的buffer是脏的  还要同步到磁盘
	while (bh->b_dirt) {
		sync_dev(bh->b_dev);
		wait_on_buffer(bh);
		//同样醒来后 要检查是否重新被人引用了 如果是 则要重新找
		if (bh->b_count)
			goto repeat;
	}
/* NOTE!! While we slept waiting for this block, somebody else might */
/* already have added "this" block to the cache. check it */
//上面的睡眠过程中 可能别的进程 已经将对应的buffer加入到hash中 我们也要重新找一次
//直接从hash中取buffer
	if (find_buffer(dev,block))
		goto repeat;
/* OK, FINALLY we know that this buffer is the only one of it's kind, */
/* and that it's unused (b_count=0), unlocked (b_lock=0), and clean */
//终于得到一个参数指定的 没有被占用的 且是干净的
//设置count=1
	bh->b_count=1; 
//干净
	bh->b_dirt=0;
//无效
	bh->b_uptodate=0;
//先remove
	remove_from_queues(bh);
//对应设备
	bh->b_dev=dev;
	//缓冲块对应的磁盘块
	bh->b_blocknr=block;
	//再插入  插入到free list尾部
	insert_into_queues(bh);
	return bh;
}

	//释放buffer 就是count--
void brelse(struct buffer_head * buf)
{
	if (!buf)
		return;
	wait_on_buffer(buf);
	//释放buffer 就是count--
	if (!(buf->b_count--))
		panic("Trying to free free buffer");
	//有人释放了buffer 唤醒等待空闲buffer的进程
	wake_up(&buffer_wait);
}

/*
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 */
 //读取一个block到buffer中
struct buffer_head * bread(int dev,int block)
{
	struct buffer_head * bh;

	//先尝试获取一个buffer  可能是新申请的
	if (!(bh=getblk(dev,block)))
		panic("bread: getblk returned NULL\n");
	//如果有效 则无需读磁盘 直接返回buffer
	if (bh->b_uptodate)
		return bh;
	//否则发生读磁盘请求
	ll_rw_block(READ,bh);
	//度磁盘会lock buffer 我们等解锁
	wait_on_buffer(bh);
	//醒来后 检查是否已经uptodate 如果是 则返回buffer
	if (bh->b_uptodate)
		return bh;
	//读磁盘错误? 我们释放之前申请的buffer
	brelse(bh);
	return NULL;
}

//复制1024字节
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
 //一次读4k 四个缓冲块到指定地址
void bread_page(unsigned long address,int dev,int b[4] /* block号数组 */)
{
	struct buffer_head * bh[4];
	int i;
	//循环4次
	for (i=0 ; i<4 ; i++)
		if (b[i]) {
			//获取对应dev和block号的buffer
			if (bh[i] = getblk(dev,b[i]))
				//无效 则从磁盘读
				if (!bh[i]->b_uptodate)
					ll_rw_block(READ,bh[i]);
		} else
			bh[i] = NULL;
	for (i=0 ; i<4 ; i++,address += BLOCK_SIZE)
		//从buffer拷贝到指定地址
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
	//获取一个block对应的buffer
	if (!(bh=getblk(dev,first)))
		panic("bread: getblk returned NULL\n");
	//无效 发生磁盘读
	if (!bh->b_uptodate)
		ll_rw_block(READ,bh);
	//多参数  读后面指定的block号
	while ((first=va_arg(args,int))>=0) {
		tmp=getblk(dev,first);
		if (tmp) {
			//无效 发生磁盘读
			if (!tmp->b_uptodate)
				//bug 应该是tmp
				ll_rw_block(READA,bh);
			//读完 但是不会马上引用 因此 cnt--  预读
			tmp->b_count--;
		}
	}
	va_end(args);
	//返回第一个块对应的buffer
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return (NULL);
}

//buffer里面除了真正的buffer 1k之外，还有用来管理的buffer head
void buffer_init(long buffer_end)
{
	struct buffer_head * h = start_buffer; /* 内核代码和数据结束的·地址 ?*/
	void * b;
	int i;
	//buffer end=1m时从新设置end为640k
	if (buffer_end == 1<<20)
		b = (void *) (640*1024);
	else
		b = (void *) buffer_end;
	//需要跳过bios rom和显示缓存，然后将这段buffer以1k大小用链表管理起来
	while ( (b -= BLOCK_SIZE) >= ((void *) (h+1)) ) { /* buffers这段内存的前面是buffer head所占用，用于管理buffer */
		h->b_dev = 0;    /* 1k当前的1k区域的起始地址和buffer_head占用内存的结束地址之前是否还有足够空间容纳一个bufer_head */
		h->b_dirt = 0;
		h->b_count = 0;
		h->b_lock = 0;
		h->b_uptodate = 0;
		h->b_wait = NULL;
		h->b_next = NULL;
		h->b_prev = NULL;
		h->b_data = (char *) b; /* 指向1k buffer的起始地址 */
		h->b_prev_free = h-1;
		h->b_next_free = h+1; 
		h++; /* 下一个buffer_head的地址 */
		NR_BUFFERS++;
		if (b == (void *) 0x100000) /* 跳过bios?  640k-1024k被bios和显存占用*/
			b = (void *) 0xA0000; /* 指向640k地址处 */
	}
	h--; /* h指向最后一个有效的buffer head  ，前面的while循环决定最后要-- */
	free_list = start_buffer; /* free list指向头一个缓冲块 */
	free_list->b_prev_free = h; /* 第一个buffer_head指向的1k缓冲区在地址上是最后一个，它的prev指向h(其指向地址最小的1k缓冲区)，形成环形链表 */
	h->b_next_free = free_list; /* 形成环形链表 */
	for (i=0;i<NR_HASH;i++)
		hash_table[i]=NULL;
}	
