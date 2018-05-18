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

//�ж������ַ�Ƿ��ڴ����
#define CODE_SPACE(addr) ((((addr)+4095)&~4095) < \
current->start_code + current->end_code)

//�ڴ���ߵ�ַ
unsigned long HIGH_MEMORY = 0;

//����1ҳ��to��
#define copy_page(from,to) \
__asm__("cld ; rep ; movsl"::"S" (from),"D" (to),"c" (1024):"cx","di","si")

//����û��ҳ��״̬
unsigned char mem_map [ PAGING_PAGES ] = {0,};

/*
 * Free a page of memory at physical address 'addr'. Used by
 * 'free_page_tables()'
 */
void free_page(unsigned long addr /* ҳ�������ַ */)
{
	//����������С��1m�������ں˺�buffer����
	if (addr < LOW_MEM) return;
	// ͬ�����ܴ����ڴ�����ַ
	if (addr >= HIGH_MEMORY)
		panic("trying to free nonexistent page");
	addr -= LOW_MEM; //mem map��������1-16m�ڴ�
	addr >>= 12;//���������ַ��Ӧ��ҳ���
	//���ü�����1
	if (mem_map[addr]--) return;
	mem_map[addr]=0;
	panic("trying to free free page");
}

/*
 * This function frees a continuos block of page tables, as needed
 * by 'exit()'. As does copy_page_tables(), this handles only 4Mb blocks.
 */
 //ֻ�ܴ���4mb����ĵ�ַ����ҳĿ¼��ӳ��ķ�Χ 
