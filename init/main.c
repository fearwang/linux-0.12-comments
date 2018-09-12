/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>
#include <time.h>

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */
 //以下inline避免idle进程使用堆栈，直到init执行execv 他们一开始共享
 //用户态堆栈
static inline _syscall0(int,fork)
static inline _syscall0(int,pause)
static inline _syscall1(int,setup,void *,BIOS)
static inline _syscall0(int,sync)

#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <asm/system.h>
#include <asm/io.h>

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <linux/fs.h>

#include <string.h>

//printf使用的buffer
static char printbuf[1024];

extern char *strcpy();
extern int vsprintf();
extern void init(void);
extern void blk_dev_init(void);
extern void chr_dev_init(void);
extern void hd_init(void);
extern void floppy_init(void);
extern void mem_init(long start, long end);
extern long rd_init(long mem_start, int length);
extern long kernel_mktime(struct tm * tm);

//格式化输出到buffer
static int sprintf(char * str, const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	i = vsprintf(str, fmt, args);
	va_end(args);
	return i;
}

/*
 * This is set up by the setup-routine at boot-time
 */
 //以下地址都是setup填充的系统参数 这里直接强制类型转换
#define EXT_MEM_K (*(unsigned short *)0x90002) //1mb以后的扩展内存大小
#define CON_ROWS ((*(unsigned short *)0x9000e) & 0xff) 
#define CON_COLS (((*(unsigned short *)0x9000e) & 0xff00) >> 8)
#define DRIVE_INFO (*(struct drive_info *)0x90080) //硬盘参数表
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC)//根设备号
#define ORIG_SWAP_DEV (*(unsigned short *)0x901FA)

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */
//读取cmos时钟信息
#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)

static void time_init(void)
{
	struct tm time;

	do {
		time.tm_sec = CMOS_READ(0);
		time.tm_min = CMOS_READ(2);
		time.tm_hour = CMOS_READ(4);
		time.tm_mday = CMOS_READ(7);
		time.tm_mon = CMOS_READ(8);
		time.tm_year = CMOS_READ(9);
	} while (time.tm_sec != CMOS_READ(0));
	BCD_TO_BIN(time.tm_sec);
	BCD_TO_BIN(time.tm_min);
	BCD_TO_BIN(time.tm_hour);
	BCD_TO_BIN(time.tm_mday);
	BCD_TO_BIN(time.tm_mon);
	BCD_TO_BIN(time.tm_year);
	time.tm_mon--;
	startup_time = kernel_mktime(&time); //从cmos读取开机时间并设置到startup_time，转换成1970.1.1开始的秒
}

static long memory_end = 0; //物理地址结束
static long buffer_memory_end = 0; //高速缓存结束地址
static long main_memory_start = 0; //主存开始地址，即buddy管理的部分
static char term[32]; //终端设置字符串

static char * argv_rc[] = { "/bin/sh", NULL };
static char * envp_rc[] = { "HOME=/", NULL ,NULL };

static char * argv[] = { "-/bin/sh",NULL };
static char * envp[] = { "HOME=/usr/root", NULL, NULL };

struct drive_info { char dummy[32]; } drive_info; //用于存放硬盘参数


