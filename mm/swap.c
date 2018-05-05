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

bitop(bit,"")  /* ���Բ���ԭֵ���ý�λλ */
bitop(setbit,"s") /* ���ö�Ӧbit ����ԭֵ���ý�λλ */
bitop(clrbit,"r") /* ��λ��Ӧbit ����ԭֵ���ý�λλ */

static char * swap_bitmap = NULL;
int SWAP_DEV = 0;

/*
 * We never page the pages in task[0] - kernel memory.
 * We page all other pages.
 */
#define FIRST_VM_PAGE (TASK_SIZE>>12)  /* 64M��ַ��Ӧ��vm page num*/
#define LAST_VM_PAGE (1024*1024)/* 4gb��ַ��Ӧ��vm page num */
#define VM_PAGES (LAST_VM_PAGE - FIRST_VM_PAGE)/* �ܹ����ٸ�vm page */

/* ����һҳ����ҳ�� ������swap bitmap��bit index*/
static int get_swap_page(void)
{
	int nr;

	if (!swap_bitmap) /* bitmap=null ��֧��swapֱ���˳� */
		return 0;
	for (nr = 1; nr < 32768 ; nr++)
		if (clrbit(swap_bitmap,nr)) /* �ҵ���һ��bit=1��λ ������пռ� 4k*/
			return nr;
	return 0;
}

/* �ͷ�swap�豸��ָ����4k�ռ�  ����ҳ�� */
void swap_free(int swap_nr)
{
	if (!swap_nr)
		return;
	// 
	if (swap_bitmap && swap_nr < SWAP_BITS)
		if (!setbit(swap_bitmap,swap_nr)) /* ��Ӧ��bit ��1 Ӧ�÷���ԭֵ ��0(��ʹ��) �����ͷŵ��Ǳ������ǿ��еĽ���ҳ�� ���� */
			return;
	printk("Swap-space bad (swap_free())\n\r");
	return;
}

/* ָ������ҳ��  �������ڴ��� */
// ��ָ��ҳ�����Ӧ��ҳ��ӽ����豸�ж�ȡ�����������ҳ�� �޸Ķ�Ӧ��bitmap=1(����Ϊ����) 
// ͬʱ����ҳ����ָ��ղ����������ҳ��
void swap_in(unsigned long *table_ptr)
{
	int swap_nr;
	unsigned long page;

	if (!swap_bitmap) {
		printk("Trying to swap in without swap bit-map");
		return;
	}
	if (1 & *table_ptr) { /* ҳ����ָ���ҳ�Ǵ������ڴ��е�  ֱ�ӷ��� */
		printk("trying to swap in present page\n\r");
		return;
	}
	swap_nr = *table_ptr >> 1;/* ��ҳ���ֶ�Ӧ��ҳ ��swap outʱ��ҳ����洢�˶�Ӧ��swap_nr*2����Ӧ��bitmap��λ ����ȡ����Ӧ��bit �Ա��ҵ�����ҳ�� */
	if (!swap_nr) {/* nr=0 û�ҵ���Ӧ�Ľ���ҳ�� ���� */
		printk("No swap page in swap_in\n\r");
		return;
	}
	if (!(page = get_free_page()))/* ����һҳ */
		oom();/* ���벻�� oom */
	read_swap_page(swap_nr, (char *) page);/* ����Ӧ�Ľ���ҳ���ȡ���ղ������ҳ���� */
	if (setbit(swap_bitmap,swap_nr))/* ���ö�Ӧ��bit=1 ����Ӧ�Ľ���ҳ�����ڿ����� */
		printk("swapping in multiply from same page\n\r");
	*table_ptr = page | (PAGE_DIRTY | 7);/* ��������ҳ����ָ���������ҳ�� */
}

