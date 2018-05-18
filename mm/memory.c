/*
 *  linux/mm/memory.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * demand-loading started 01.12.91 - seems it is high on the list of
 * things wanted, and it should be easy to implement. - Linus
 */

/*
 * Ok, demand-loading was easy, shared pages a little bit tricker. Shared
 * pages started 02.12.91, seems to work. - Linus.
 *
 * Tested sharing by executing about 30 /bin/sh: under the old kernel it
 * would have taken more than the 6M I have free, but it worked well as
 * far as I could see.
 *
 * Also corrected some "invalidate()"s - I wasn't doing enough of them.
 */

/*
 * Real VM (paging to/from disk) started 18.12.91. Much more work and
 * thought has to go into this. Oh, well..
 * 19.12.91  -  works, somewhat. Sometimes I get faults, don't know why.
 *		Found it. Everything seems to work now.
 * 20.12.91  -  Ok, making the swap-device changeable like the root.
 */

#include <signal.h>

#include <asm/system.h>

#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>

//判断虚拟地址是否在代码段
#define CODE_SPACE(addr) ((((addr)+4095)&~4095) < \
current->start_code + current->end_code)

//内存最高地址
unsigned long HIGH_MEMORY = 0;

//复制1页到to处
#define copy_page(from,to) \
__asm__("cld ; rep ; movsl"::"S" (from),"D" (to),"c" (1024):"cx","di","si")

//描述没个页的状态
unsigned char mem_map [ PAGING_PAGES ] = {0,};

/*
 * Free a page of memory at physical address 'addr'. Used by
 * 'free_page_tables()'
 */
void free_page(unsigned long addr /* 页面物理地址 */)
{
	//主存区不能小于1m，那是内核和buffer区域
	if (addr < LOW_MEM) return;
	// 同样不能大于内存最大地址
	if (addr >= HIGH_MEMORY)
		panic("trying to free nonexistent page");
	addr -= LOW_MEM; //mem map数组管理的1-16m内存
	addr >>= 12;//计算物理地址对应的页面号
	//引用计数减1
	if (mem_map[addr]--) return;
	mem_map[addr]=0;
	panic("trying to free free page");
}

/*
 * This function frees a continuos block of page tables, as needed
 * by 'exit()'. As does copy_page_tables(), this handles only 4Mb blocks.
 */
 //只能处理4mb对其的地址，即页目录项映射的范围 
int free_page_tables(unsigned long from  /* 线性地址 */,unsigned long size /* 释放的长度 */)
{
	unsigned long *pg_table;
	unsigned long * dir, nr;

	if (from & 0x3fffff)
		panic("free_page_tables called with wrong alignment");
	if (!from)
		panic("Trying to free up swapper memory space");
	//size转换成有多少个4mb，每个对应一个页目录项
	size = (size + 0x3fffff) >> 22;
	//页目录表从0开始，这里取到对应的页目录项
	dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	//循环处理每个页目录项
	for ( ; size-->0 ; dir++) {
		//p位=0，即对应页目录项是不在内存中的，什么也不做
		if (!(1 & *dir))
			continue;
		pg_table = (unsigned long *) (0xfffff000 & *dir); /* 取页表的base地址 */
		for (nr=0 ; nr<1024 ; nr++) { /* 处理1204项 页表项 */
			if (*pg_table) {/* 页表项有内容 不为0 */
				if (1 & *pg_table) /* p位=1 ，需要free */
					free_page(0xfffff000 & *pg_table); /* 入参是页面的物理base地址  此函数就是将page的引用-- */
				else
					//此时页表项中保存的是什么? 像是swap的索引 而不是对应的物理页面的base地址
					swap_free(*pg_table >> 1); /* 页表项不为0，但是p=0，此页可能被swap 需要从swap中free */
				*pg_table = 0; /* free完后清空页表项 */
			}
			pg_table++; /* 处理下一项 */
		}
		free_page(0xfffff000 & *dir); /* 释放页表本身的那一页内存 */
		*dir = 0; /* 清空页目录项 */
	}
	invalidate(); /* 刷新cpu页变换高速缓冲区 */
	return 0;
}

/*
 *  Well, here is one of the most complicated functions in mm. It
 * copies a range of linerar addresses by copying only the pages.
 * Let's hope this is bug-free, 'cause this one I don't want to debug :-)
 *
 * Note! We don't copy just any chunks of memory - addresses have to
 * be divisible by 4Mb (one page-directory entry), as this makes the
 * function easier. It's used only by fork anyway.
 *
 * NOTE 2!! When from==0 we are copying kernel space for the first
 * fork(). Then we DONT want to copy a full page-directory entry, as
 * that would lead to some serious memory waste - we just copy the
 * first 160 pages - 640kB. Even that is more than we need, but it
 * doesn't take any more memory - we don't copy-on-write in the low
 * 1 Mb-range, so the pages can be shared with the kernel. Thus the
 * special case for nr=xxxx.
 */