//该函数最后以idle进程的身份运行
void main(void)		/* This really IS void, no error here. */
{			/* The startup routine assumes (well, ...) this */
/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */
 	ROOT_DEV = ORIG_ROOT_DEV;
 	SWAP_DEV = ORIG_SWAP_DEV;
	sprintf(term, "TERM=con%dx%d", CON_COLS, CON_ROWS); //打印控制台长宽字符数
	envp[1] = term;	
	envp_rc[1] = term;
 	drive_info = DRIVE_INFO;//硬盘参数表
	memory_end = (1<<20) + (EXT_MEM_K<<10); //1M+
	memory_end &= 0xfffff000; //4k对其
	if (memory_end > 16*1024*1024)
		memory_end = 16*1024*1024;  //最大支持16M
	if (memory_end > 12*1024*1024) 
		buffer_memory_end = 4*1024*1024;  //最大内存12m时，高速缓存结束地址4M
	else if (memory_end > 6*1024*1024)
		buffer_memory_end = 2*1024*1024; //最大内存6M时，高速缓存结束地址2M
	else
		buffer_memory_end = 1*1024*1024; //否则高速缓存地址结束在1M
	main_memory_start = buffer_memory_end;  //主存开始的地址是高速缓存结束的地址
	
#ifdef RAMDISK  //如果配置了ramdisk 则，再从主存中reserve出来一部分，rd_init将reserve的全部清0
	main_memory_start += rd_init(main_memory_start, RAMDISK*1024);
#endif

//内存初始化，搞得就是buddy管理的那部分，
//设置mem_map数组，除了主存区，其他部分全部设置为used
	mem_init(main_memory_start,memory_end);

	trap_init();/*设置大部分异常的处理函数，其他特殊的由具体硬件初始化时设置*/
////块设备初始化，IO请求数组初始化
	blk_dev_init();/*  */
//字符设备初始化 目前为空
	chr_dev_init();/*  */


	tty_init();/*待分析*/

	//初始化开机时间保存到startup_time
	time_init();/*  */
	//任务相关的初始化，设置ldt，tss等
	sched_init();/*  */
	//将高速缓存按1k大小用链表管理起来  给块设备使用的
	buffer_init(buffer_memory_end);/*  */
	//设置io request处理函数，设置hd中断处理函数
	hd_init();/*  */
	//软驱初始化和hd一样 设置IO REQUEST的处理函数  设置软驱中断处理
	floppy_init();/*  */
	//初始化完成 可以打开中断了，我们已经设置了所有 中断处理函数
	sti();/* 打开中断  可以接受中断了 包括时钟中断 */

	//内核堆栈中设置调用栈，然后iret返回到用户态
	//到目前为止初始化代码使用的都是user_stack数组的栈而不是task struct页中的内核堆栈
	//这里回到用户态后再回到内核态时就会使用内核态堆栈
	move_to_user_mode();/*  */


	//注意此时是在用户态 执行fork生成init 1号进程
	if (!fork()) {		/* we count on this going ok */
		//init进程执行init函数继续初始化
		init();
	}
/*
 *   NOTE!!   For any other task 'pause()' would mean we have to get a
 * signal to awaken, but task0 is the sole exception (see 'schedule()')
 * as task 0 gets activated at every idle moment (when no other tasks
 * can run). For task0 'pause()' just means we go check if some other
 * task can run, and if not we return here.
 */
 //idle进程执行pause调用
	for(;;)
		//这里会把idle进程设置为可中断等待状态，但是调度程序
		//如果发现没有其他进程可以运行，则会忽略其等待状态，让其运行
		__asm__("int $0x80"::"a" (__NR_pause):"ax");
}

static int printf(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);

	//将vsprintf生成的格式化数据写到1，标准输出中
	write(1,printbuf,i=vsprintf(printbuf, fmt, args));
	va_end(args);
	return i;
}


//init进程执行的逻辑
void init(void)
{
	int pid,i;

	// 读取硬盘参数，包括分区表信息  系统调用sys_setup  
	// 挂载根文件系统
	// 所谓挂载 就是读进超级块，然后设置一些inode之类的信息，设置根设备号
	setup((void *) &drive_info);

	//打开tty1
	(void) open("/dev/tty1",O_RDWR,0);
	//复制文件描述符
	(void) dup(0);
	(void) dup(0);

	//打印内存信息
	printf("%d buffers = %d bytes buffer space\n\r",NR_BUFFERS,
		NR_BUFFERS*BLOCK_SIZE);
	printf("Free mem: %d bytes\n\r",memory_end-main_memory_start);
	if (!(pid=fork())) {
		close(0); /* sys_close */
		//子进程逻辑
		//上面close 0，这里打开/etc/rc，因此得到的fd=0
		if (open("/etc/rc",O_RDONLY,0))
			_exit(1);
		//执行sh，导致sh从rc中读取内容并执行
		execve("/bin/sh",argv_rc,envp_rc);
		_exit(2); //execv执行失败则退出
	}
	if (pid>0) 
		//init进程等待子进程退出
		while (pid != wait(&i))
			/* nothing */;
	while (1) { 
		//如果子进程退出 则重新fork 
		if ((pid=fork())<0) {
			printf("Fork failed in init\r\n");
			continue;
		}
		if (!pid) {
			//fork出来的子进程执行sh
			close(0);close(1);close(2);
			setsid();   //创建新的会话期
			(void) open("/dev/tty1",O_RDWR,0);
			(void) dup(0);
			(void) dup(0);
			_exit(execve("/bin/sh",argv,envp));  //这里的argv和envp与第一次不一样，这里是login shell
		}
		while (1)
			//仍然是等待进程退出
			if (pid == wait(&i))
				break;
		printf("\n\rchild %d died with code %04x\n\r",pid,i);
		sync();
	}
	_exit(0);	/* NOTE! _exit, not exit() */
}