/* ���԰�ҳ�����Ӧ��ҳ������ȥ */
int try_to_swap_out(unsigned long * table_ptr)
{
	unsigned long page;
	unsigned long swap_nr;

	page = *table_ptr;
	if (!(PAGE_PRESENT & page)) /* ��Ӧҳ�������ڴ��� ���� */
		return 0;
	if (page - LOW_MEM > PAGING_MEMORY)/* ����15M ���� */
		return 0;
	if (PAGE_DIRTY & page) {/* ���ҳ���Ǳ��޸Ĺ��� */
		page &= 0xfffff000;
		if (mem_map[MAP_NR(page)] != 1)/* ���Ǳ������ ����swapout */
			return 0;
		if (!(swap_nr = get_swap_page()))/* ���ǹ���� ������һҳ����ҳ�� */
			return 0;
		*table_ptr = swap_nr<<1;/* ��swap_nr*2 �洢��ҳ������ */
		invalidate();/* ˢ��tlb */
		write_swap_page(swap_nr, (char *) page);/* ��Ӧҳд����Ӧ����ҳ�� */
		free_page(page); /* �ͷ��Ѿ�swap out��ҳ */
		return 1;
	}
	*table_ptr = 0;/* �ߵ�������ҳ��ûӴ���޸Ĺ�  ��ֱ���ͷ� ����Ҫswap  ���������ָû��д������ݵ�ҳ? */
	invalidate();
	free_page(page);/*  */
	return 1;
}

/*
 * Ok, this has a rather intricate logic - the idea is to make good
 * and fast machine code. If we didn't worry about that, things would
 * be easier.
 */
 /* ��ҳ��ŵ������豸�� ��64m��Ӧ��ҳĿ¼�ʼ���� ����Чҳ����ָ���ҳ�波�Խ�����ȥ һ���ɹ�����һ�� �򷵻�1 ���򷵻�0 */
//get_free_page�л����
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
 /* ��mem map���ҵ�һ������ҳ  û���ҵ��᳢��swap out */
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

// �ڴ潻����ʼ��
void init_swapping(void)
{
	/* ÿһ���һ��ָ�������ָ�룬blk_size[MAJOR][MINOR] ��ָ���������minor��������豸����ӵ�е����ݿ����� 1kbһ�� */
	extern int *blk_size[];
	int swap_size,i,j;
	/* û��ָ��swap�豸 ֱ�ӷ��� */
	if (!SWAP_DEV)
		return;
	/* �����豸û������ ��������  ���� */
	if (!blk_size[MAJOR(SWAP_DEV)]) {
		printk("Unable to get size of swap device\n\r");
		return;
	}
	/* �õ������豸�Ľ����������ݿ����� */
	swap_size = blk_size[MAJOR(SWAP_DEV)][MINOR(SWAP_DEV)];
	if (!swap_size)/* �����0 ���� */
		return;
	if (swap_size < 100) {/* С��100 ̫С ���� */
		printk("Swap device too small (%d blocks)\n\r",swap_size);
		return;
	}
	/* ת����ҳ����� 4k */
	swap_size >>= 2;
	if (swap_size > SWAP_BITS)/* ���SWAP_BITS */
		swap_size = SWAP_BITS;
	swap_bitmap = (char *) get_free_page();/* ����һҳ��Ϊbitmap ����swapҳ�� */
	if (!swap_bitmap) {/*  */
		printk("Unable to start swapping: out of memory :-)\n\r");
		return;
	}
	read_swap_page(0,swap_bitmap);/* �ѽ����豸��0ҳ���ȡ���� ���汣�����bitmap 4086�ֽڿ�ʼ������������ַ��� */
	if (strncmp("SWAP-SPACE",swap_bitmap+4086,10)) {/*  */
		printk("Unable to find swap-space signature\n\r");
		free_page((long) swap_bitmap);/* û���ҵ������ַ��� �ͷ������ҳ������ */
		swap_bitmap = NULL;/*  */
		return;
	}
	memset(swap_bitmap+4086,0,10)/* ���10byte ��0 */;
	for (i = 0 ; i < SWAP_BITS ; i++) {/* */
		if (i == 1)
			i = swap_size;/*  */
		if (bit(swap_bitmap,i)) {/* λ0�����һλӦ��Ϊ0��bit0��Ӧ��ҳ��洢��bitmap */
			printk("Bad swap-space bit-map\n\r");
			free_page((long) swap_bitmap);/* �ͷ�ҳ�� �˳� */
			swap_bitmap = NULL;/*  */
			return;
		}
	}
	j = 0;
	for (i = 1 ; i < swap_size ; i++)
		if (bit(swap_bitmap,i))/* ʣ�µ�bit Ӧ��Ϊ1 ������� */
			j++;/*  */
	if (!j) {/* j=0 ����ô�п��е�swap �ռ�  �����˳� */
		free_page((long) swap_bitmap);/*  */
		swap_bitmap = NULL;/*  */
		return;
	}
	printk("Swap device ok: %d pages (%d bytes) swap-space\n\r",j,j*4096);
}