//复制页目录项和页表项
int copy_page_tables(unsigned long from /* 线性地址 */,unsigned long to/* 线性地址 */,long size /* 长度 */)
{
	unsigned long * from_page_table;
	unsigned long * to_page_table;
	unsigned long this_page;
	unsigned long * from_dir, * to_dir;
	unsigned long new_page;
	unsigned long nr;
	//from和to都必须在4mb地址对齐
	if ((from&0x3fffff) || (to&0x3fffff))
		panic("copy_page_tables called with wrong alignment");
	//源线性地址对应的页目录项
	from_dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	//目的线性地址对应的页目录项
	to_dir = (unsigned long *) ((to>>20) & 0xffc);
	//size同样取4mb的整数  ， 8mb->2
	size = ((unsigned) (size+0x3fffff)) >> 22;
	//循环处理每个4mb
	for( ; size-->0 ; from_dir++,to_dir++) {
		if (1 & *to_dir) /* 目的页目录项p=1，即已经存在，panic */
			panic("copy_page_tables: already exist");
		if (!(1 & *from_dir)) /* 源页目录项 p=0 即不可用， continue */
			continue;
		//取出源页目录项指向的页表基地址
		from_page_table = (unsigned long *) (0xfffff000 & *from_dir);
		if (!(to_page_table = (unsigned long *) get_free_page())) /* 申请一页做目的页表 */
			return -1;	/* Out of memory, see freeing */
		*to_dir = ((unsigned long) to_page_table) | 7; /*填充目的页目录项， 表示内存页表可读可写，存在， 用户级 */
		nr = (from==0)?0xA0:1024; /* 内核空间只要复制160项 */
		for ( ; nr-- > 0 ; from_page_table++,to_page_table++) { /* 逐个复制页表项 */
			this_page = *from_page_table;
			if (!this_page) /* 源页表项空 跳过 */
				continue;
			if (!(1 & this_page)) { /* 源页表项代表的页被swap出去 */
				if (!(new_page = get_free_page())) /* 则分配一个页 并从swap拷贝出来到新分配的页 */
					return -1;
				read_swap_page(this_page>>1, (char *) new_page);
				*to_page_table = this_page; /* 目的页表项的值设置为源页表项内容，则目的页表项对应的页此时在swap中 */
				*from_page_table = new_page | (PAGE_DIRTY | 7);/* 源页表项指向新分配的页 并设置为dirty ，即源页表项对应的页此时已经在内存中*/
				//即此时没有共享页面 ，为啥?
				continue;
			}
			this_page &= ~2;/* 让源页表项对应的内存页只读 */
			*to_page_table = this_page;/* 目的页表项的值设置为源页表项内容，此时目的页表项变成只读 */
			if (this_page > LOW_MEM) {/*  */
				*from_page_table = this_page;/* 重新设置源页表项 此时源页表项也变成只读 */
				this_page -= LOW_MEM;/*  */
				this_page >>= 12;/*  */
				mem_map[this_page]++;/* 增加只读共享的页的计数 */
			}
		}
	}
	invalidate();/*  */
	return 0;
}

/*
 * This function puts a page in memory at the wanted address.
 * It returns the physical address of the page gotten, 0 if
 * out of memory (either when trying to access page-table or
 * page.)
 */
 /* 将一页映射到指定的虚拟地址 */
static unsigned long put_page(unsigned long page /* 页的物理地址 */,unsigned long address /* 要映射的目的虚拟地址 */)
{
	unsigned long tmp, *page_table;

/* NOTE !!! This uses the fact that _pg_dir=0 */

	if (page < LOW_MEM || page >= HIGH_MEMORY) /* 检查页面地址范围 */
		printk("Trying to put page %p at %p\n",page,address);
	if (mem_map[(page-LOW_MEM)>>12] != 1) /* 要映射的页没有申请? */
		printk("mem_map disagrees with %p at %p\n",page,address);
	page_table = (unsigned long *) ((address>>20) & 0xffc); /* 计算虚拟地址对应的页目录项 */
	if ((*page_table)&1)
		//如果页目录项存在，则取出其指向的页表地址
		page_table = (unsigned long *) (0xfffff000 & *page_table);
	else {
		//否则要申请一页，并将页目录项指向新申请的页表
		if (!(tmp=get_free_page()))
			return 0;
		*page_table = tmp | 7;
		page_table = (unsigned long *) tmp;/*项指向新申请的页表 */
	}
	page_table[(address>>12) & 0x3ff] = page | 7;/*填充页表中的对应项 */
/* no need for invalidate */
	return page;
}

