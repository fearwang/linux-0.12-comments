/*
 *  linux/mm/swap.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * This file should contain most things doing the swapping from/to disk.
 * Started 18.12.91
 */

#include <string.h>

#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>

#define SWAP_BITS (4096<<3)

#define bitop(name,op) \
static inline int name(char * addr,unsigned int nr) \
{ \
int __res; \
__asm__ __volatile__("bt" op " %1,%2; adcl $0,%0" \
:"=g" (__res) \
:"r" (nr),"m" (*(addr)),"0" (0)); \
return __res; \
}

bitop(bit,"")  /* 测试并用原值设置进位位 */
bitop(setbit,"s") /* 设置对应bit 并用原值设置进位位 */
bitop(clrbit,"r") /* 复位对应bit 并用原值设置进位位 */

static char * swap_bitmap = NULL;
int SWAP_DEV = 0;

/*
 * We never page the pages in task[0] - kernel memory.
 * We page all other pages.
 */
#define FIRST_VM_PAGE (TASK_SIZE>>12)  /* 64M地址对应的vm page num*/
#define LAST_VM_PAGE (1024*1024)/* 4gb地址对应的vm page num */
#define VM_PAGES (LAST_VM_PAGE - FIRST_VM_PAGE)/* 总共多少个vm page */

/* 申请一页交换页面 返回在swap bitmap的bit index*/
static int get_swap_page(void)
{
	int nr;

	if (!swap_bitmap) /* bitmap=null 不支持swap直接退出 */
		return 0;
	for (nr = 1; nr < 32768 ; nr++)
		if (clrbit(swap_bitmap,nr)) /* 找到第一个bit=1的位 代表空闲空间 4k*/
			return nr;
	return 0;
}

/* 释放swap设备中指定的4k空间  交换页面 */
void swap_free(int swap_nr)
{
	if (!swap_nr)
		return;
	// 
	if (swap_bitmap && swap_nr < SWAP_BITS)
		if (!setbit(swap_bitmap,swap_nr)) /* 对应的bit 置1 应该返回原值 即0(被使用) 否则释放的是本来就是空闲的交换页面 错误 */
			return;
	printk("Swap-space bad (swap_free())\n\r");
	return;
}

/* 指定交换页面  交换进内存中 */
// 将指定页表项对应的页面从交换设备中读取进重新申请的页面 修改对应的bitmap=1(重置为空闲) 
// 同时设置页表项指向刚才重新申请的页面
void swap_in(unsigned long *table_ptr)
{
	int swap_nr;
	unsigned long page;

	if (!swap_bitmap) {
		printk("Trying to swap in without swap bit-map");
		return;
	}
	if (1 & *table_ptr) { /* 页表项指向的页是存在于内存中的  直接返回 */
		printk("trying to swap in present page\n\r");
		return;
	}
	swap_nr = *table_ptr >> 1;/* 当页表现对应的页 被swap out时，页表项存储了对应的swap_nr*2即对应的bitmap的位 这里取出对应的bit 以便找到交换页面 */
	if (!swap_nr) {/* nr=0 没找到对应的交换页面 返回 */
		printk("No swap page in swap_in\n\r");
		return;
	}
	if (!(page = get_free_page()))/* 申请一页 */
		oom();/* 申请不到 oom */
	read_swap_page(swap_nr, (char *) page);/* 将对应的交换页面读取到刚才申请的页面中 */
	if (setbit(swap_bitmap,swap_nr))/* 设置对应的bit=1 即对应的交换页面现在空闲了 */
		printk("swapping in multiply from same page\n\r");
	*table_ptr = page | (PAGE_DIRTY | 7);/* 重新设置页表项指向新申请的页面 */
}

/* 尝试吧页表项对应的页交换出去 */
int try_to_swap_out(unsigned long * table_ptr)
{
	unsigned long page;
	unsigned long swap_nr;

	page = *table_ptr;
	if (!(PAGE_PRESENT & page)) /* 对应页不存在内存中 返回 */
		return 0;
	if (page - LOW_MEM > PAGING_MEMORY)/* 超出15M 返回 */
		return 0;
	if (PAGE_DIRTY & page) {/* 如果页面是被修改过的 */
		page &= 0xfffff000;
		if (mem_map[MAP_NR(page)] != 1)/* 且是被共享的 则不宜swapout */
			return 0;
		if (!(swap_nr = get_swap_page()))/* 不是共享的 则申请一页交换页面 */
			return 0;
		*table_ptr = swap_nr<<1;/* 将swap_nr*2 存储在页表项中 */
		invalidate();/* 刷新tlb */
		write_swap_page(swap_nr, (char *) page);/* 对应页写到对应交换页中 */
		free_page(page); /* 释放已经swap out的页 */
		return 1;
	}
	*table_ptr = 0;/* 走到这里则页面没哟被修改过  则直接释放 不需要swap  因此这里是指没有写入过数据的页? */
	invalidate();
	free_page(page);/*  */
	return 1;
}

/*
 * Ok, this has a rather intricate logic - the idea is to make good
 * and fast machine code. If we didn't worry about that, things would
 * be easier.
 */
 /* 把页面放到交换设备中 从64m对应的页目录项开始搜索 对有效页表项指向的页面尝试交换出去 一旦成功交换一个 则返回1 否则返回0 */