int free_page_tables(unsigned long from  /* ���Ե�ַ */,unsigned long size /* �ͷŵĳ��� */)
{
	unsigned long *pg_table;
	unsigned long * dir, nr;

	if (from & 0x3fffff)
		panic("free_page_tables called with wrong alignment");
	if (!from)
		panic("Trying to free up swapper memory space");
	//sizeת�����ж��ٸ�4mb��ÿ����Ӧһ��ҳĿ¼��
	size = (size + 0x3fffff) >> 22;
	//ҳĿ¼���0��ʼ������ȡ����Ӧ��ҳĿ¼��
	dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	//ѭ������ÿ��ҳĿ¼��
	for ( ; size-->0 ; dir++) {
		//pλ=0������ӦҳĿ¼���ǲ����ڴ��еģ�ʲôҲ����
		if (!(1 & *dir))
			continue;
		pg_table = (unsigned long *) (0xfffff000 & *dir); /* ȡҳ���base��ַ */
		for (nr=0 ; nr<1024 ; nr++) { /* ����1204�� ҳ���� */
			if (*pg_table) {/* ҳ���������� ��Ϊ0 */
				if (1 & *pg_table) /* pλ=1 ����Ҫfree */
					free_page(0xfffff000 & *pg_table); /* �����ҳ�������base��ַ  �˺������ǽ�page������-- */
				else
					//��ʱҳ�����б������ʲô? ����swap������ �����Ƕ�Ӧ������ҳ���base��ַ
					swap_free(*pg_table >> 1); /* ҳ���Ϊ0������p=0����ҳ���ܱ�swap ��Ҫ��swap��free */
				*pg_table = 0; /* free������ҳ���� */
			}
			pg_table++; /* ������һ�� */
		}
		free_page(0xfffff000 & *dir); /* �ͷ�ҳ�������һҳ�ڴ� */
		*dir = 0; /* ���ҳĿ¼�� */
	}
	invalidate(); /* ˢ��cpuҳ�任���ٻ����� */
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
//����ҳĿ¼���ҳ����
int copy_page_tables(unsigned long from /* ���Ե�ַ */,unsigned long to/* ���Ե�ַ */,long size /* ���� */)
{
	unsigned long * from_page_table;
	unsigned long * to_page_table;
	unsigned long this_page;
	unsigned long * from_dir, * to_dir;
	unsigned long new_page;
	unsigned long nr;
	//from��to��������4mb��ַ����
	if ((from&0x3fffff) || (to&0x3fffff))
		panic("copy_page_tables called with wrong alignment");
	//Դ���Ե�ַ��Ӧ��ҳĿ¼��
	from_dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	//Ŀ�����Ե�ַ��Ӧ��ҳĿ¼��
	to_dir = (unsigned long *) ((to>>20) & 0xffc);
	//sizeͬ��ȡ4mb������  �� 8mb->2
	size = ((unsigned) (size+0x3fffff)) >> 22;
	//ѭ������ÿ��4mb
	for( ; size-->0 ; from_dir++,to_dir++) {
		if (1 & *to_dir) /* Ŀ��ҳĿ¼��p=1�����Ѿ����ڣ�panic */
			panic("copy_page_tables: already exist");
		if (!(1 & *from_dir)) /* ԴҳĿ¼�� p=0 �������ã� continue */
			continue;
		//ȡ��ԴҳĿ¼��ָ���ҳ�����ַ
		from_page_table = (unsigned long *) (0xfffff000 & *from_dir);
		if (!(to_page_table = (unsigned long *) get_free_page())) /* ����һҳ��Ŀ��ҳ�� */
			return -1;	/* Out of memory, see freeing */
		*to_dir = ((unsigned long) to_page_table) | 7; /*���Ŀ��ҳĿ¼� ��ʾ�ڴ�ҳ��ɶ���д�����ڣ� �û��� */
		nr = (from==0)?0xA0:1024; /* �ں˿ռ�ֻҪ����160�� */
		for ( ; nr-- > 0 ; from_page_table++,to_page_table++) { /* �������ҳ���� */
			this_page = *from_page_table;
			if (!this_page) /* Դҳ����� ���� */
				continue;
			if (!(1 & this_page)) { /* Դҳ��������ҳ��swap��ȥ */
				if (!(new_page = get_free_page())) /* �����һ��ҳ ����swap�����������·����ҳ */
					return -1;
				read_swap_page(this_page>>1, (char *) new_page);
				*to_page_table = this_page; /* Ŀ��ҳ�����ֵ����ΪԴҳ�������ݣ���Ŀ��ҳ�����Ӧ��ҳ��ʱ��swap�� */
				*from_page_table = new_page | (PAGE_DIRTY | 7);/* Դҳ����ָ���·����ҳ ������Ϊdirty ����Դҳ�����Ӧ��ҳ��ʱ�Ѿ����ڴ���*/
				//����ʱû�й���ҳ�� ��Ϊɶ?
				continue;
			}
			this_page &= ~2;/* ��Դҳ�����Ӧ���ڴ�ҳֻ�� */
			*to_page_table = this_page;/* Ŀ��ҳ�����ֵ����ΪԴҳ�������ݣ���ʱĿ��ҳ������ֻ�� */
			if (this_page > LOW_MEM) {/*  */
				*from_page_table = this_page;/* ��������Դҳ���� ��ʱԴҳ����Ҳ���ֻ�� */
				this_page -= LOW_MEM;/*  */
				this_page >>= 12;/*  */
				mem_map[this_page]++;/* ����ֻ�������ҳ�ļ��� */
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
 /* ��һҳӳ�䵽ָ���������ַ */
static unsigned long put_page(unsigned long page /* ҳ�������ַ */,unsigned long address /* Ҫӳ���Ŀ�������ַ */)
{
	unsigned long tmp, *page_table;

/* NOTE !!! This uses the fact that _pg_dir=0 */

	if (page < LOW_MEM || page >= HIGH_MEMORY) /* ���ҳ���ַ��Χ */
		printk("Trying to put page %p at %p\n",page,address);
	if (mem_map[(page-LOW_MEM)>>12] != 1) /* Ҫӳ���ҳû������? */
		printk("mem_map disagrees with %p at %p\n",page,address);
	page_table = (unsigned long *) ((address>>20) & 0xffc); /* ���������ַ��Ӧ��ҳĿ¼�� */
	if ((*page_table)&1)
		//���ҳĿ¼����ڣ���ȡ����ָ���ҳ���ַ
		page_table = (unsigned long *) (0xfffff000 & *page_table);
	else {
		//����Ҫ����һҳ������ҳĿ¼��ָ���������ҳ��
		if (!(tmp=get_free_page()))
			return 0;
		*page_table = tmp | 7;
		page_table = (unsigned long *) tmp;/*��ָ���������ҳ�� */
	}
	page_table[(address>>12) & 0x3ff] = page | 7;/*���ҳ���еĶ�Ӧ�� */
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

//ȡ��д����
void un_wp_page(unsigned long * table_entry /* ҳ���� */)
{
	unsigned long old_page,new_page;

	old_page = 0xfffff000 & *table_entry;
	if (old_page >= LOW_MEM && mem_map[MAP_NR(old_page)]==1) { /* >1m û�б����� */
		*table_entry |= 2; /* ֱ�Ӽ��Ͽ�д��־ */
		invalidate();
		return;
	} 
	if (!(new_page=get_free_page())) /* ���� ���ҳһҳ */
		oom();
	if (old_page >= LOW_MEM)
		mem_map[MAP_NR(old_page)]--;
	copy_page(old_page,new_page); /* Ȼ�󿽱� */
	*table_entry = new_page | 7; /* ҳ��ָ���·����ҳ �ɶ���д */
	invalidate();
}	

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * If it's in code space we exit with a segment error.
 */

//д����ҳ�洦��
void do_wp_page(unsigned long error_code/* ������ */,unsigned long address/* ���Ե�ַ */)
{
	if (address < TASK_SIZE)
		printk("\n\rBAD! KERNEL MEMORY WP-ERR!\n\r");/* �ں˿ռ� */
	if (address - current->start_code > TASK_SIZE) { /* �������̵�ַ�ռ� */
		printk("Bad things happen: page error in do_wp_page\n\r");
		do_exit(SIGSEGV);
	}
#if 0
/* we cannot do this yet: the estdio library writes to code space */
/* stupid, stupid. I really want the libc.a from GNU */
	if (CODE_SPACE(address))
		do_exit(SIGSEGV);
#endif
	un_wp_page((unsigned long *)/* ����un_wp_page ��������Ե�ַ��Ӧ��ҳ���� */
		(((address>>10) & 0xffc) /* ����ҳ����ƫ�� */+ (0xfffff000 & /* ȡ��ҳ��base��ַ */
		*((unsigned long *) ((address>>20) &0xffc)/* ҳĿ¼�� */))));

}

//��������ֻ�� ������һҳ ȡ��д����
void write_verify(unsigned long address /* ���Ե�ַ */)
{
	unsigned long page;
	/* ȡ����Ӧ��ҳĿ¼�� ��ȷ��p�Ƿ�Ϊ0 */
	if (!( (page = *((unsigned long *) ((address>>20) & 0xffc)) )&1))
		return; /* p=0 ֱ���˳�  ������*/
	page &= 0xfffff000;/* ����ȡ��ҳ���ַ */
	page += ((address>>10) & 0xffc);/* ��һ��ȡ��ҳ���� */
	if ((3 & *(unsigned long *) page) == 1)  /* non-writeable, present */
		un_wp_page((unsigned long *) page);/* //ȡ��д���� */
	return;
}

//�õ�һҳ ��ӳ�䵽address��Ӧ�����Ե�ַ
void get_empty_page(unsigned long address/* ���Ե�ַ */)
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

//����p�е�address��Ӧ��ҳ �Ƿ�clean�����������й���
static int try_to_share(unsigned long address/* �߼���ַ ��0-64m */, struct task_struct * p/* ����p */)
{
	unsigned long from;
	unsigned long to;
	unsigned long from_page;
	unsigned long to_page;
	unsigned long phys_addr;

	from_page = to_page = ((address>>20) & 0xffc);/* �����ַ��Ӧ��ҳĿ¼�� */
	from_page += ((p->start_code>>20) & 0xffc);/* p �������address��Ӧ��ҳĿ¼��*/
	to_page += ((current->start_code>>20) & 0xffc);/* ��ǰ�����address�߼���ַ ��Ӧ��ҳĿ¼�� */
/* is there a page-directory at from? */
	from = *(unsigned long *) from_page;/*  */
	if (!(from & 1))/* p����address��Ӧ��ҳĿ¼�� ��Ч  ��ֱ�ӷ��� ����share */
		return 0;
	from &= 0xfffff000;/* ȡ��p�����ҳĿ¼����ָ���ҳ��ĵ�ַ */
	from_page = from + ((address>>10) & 0xffc);/* ȡ��p����address��Ӧ��ҳ���� */
	phys_addr = *(unsigned long *) from_page;/* ȡ��ҳ������ */
/* is the page clean and present? */
	if ((phys_addr & 0x41) != 0x01)/* ����ҳ�ɾ��Ҵ���ô */
		return 0;
	phys_addr &= 0xfffff000;/* p����address��Ӧҳ��base��ַ */
	if (phys_addr >= HIGH_MEMORY || phys_addr < LOW_MEM)/*  */
		return 0;
	
	to = *(unsigned long *) to_page;/* ��ǰ����ҳĿ¼������ */
	if (!(to & 1))/* p=0 û��ҳ�� */
		if (to = get_free_page())/* ����һҳ����ҳ�� */
			*(unsigned long *) to_page = to | 7;/* ���ҳ����ָ���������ҳ�� */
		else
			oom();/*  */
	to &= 0xfffff000;/* ҳ���ַ */
	to_page = to + ((address>>10) & 0xffc);/* ҳ�����ַ */
	if (1 & *(unsigned long *) to_page)/* ҳ���������Ƿ���Ч ��Ч��˵����ǰ����address�Ѿ�ӳ����ҳ panic */
		panic("try_to_share: to_page already exists");
/* share them: write-protect */
	*(unsigned long *) from_page &= ~2;/* ���p���̵�ҳ����Ŀ�д��� */
	*(unsigned long *) to_page = *(unsigned long *) from_page;/* ����p���̵�ҳ�����ǰ���̣�ʵ�ֹ��� */
	invalidate();
	phys_addr -= LOW_MEM;/*  */
	phys_addr >>= 12;/*  */
	mem_map[phys_addr]++;/* �������ü��� */
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
 //����ȱҳ�쳣ʱ���Ȳ鿴�Ƿ���������������ͬ��ִ���ļ��Ľ��̹���ҳ��
static int share_page(struct m_inode * inode/* ���̿�ִ���ļ�/���ļ���inode? */, unsigned long address/* Ҫ������߼���ַ */)
{
	struct task_struct ** p;

	if (inode->i_count < 2 || !inode)/* ��ִ���ļ�û����������ʹ�� �޷����� ֱ���˳� */
		return 0;
	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {/* ѭ���������н��� */
		if (!*p)/* �������� */
			continue;
		if (current == *p)/* �����Լ� */
			continue;
		if (address < LIBRARY_OFFSET) {/* ��ִ���ļ���inode */
			if (inode != (*p)->executable)/*  */
				continue;
		} else {
			if (inode != (*p)->library)/* ������inode */
				continue;
		}
		if (try_to_share(address,*p))/* ���Թ���ҳ�� */
			return 1;
	}
	return 0;
}

//�����ת�����  ����ȱҳ  
void do_no_page(unsigned long error_code/*  */,unsigned long address/* ���Ե�ַ  �������߼���ַ */)
{
	int nr[4];
	unsigned long tmp;
	unsigned long page;
	int block,i;
	struct m_inode * inode;

	if (address < TASK_SIZE)/*�ں˿ռ�  */
		printk("\n\rBAD!! KERNEL PAGE MISSING\n\r");
	if (address - current->start_code > TASK_SIZE) {/* �Ƿ��ڽ��̵�ַ�ռ��� */
		printk("Bad things happen: nonexistent page error in do_no_page\n\r");
		do_exit(SIGSEGV); /* ���� ��δ��� */
	}
	page = *(unsigned long *) ((address >> 20) & 0xffc);/* ҳĿ¼������� */
	if (page & 1) {/* ҳĿ¼����Ч�����if */
		page &= 0xfffff000;/* ȡ����ָҳ���ַ */
		page += (address >> 10) & 0xffc;/* ȡ��ҳ�����ַ */
		tmp = *(unsigned long *) page;/* ҳ�������� */
		if (tmp && !(1 & tmp)) {/* ҳ���Ϊ�� ��p=0 ����swap out */
			swap_in((unsigned long *) page);/* ָ��swap in */
			return;
		}
	}

	//�ߵ���������������ȱҳ
	address &= 0xfffff000;/* �����ַ4k���� */
	tmp = address - current->start_code;/*  */
	if (tmp >= LIBRARY_OFFSET ) {/* �ж������ַ���ڷ�Χ�Ƿ��ǹ���� */
		inode = current->library;/* ����� ��inode��ֵΪ������ļ���inode */
		block = 1 + (tmp-LIBRARY_OFFSET) / BLOCK_SIZE;/* ����ȱҳ�ڿ��ļ��е���ʵ���ݿ��block */
	} else if (tmp < current->end_data) {/*  */
		inode = current->executable;/* ͬ��inode��ֵΪ��ִ���ļ���inode */
		block = 1 + tmp / BLOCK_SIZE;/*  */
	} else {
		inode = NULL;/* d��̬������ڴ�ҳ�浼�µ� inode��block���ÿ�  */
		block = 0;/*  */
	}
	if (!inode) {/* �Ƕ�̬����� ���޷����� ֱ�ӻ�ȡ��ҳ ��ӳ�� */
		get_empty_page(address);/*  */
		return;
	}
	if (share_page(inode,tmp))/* ��ִ���ļ����߹����  �ȳ��Թ��� */
		return;
	if (!(page = get_free_page()))/* ����ʧ�������»�ȡ��ҳ */
		oom();
/* remember that 1 block is used for header */
	for (i=0 ; i<4 ; block++,i++)/* ��ȡ4k���ݵ��ղ������ҳ */
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
	if (put_page(page,address))/* ӳ��page��address��ַ */
		return;/*  */
	free_page(page);/* ӳ��ʧ�� ���ͷŸղ������ҳ */
	oom();/*  */
}


//0.12�ں�û�л��ϵͳ
void mem_init(long start_mem, long end_mem)
{
	int i;
	//�����ڴ���ߵ�ַ
	HIGH_MEMORY = end_mem;
	for (i=0 ; i<PAGING_PAGES ; i++) //Ĭ�Ϸ�ҳ�������15M,ȥ���ں˵�1M,��ʵ�ʿ���û��15M
	//��1M-16M
		mem_map[i] = USED;  //��15M���е�page�����Ϊused
	i = MAP_NR(start_mem);  //�������������ʼ�ĵ�ַ��Ӧ��page num
	end_mem -= start_mem;
	end_mem >>= 12;   //�������������page ����
	while (end_mem-->0)
		mem_map[i++]=0;  //������������page�����used���
		//������һ�Σ��������������ã��������ֶ���used
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