/*
 * The previous function doesn't work very well if you also want to mark
 * the page dirty: exec.c wants this, as it has earlier changed the page,
 * and we want the dirty-status to be correct (for VM). Thus the same
 * routine, but this time we mark it dirty too.
 */
unsigned long put_dirty_page(unsigned long page, unsigned long address)
{
	unsigned long tmp, *page_table;

/* NOTE !!! This uses the fact that _pg_dir=0 */

	if (page < LOW_MEM || page >= HIGH_MEMORY)
		printk("Trying to put page %p at %p\n",page,address);
	if (mem_map[(page-LOW_MEM)>>12] != 1)
		printk("mem_map disagrees with %p at %p\n",page,address);
	page_table = (unsigned long *) ((address>>20) & 0xffc);
	if ((*page_table)&1)
		page_table = (unsigned long *) (0xfffff000 & *page_table);
	else {
		if (!(tmp=get_free_page()))
			return 0;
		*page_table = tmp|7;
		page_table = (unsigned long *) tmp;
	}
	page_table[(address>>12) & 0x3ff] = page | (PAGE_DIRTY | 7);
/* no need for invalidate */
	return page;
}

//取消写保护
void un_wp_page(unsigned long * table_entry /* 页表项 */)
{
	unsigned long old_page,new_page;

	old_page = 0xfffff000 & *table_entry;
	if (old_page >= LOW_MEM && mem_map[MAP_NR(old_page)]==1) { /* >1m 没有被共享 */
		*table_entry |= 2; /* 直接加上可写标志 */
		invalidate();
		return;
	} 
	if (!(new_page=get_free_page())) /* 共享 则分页一页 */
		oom();
	if (old_page >= LOW_MEM)
		mem_map[MAP_NR(old_page)]--;
	copy_page(old_page,new_page); /* 然后拷贝 */
	*table_entry = new_page | 7; /* 页表指向新分配的页 可读可写 */
	invalidate();
}	

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * If it's in code space we exit with a segment error.
 */

//写保护页面处理
void do_wp_page(unsigned long error_code/* 错误码 */,unsigned long address/* 线性地址 */)
{
	if (address < TASK_SIZE)
		printk("\n\rBAD! KERNEL MEMORY WP-ERR!\n\r");/* 内核空间 */
	if (address - current->start_code > TASK_SIZE) { /* 超出进程地址空间 */
		printk("Bad things happen: page error in do_wp_page\n\r");
		do_exit(SIGSEGV);
	}
#if 0
/* we cannot do this yet: the estdio library writes to code space */
/* stupid, stupid. I really want the libc.a from GNU */
	if (CODE_SPACE(address))
		do_exit(SIGSEGV);
#endif
	un_wp_page((unsigned long *)/* 调用un_wp_page 入参是线性地址对应的页表项 */
		(((address>>10) & 0xffc) /* 加上页表内偏移 */+ (0xfffff000 & /* 取出页表base地址 */
		*((unsigned long *) ((address>>20) &0xffc)/* 页目录项 */))));

}

//若设置了只读 则会分配一页 取消写保护
void write_verify(unsigned long address /* 线性地址 */)
{
	unsigned long page;
	/* 取出对应的页目录项 并确定p是否为0 */
	if (!( (page = *((unsigned long *) ((address>>20) & 0xffc)) )&1))
		return; /* p=0 直接退出  无需检查*/
	page &= 0xfffff000;/* 否则取出页表地址 */
	page += ((address>>10) & 0xffc);/* 进一步取出页表项 */
	if ((3 & *(unsigned long *) page) == 1)  /* non-writeable, present */
		un_wp_page((unsigned long *) page);/* //取消写保护 */
	return;
}

//得到一页 并映射到address对应的线性地址
void get_empty_page(unsigned long address/* 线性地址 */)
{
	unsigned long tmp;
	/*  */
	if (!(tmp=get_free_page()) || !put_page(tmp,address)) {
		free_page(tmp);		/* 0 is ok - ignored */
		oom();
	}
}

/*
 * try_to_share() checks the page at address "address" in the task "p",
 * to see if it exists, and if it is clean. If so, share it with the current
 * task.
 *
 * NOTE! This assumes we have checked that p != current, and that they
 * share the same executable or library.
 */

