/*
 *  linux/kernel/blk_dev/ll_rw.c
 *
 * (C) 1991 Linus Torvalds
 */

/*
 * This handles all read/write requests to block devices
 */
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include "blk.h"

/*
 * The request-struct contains all necessary data
 * to load a nr of sectors into memory
 */
struct request request[NR_REQUEST];

/*
 * used to wait on when there are no free requests
 */
struct task_struct * wait_for_request = NULL;

/* blk_dev_struct is:
 *	do_request-address
 *	next-request
 */

//���豸��������
struct blk_dev_struct blk_dev[NR_BLK_DEV] = {
	{ NULL, NULL },		/* no_dev */
	{ NULL, NULL },		/* dev mem */
	{ NULL, NULL },		/* dev fd */
	{ NULL, NULL },		/* dev hd */
	{ NULL, NULL },		/* dev ttyx */
	{ NULL, NULL },		/* dev tty */
	{ NULL, NULL }		/* dev lp */
};

/*
 * blk_size contains the size of all block-devices:
 *
 * blk_size[MAJOR][MINOR]
 *
 * if (!blk_size[MAJOR]) then no minor size checking is done.
 */
int * blk_size[NR_BLK_DEV] = { NULL, NULL, };

static inline void lock_buffer(struct buffer_head * bh)
{
	cli();
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
	bh->b_lock=1;
	sti();
}

static inline void unlock_buffer(struct buffer_head * bh)
{
	if (!bh->b_lock)
		printk("ll_rw_block.c: buffer not locked\n\r");
	bh->b_lock = 0;
	wake_up(&bh->b_wait);
}

/*
 * add-request adds a request to the linked list.
 * It disables interrupts so that it can muck with the
 * request-lists in peace.
 *
 * Note that swapping requests always go before other requests,
 * and are done in the order they appear.
 */
 //�����������һ��request��
static void add_request(struct blk_dev_struct * dev, struct request * req)
{
	struct request * tmp;

	req->next = NULL;
	cli();
	if (req->bh)
		req->bh->b_dirt = 0;
	//����ǰ�豸�Ƿ���û�д����request
	if (!(tmp = dev->current_request)) {
		//���û��  ��ǰadd����Ψһһ��
		dev->current_request = req;
		sti();
		//��������ִ�����request  fnһ��ִ�� Ӧ�û�һֱ���� �ҵ�����Ϊ��
		(dev->request_fn)();
		return;
	}
	//����Ļ����뵽���ʵ�λ�� ��ν�����㷨
	for ( ; tmp->next ; tmp=tmp->next) {
		if (!req->bh)
			if (tmp->next->bh)
				break;
			else
				continue;
		if ((IN_ORDER(tmp,req) ||
		    !IN_ORDER(tmp,tmp->next)) &&
		    IN_ORDER(req,tmp->next))
			break;
	}
	//��������
	req->next=tmp->next;
	tmp->next=req;
	sti();
}

static void make_request(int major,int rw, struct buffer_head * bh)
{
	struct request * req;
	int rw_ahead;

/* WRITEA/READA is special case - it is not really needed, so if the */
/* buffer is locked, we just forget about it, else it's a normal read */
	if (rw_ahead = (rw == READA || rw == WRITEA)) {
		//Ԥ��
		if (bh->b_lock)
			//buffer���� ���� �޷��޸�buffer
			return;
		if (rw == READA)
			rw = READ;
		else
			rw = WRITE;
	}
	if (rw!=READ && rw!=WRITE)
		//�Ƕ� ��д
		panic("Bad block dev command, must be R/W/RA/WA");
	//����bufferǰ Ҫlockbuffer
	lock_buffer(bh);
	//��д�ɾ���buffer ���� �� �Ѿ� ��Ч�� buffer �Ƕ����
	if ((rw == WRITE && !bh->b_dirt) || (rw == READ && bh->b_uptodate)) {
		unlock_buffer(bh);
		return;
	}
repeat:
/* we don't allow the write-requests to fill up the queue completely:
 * we want some room for reads: they take precedence. The last third
 * of the requests are only for reads.
 */
 //��дrequest ����?
	if (rw == READ)
		req = request+NR_REQUEST;
	else
		req = request+((NR_REQUEST*2)/3);
/* find an empty request */
//��һ�����е� request ������
	while (--req >= request)
		if (req->dev<0)
			break;
/* if none found, sleep on new requests: check for rw_ahead */
//û�ҵ� 
	if (req < request) {
		//�����Ԥ�� ����ֱ�ӷ���
		if (rw_ahead) {
			unlock_buffer(bh);
			return;
		}
		//������Ԥ��  �ȴ����е�request������
		sleep_on(&wait_for_request);
		goto repeat;
	}
/* fill up the request-info, and add it to the queue */
//�ߵ����� �����Ѿ��õ���һ��request������
	req->dev = bh->b_dev;
	req->cmd = rw;
	req->errors=0;
	//��� ת����������
	req->sector = bh->b_blocknr<<1;
	//һ�����2 ����
	req->nr_sectors = 2;
	req->buffer = bh->b_data;
	req->waiting = NULL;
	req->bh = bh;
	req->next = NULL;
	//���request
	add_request(major+blk_dev,req);
}

void ll_rw_page(int rw, int dev, int page, char * buffer)
{
	struct request * req;
	unsigned int major = MAJOR(dev);

	if (major >= NR_BLK_DEV || !(blk_dev[major].request_fn)) {
		printk("Trying to read nonexistent block-device\n\r");
		return;
	}
	if (rw!=READ && rw!=WRITE)
		panic("Bad block dev command, must be R/W");
repeat:
	req = request+NR_REQUEST;
	while (--req >= request)
		if (req->dev<0)
			break;
	if (req < request) {
		sleep_on(&wait_for_request);
		goto repeat;
	}
/* fill up the request-info, and add it to the queue */
	req->dev = dev;
	req->cmd = rw;
	req->errors = 0;
	req->sector = page<<3;
	req->nr_sectors = 8;
	req->buffer = buffer;
	req->waiting = current;
	req->bh = NULL;
	req->next = NULL;
	current->state = TASK_UNINTERRUPTIBLE;
	add_request(major+blk_dev,req);
	schedule();
}	

//����Ƕ� �������Ƿ���buffer��������
void ll_rw_block(int rw, struct buffer_head * bh)
{
	unsigned int major;

	//���ܳ����������豸��  ��Ӧ�豸��request fn����Ϊ��
	if ((major=MAJOR(bh->b_dev)) >= NR_BLK_DEV ||
	!(blk_dev[major].request_fn)) {
		printk("Trying to read nonexistent block-device\n\r");
		return;
	}
	//����һ��request
	make_request(major,rw,bh);
}

//���豸��ʼ����IO���������ʼ��
//֮���Ӧ����Ŀ��豸 ��ʼ��ʱ������request fn
void blk_dev_init(void)
{
	int i;

	for (i=0 ; i<NR_REQUEST ; i++) {
		request[i].dev = -1;
		request[i].next = NULL;
	}
}
