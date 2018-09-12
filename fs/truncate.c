/*
 *  linux/fs/truncate.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <linux/sched.h>

#include <sys/stat.h>

static int free_ind(int dev,int block)
{
	struct buffer_head * bh;
	unsigned short * p;
	int i;
	int block_busy;

	if (!block)
		return 1;
	block_busy = 0;
	if (bh=bread(dev,block)) {
		p = (unsigned short *) bh->b_data;
		for (i=0;i<512;i++,p++)
			if (*p)
				if (free_block(dev,*p)) {
					*p = 0;
					bh->b_dirt = 1;
				} else
					block_busy = 1;
		brelse(bh);
	}
	if (block_busy)
		return 0;
	else
		return free_block(dev,block);
}

static int free_dind(int dev,int block)
{
	struct buffer_head * bh;
	unsigned short * p;
	int i;
	int block_busy;

	if (!block)
		return 1;
	block_busy = 0;
	if (bh=bread(dev,block)) {
		p = (unsigned short *) bh->b_data;
		for (i=0;i<512;i++,p++)
			if (*p)
				if (free_ind(dev,*p)) {
					*p = 0;
					bh->b_dirt = 1;
				} else
					block_busy = 1;
		brelse(bh);
	}
	if (block_busy)
		return 0;
	else
		return free_block(dev,block);
}

//截断文件数据 释放占用设备空间
void truncate(struct m_inode * inode)
{
	int i;
	int block_busy;
	//不是常规文件 目录文件 和连接文件 则返回
	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
	     S_ISLNK(inode->i_mode)))
		return;
repeat:
	block_busy = 0;
	for (i=0;i<7;i++)
		//释放直接块
		if (inode->i_zone[i]) {
			if (free_block(inode->i_dev,inode->i_zone[i]))
				inode->i_zone[i]=0;
			else
				//没有成功释放 则 置busy
				block_busy = 1;
		}
		//释放一级块
	if (free_ind(inode->i_dev,inode->i_zone[7]))
		inode->i_zone[7] = 0;
	else
		block_busy = 1;
	//释放二级块
	if (free_dind(inode->i_dev,inode->i_zone[8]))
		inode->i_zone[8] = 0;
	else
		block_busy = 1;
	//设置inode dirty=1
	inode->i_dirt = 1;
	if (block_busy) {
		//如果还有块由于忙 没有释放 则切换进程 回来重试
		current->counter = 0;
		schedule();
		goto repeat;
	}
	inode->i_size = 0;
	inode->i_mtime = inode->i_ctime = CURRENT_TIME;
}