//尝试p中的address对应的页 是否clean，如果是则进行共享
static int try_to_share(unsigned long address/* 逻辑地址 从0-64m */, struct task_struct * p/* 任务p */)
{
	unsigned long from;
	unsigned long to;
	unsigned long from_page;
	unsigned long to_page;
	unsigned long phys_addr;

	from_page = to_page = ((address>>20) & 0xffc);/* 虚拟地址对应的页目录项 */
	from_page += ((p->start_code>>20) & 0xffc);/* p 任务的中address对应的页目录项*/
	to_page += ((current->start_code>>20) & 0xffc);/* 当前任务的address逻辑地址 对应的页目录项 */
/* is there a page-directory at from? */
	from = *(unsigned long *) from_page;/*  */
	if (!(from & 1))/* p任务address对应的页目录项 无效  则直接返回 无需share */
		return 0;
	from &= 0xfffff000;/* 取出p任务的页目录项所指向的页表的地址 */
	from_page = from + ((address>>10) & 0xffc);/* 取出p任务address对应的页表项 */
	phys_addr = *(unsigned long *) from_page;/* 取出页表内容 */
/* is the page clean and present? */
	if ((phys_addr & 0x41) != 0x01)/* 物理页干净且存在么 */
		return 0;
	phys_addr &= 0xfffff000;/* p任务address对应页面base地址 */
	if (phys_addr >= HIGH_MEMORY || phys_addr < LOW_MEM)/*  */
		return 0;
	
	to = *(unsigned long *) to_page;/* 当前进程页目录项内容 */
	if (!(to & 1))/* p=0 没有页表 */
		if (to = get_free_page())/* 申请一页当做页表 */
			*(unsigned long *) to_page = to | 7;/* 填充页表项指向新申请的页表 */
		else
			oom();/*  */
	to &= 0xfffff000;/* 页表地址 */
	to_page = to + ((address>>10) & 0xffc);/* 页表项地址 */
	if (1 & *(unsigned long *) to_page)/* 页表项内容是否有效 有效则说明当前进程address已经映射了页 panic */
		panic("try_to_share: to_page already exists");
/* share them: write-protect */
	*(unsigned long *) from_page &= ~2;/* 清除p进程的页表项的可写标记 */
	*(unsigned long *) to_page = *(unsigned long *) from_page;/* 复制p进程的页表项到当前进程，实现共享 */
	invalidate();
	phys_addr -= LOW_MEM;/*  */
	phys_addr >>= 12;/*  */
	mem_map[phys_addr]++;/* 增加引用计数 */
	return 1;
}

/*
 * share_page() tries to find a process that could share a page with
 * the current one. Address is the address of the wanted page relative
 * to the current data space.
 *
 * We first check if it is at all feasible by checking executable->i_count.
 * It should be >1 if there are other tasks sharing this inode.
 */
 //发生缺页异常时首先查看是否能与其他运行相同可执行文件的进程共享页面
static int share_page(struct m_inode * inode/* 进程可执行文件/库文件的inode? */, unsigned long address/* 要共享的逻辑地址 */)
{
	struct task_struct ** p;

	if (inode->i_count < 2 || !inode)/* 可执行文件没有其他进程使用 无法共享 直接退出 */
		return 0;
	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {/* 循环查找所有进程 */
		if (!*p)/* 跳过空项 */
			continue;
		if (current == *p)/* 跳过自己 */
			continue;
		if (address < LIBRARY_OFFSET) {/* 可执行文件的inode */
			if (inode != (*p)->executable)/*  */
				continue;
		} else {
			if (inode != (*p)->library)/* 共享库的inode */
				continue;
		}
		if (try_to_share(address,*p))/* 尝试共享页面 */
			return 1;
	}
	return 0;
}