//get_free_page中会调用
int swap_out(void)
{
	static int dir_entry = FIRST_VM_PAGE>>10;
	static int page_entry = -1;
	int counter = VM_PAGES;
	int pg_table;

	while (counter>0) {
		pg_table = pg_dir[dir_entry];
		if (pg_table & 1)
			break;
		counter -= 1024;
		dir_entry++;
		if (dir_entry >= 1024)
			dir_entry = FIRST_VM_PAGE>>10;
	}
	pg_table &= 0xfffff000;
	while (counter-- > 0) {
		page_entry++;
		if (page_entry >= 1024) {
			page_entry = 0;
		repeat:
			dir_entry++;
			if (dir_entry >= 1024)
				dir_entry = FIRST_VM_PAGE>>10;
			pg_table = pg_dir[dir_entry];
			if (!(pg_table&1))
				if ((counter -= 1024) > 0)
					goto repeat;
				else
					break;
			pg_table &= 0xfffff000;
		}
		if (try_to_swap_out(page_entry + (unsigned long *) pg_table))
			return 1;
	}
	printk("Out of swap-memory\n\r");
	return 0;
}

/*
 * Get physical address of first (actually last :-) free page, and mark it
 * used. If no free pages left, return 0.
 */
 /* 从mem map中找到一个空闲页  没有找到会尝试swap out */
unsigned long get_free_page(void)
{
register unsigned long __res asm("ax");

repeat:
	__asm__("std ; repne ; scasb\n\t"
		"jne 1f\n\t"
		"movb $1,1(%%edi)\n\t"
		"sall $12,%%ecx\n\t"
		"addl %2,%%ecx\n\t"
		"movl %%ecx,%%edx\n\t"
		"movl $1024,%%ecx\n\t"
		"leal 4092(%%edx),%%edi\n\t"
		"rep ; stosl\n\t"
		"movl %%edx,%%eax\n"
		"1:"
		:"=a" (__res)
		:"0" (0),"i" (LOW_MEM),"c" (PAGING_PAGES),
		"D" (mem_map+PAGING_PAGES-1)
		:"di","cx","dx");
	if (__res >= HIGH_MEMORY)
		goto repeat;
	if (!__res && swap_out())
		goto repeat;
	return __res;
}

// 内存交换初始化
void init_swapping(void)
{
	/* 每一项都是一个指向数组的指针，blk_size[MAJOR][MINOR] 所指向的数组是minor代表的子设备上所拥有的数据块总数 1kb一块 */
	extern int *blk_size[];
	int swap_size,i,j;
	/* 没有指定swap设备 直接返回 */
	if (!SWAP_DEV)
		return;
	/* 交换设备没有设置 块数数组  返回 */
	if (!blk_size[MAJOR(SWAP_DEV)]) {
		printk("Unable to get size of swap device\n\r");
		return;
	}
	/* 得到交换设备的交换区的数据块总数 */
	swap_size = blk_size[MAJOR(SWAP_DEV)][MINOR(SWAP_DEV)];
	if (!swap_size)/* 如果是0 返回 */
		return;
	if (swap_size < 100) {/* 小于100 太小 返回 */
		printk("Swap device too small (%d blocks)\n\r",swap_size);
		return;
	}
	/* 转换成页面个数 4k */
	swap_size >>= 2;
	if (swap_size > SWAP_BITS)/* 最大SWAP_BITS */
		swap_size = SWAP_BITS;
	swap_bitmap = (char *) get_free_page();/* 申请一页作为bitmap 管理swap页面 */
	if (!swap_bitmap) {/*  */
		printk("Unable to start swapping: out of memory :-)\n\r");
		return;
	}
	read_swap_page(0,swap_bitmap);/* 把交换设备的0页面读取进来 里面保存的是bitmap 4086字节开始保存的是特殊字符串 */
	if (strncmp("SWAP-SPACE",swap_bitmap+4086,10)) {/*  */
		printk("Unable to find swap-space signature\n\r");
		free_page((long) swap_bitmap);/* 没有找到特殊字符串 释放申请的页并返回 */
		swap_bitmap = NULL;/*  */
		return;
	}
	memset(swap_bitmap+4086,0,10)/* 最后10byte 清0 */;
	for (i = 0 ; i < SWAP_BITS ; i++) {/* */
		if (i == 1)
			i = swap_size;/*  */
		if (bit(swap_bitmap,i)) {/* 位0和最后一位应该为0，bit0对应的页面存储了bitmap */
			printk("Bad swap-space bit-map\n\r");
			free_page((long) swap_bitmap);/* 释放页面 退出 */
			swap_bitmap = NULL;/*  */
			return;
		}
	}
	j = 0;
	for (i = 1 ; i < swap_size ; i++)
		if (bit(swap_bitmap,i))/* 剩下的bit 应该为1 代表空闲 */
			j++;/*  */
	if (!j) {/* j=0 代表么有空闲的swap 空间  出错退出 */
		free_page((long) swap_bitmap);/*  */
		swap_bitmap = NULL;/*  */
		return;
	}
	printk("Swap device ok: %d pages (%d bytes) swap-space\n\r",j,j*4096);
}