//汇编跳转的入口  处理缺页  
void do_no_page(unsigned long error_code/*  */,unsigned long address/* 线性地址  而不是逻辑地址 */)
{
	int nr[4];
	unsigned long tmp;
	unsigned long page;
	int block,i;
	struct m_inode * inode;

	if (address < TASK_SIZE)/*内核空间  */
		printk("\n\rBAD!! KERNEL PAGE MISSING\n\r");
	if (address - current->start_code > TASK_SIZE) {/* 是否在进程地址空间内 */
		printk("Bad things happen: nonexistent page error in do_no_page\n\r");
		do_exit(SIGSEGV); /* 不在 则段错误 */
	}
	page = *(unsigned long *) ((address >> 20) & 0xffc);/* 页目录项的内容 */
	if (page & 1) {/* 页目录项有效则进入if */
		page &= 0xfffff000;/* 取出所指页表地址 */
		page += (address >> 10) & 0xffc;/* 取出页表项地址 */
		tmp = *(unsigned long *) page;/* 页表项内容 */
		if (tmp && !(1 & tmp)) {/* 页表项不为空 但p=0 代表被swap out */
			swap_in((unsigned long *) page);/* 指向swap in */
			return;
		}
	}

	//走到这里则是真正的缺页
	address &= 0xfffff000;/* 虚拟地址4k对其 */
	tmp = address - current->start_code;/*  */
	if (tmp >= LIBRARY_OFFSET ) {/* 判断虚拟地址所在范围是否是共享库 */
		inode = current->library;/* 如果是 则将inode赋值为共享库文件的inode */
		block = 1 + (tmp-LIBRARY_OFFSET) / BLOCK_SIZE;/* 计算缺页在库文件中的其实数据块号block */
	} else if (tmp < current->end_data) {/*  */
		inode = current->executable;/* 同理inode赋值为可执行文件的inode */
		block = 1 + tmp / BLOCK_SIZE;/*  */
	} else {
		inode = NULL;/* d动态申请的内存页面导致的 inode和block都置空  */
		block = 0;/*  */
	}
	if (!inode) {/* 是动态申请的 则无法共享 直接获取新页 并映射 */
		get_empty_page(address);/*  */
		return;
	}
	if (share_page(inode,tmp))/* 是执行文件或者共享库  先尝试共享 */
		return;
	if (!(page = get_free_page()))/* 共享失败则重新获取新页 */
		oom();
/* remember that 1 block is used for header */
	for (i=0 ; i<4 ; block++,i++)/* 读取4k内容到刚才申请的页 */
		nr[i] = bmap(inode,block);/*  */
	bread_page(page,inode->i_dev,nr);/*  */
	i = tmp + 4096 - current->end_data;/*  */
	if (i>4095)/*  */
		i = 0;/*  */
	tmp = page + 4096;/*  */
	while (i-- > 0) {/*  */
		tmp--;/*  */
		*(char *)tmp = 0;/*  */
	}
	if (put_page(page,address))/* 映射page到address地址 */
		return;/*  */
	free_page(page);/* 映射失败 则释放刚才申请的页 */
	oom();/*  */
}


//0.12内核没有伙伴系统
void mem_init(long start_mem, long end_mem)
{
	int i;
	//设置内存最高地址
	HIGH_MEMORY = end_mem;
	for (i=0 ; i<PAGING_PAGES ; i++) //默认分页管理的是15M,去掉内核的1M,但实际可能没有15M
	//即1M-16M
		mem_map[i] = USED;  //这15M所有的page都标记为used
	i = MAP_NR(start_mem);  //计算出主存区开始的地址对应的page num
	end_mem -= start_mem;
	end_mem >>= 12;   //计算出主存区的page 数量
	while (end_mem-->0)
		mem_map[i++]=0;  //对于主存区的page，清除used标记
		//经过这一段，除了主存区可用，其他部分都是used
}

void show_mem(void)
{
	int i,j,k,free=0,total=0;
	int shared=0;
	unsigned long * pg_tbl;

	printk("Mem-info:\n\r");
	for(i=0 ; i<PAGING_PAGES ; i++) {
		if (mem_map[i] == USED)
			continue;
		total++;
		if (!mem_map[i])
			free++;
		else
			shared += mem_map[i]-1;
	}
	printk("%d free pages of %d\n\r",free,total);
	printk("%d pages shared\n\r",shared);
	k = 0;
	for(i=4 ; i<1024 ;) {
		if (1&pg_dir[i]) {
			if (pg_dir[i]>HIGH_MEMORY) {
				printk("page directory[%d]: %08X\n\r",
					i,pg_dir[i]);
				continue;
			}
			if (pg_dir[i]>LOW_MEM)
				free++,k++;
			pg_tbl=(unsigned long *) (0xfffff000 & pg_dir[i]);
			for(j=0 ; j<1024 ; j++)
				if ((pg_tbl[j]&1) && pg_tbl[j]>LOW_MEM)
					if (pg_tbl[j]>HIGH_MEMORY)
						printk("page_dir[%d][%d]: %08X\n\r",
							i,j, pg_tbl[j]);
					else
						k++,free++;
		}
		i++;
		if (!(i&15) && k) {
			k++,free++;	/* one page/process for task_struct */
			printk("Process %d: %d pages\n\r",(i>>4)-1,k);
			k = 0;
		}
	}
	printk("Memory found: %d (%d)\n\r",free-shared,total);
}
