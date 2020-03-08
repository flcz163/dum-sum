#include <stdlib.h>
#include <stdio.h>
#include <sys/mount.h>
#include <fcntl.h>

#include <dim-sum/task.h>
#include <dim-sum/time.h>

#include <shell/sh_main.h>

extern unsigned long long jiffies_64;

void fs_test(void);
void fs_test(void)
{
	__maybe_unused int err;
	__maybe_unused char write_buf[255];
	__maybe_unused char read_buf[255];
	__maybe_unused int fd;
	int i;

	fd = open("/xby1.txt", O_CREAT | O_RDWR, 0);
	printf("xby_debug in task1, creat result is %d\n", fd);

	sprintf(write_buf, "this is my test in xby1.txt.\n");
	err = write(fd,  write_buf, strlen(write_buf));
	printf("xby_debug in task1, write result is %d\n", err);
	llseek(fd, 0, 0);
	memset(read_buf, 0, 255);
	err = read(fd, read_buf, 255);
	printf("xby_debug in task1, read result is %d, [%s]\n", err, read_buf);
		
	err = creat("/xby2.txt", 0);
	printf("xby_debug in task1, creat result is %d\n", err);

	err = mkdir("/mydir", 0);
	printf("xby_debug in task1, mkdir result is %d\n", err);

	fd = open("/a1.txt", O_CREAT | O_RDWR | O_SYNC, 0);
	printf("xby_debug in task1, creat /ext3/a1.txt result is %d\n", fd);
	fdatasync(fd);
	printf("xby_debug in task1, step 1\n");
	fsync(fd);
	printf("xby_debug in task1, step 2\n");

	for (i = 0; i < 20; i++) {
		sprintf(write_buf, "jiffies is %llu.\n", jiffies_64);
		err = write(fd,  write_buf, strlen(write_buf));
	}

	printf("xby_debug in task1, write result is %d\n", err);

	memset(read_buf, 0, 255);
	llseek(fd, 0, 0);
	err = read(fd, read_buf, 255);
	printf("xby_debug in task1, read /a1.txt result is %d, [%s]\n", err, read_buf);

	close(fd);
	printf("xby_debug in task1, step 3\n");

	fd = open("/tmp/1.txt", O_CREAT | O_RDWR | O_SYNC, 0);
	printf("xby_debug in task1, creat /tmp/1.txt result is %d\n", fd);
	fdatasync(fd);
	printf("xby_debug in task1, step 1\n");
	fsync(fd);
	printf("xby_debug in task1, step 2\n");

	sprintf(write_buf, "this is my test in 1.txt, jiffies is %llu.\n", jiffies_64);
	err = write(fd,  write_buf, strlen(write_buf));
	printf("xby_debug in task1, write result is %d\n", err);

	memset(read_buf, 0, 255);
	llseek(fd, 0, 0);
	err = read(fd, read_buf, 255);
	printf("xby_debug in task1, read /tmp/1.txt result is %d, [%s]\n", err, read_buf);

	close(fd);
}

static int count = 0;
static int task1(void *data)
{
	__maybe_unused int err;
	__maybe_unused char *write_buf;
	__maybe_unused char read_buf[255];
	__maybe_unused int fd;
	
	printf("xby_debug in task1, enter\n");

	//while (1);
#if 0
	xby_ls_dev();
	//xby_ls_root();

	fd = open("/xby1.txt", O_CREAT | O_RDWR, 0);
	printf("xby_debug in task1, creat result is %d\n", fd);

	write_buf = "this is my test in xby1.txt.\n";
	err = write(fd,  write_buf, strlen(write_buf));
	printf("xby_debug in task1, write result is %d\n", err);
	llseek(fd, 0, 0);
	memset(read_buf, 0, 255);
	err = read(fd, read_buf, 255);
	printf("xby_debug in task1, read result is %d, [%s]\n", err, read_buf);
		
	err = creat("/xby2.txt", 0);
	printf("xby_debug in task1, creat result is %d\n", err);

	err = mkdir("/mydir", 0);
	printf("xby_debug in task1, mkdir result is %d\n", err);

	fd = open("/ext3/a1.txt", O_CREAT | O_RDWR | O_SYNC, 0);
	printf("xby_debug in task1, creat /ext3/a1.txt result is %d\n", fd);
	fdatasync(fd);
	printf("xby_debug in task1, step 1\n");
	fsync(fd);
	printf("xby_debug in task1, step 2\n");
	
	
	//remove("/ext3/a1.txt");
	//printf("xby_debug in task1, step 4\n");

#if 0
	write_buf = "this is my test in a1.txt.\n";
	err = write(fd,  write_buf, strlen(write_buf));
	printf("xby_debug in task1, write result is %d\n", err);
#else
	memset(read_buf, 0, 255);
	llseek(fd, 0, 0);
	err = read(fd, read_buf, 255);
	printf("xby_debug in task1, read /ext3/a1.txt result is %d, [%s]\n", err, read_buf);
#endif

	close(fd);
	printf("xby_debug in task1, step 3\n");
	//umount("/ext3");
	//sync();
	//printf("xby_debug in task1, step 5\n");

#if 0
	printf("hold_percpu_var-------------------------------------.\n");
	fd = open("/dev/console", O_RDWR, 0);
	err = write(fd, "hello world.\n", 13);
	printf("****************[%d][%d]*******************.\n", fd, err);
#endif
#endif
{
	extern int xby_test_blk(int sector);
	extern int xlk_test_blk(int sector);
//	xby_test_blk(99);
//	xlk_test_blk(99);
}
	while (1)
	{
#if 0
		char c = getchar();
		if (c != 255)
			printf("xby_debug in task1, count is %d, %c\n", count, c);
#endif
		sleep(2000);
		count++; 
	}

	return 0;
}

static int task2(void *data)
{
	printf("xby_debug in task2, enter\n");
	while (1)
	{
		sleep(1000);
		count++;
	//	printf("xby_debug in task2, count is %d\n", count);
	}

	return 0;
}

#if 0
static void write_jiffies64_to_file(char *filename)
{
	int fd;
	unsigned long long jiffies = get_jiffies_64();
	int len;
	
  	fd = open(filename, O_RDWR | O_CREAT, 0644); /* fail below */
	len = write(fd, &jiffies, sizeof(jiffies));
	close(fd);
	printf("write %llu to [%s], %d\n", jiffies, filename, len);
}

static void cat_file(char *filename)
{
	int fd;
	unsigned long long jiffies = 0;

	fd = open(filename, O_RDWR | O_CREAT, 0644); /* fail below */
	read(fd, &jiffies, sizeof(jiffies));
	close(fd);

	printf("xby_debug in cat_file, jiffies is %llu\n", jiffies);
}
#else
static void write_jiffies64_to_file(char *filename)
{
	FILE *fd;
	unsigned long long jiffies = get_jiffies_64();
	int len;
	
  	fd = fopen(filename, "w"); /* fail below */
	len = fwrite(&jiffies, sizeof(jiffies), 1, fd);
	fclose(fd);
	printf("write %llu to [%s], %d\n", jiffies, filename, len);
}

static void read_jiffies64_from_file(char *filename)
{
	FILE *fd;
	unsigned long long jiffies = 0;

	fd = fopen(filename, "r"); /* fail below */
	fread(&jiffies, sizeof(jiffies), 1, fd);
	fclose(fd);

	printf("xby_debug in cat_file, jiffies is %llu\n", jiffies);
}

static void cat_file(char *filename)
{
	FILE *fd;
	char buf[255];

	fd = fopen(filename, "r"); /* fail below */
	fread(buf, sizeof(buf), 1, fd);
	fclose(fd);

	printf("file content:\n%s\n", buf);
}

#endif

extern void xby_test(int);

int xby_test_cmd(int argc, char *argv[])
{
	int fun = 1;
	
	
	if (argc >= 2)
	{
		fun = atoi(argv[1]);
	}

	if (fun == 1)
	{
		printf("xby_debug in xby_test_cmd, sleep count:[%d], tick:[%llu]\n", count, get_jiffies_64());
	}
	else if (fun == 2)
	{
		if (argc >= 3){
			write_jiffies64_to_file(argv[2]);
		}
		else {
			printf("sorry, can not understand you!\n");
		}
	}
	else if (fun == 3)
	{
		if (argc >= 3){
			read_jiffies64_from_file(argv[2]);
		}
		else {
			printf("sorry, can not understand you!\n");
		}
	}
	else if (fun == 4)
	{
		if (argc >= 3){
			cat_file(argv[2]);
		}
		else {
			printf("sorry, can not understand you!\n");
		}
	}
	else if (fun == 5)
	{
		sync();
	}
	else if (fun == 6)
	{
		extern void dump_stack(void);
		dump_stack();
	}
	else
	{
		xby_test(fun);
	}
		
	return 0;
}



extern int cpsw_net_init(void);
extern void mount_file_systems(void);
/**
 * 用户应用程序入口，在系统完全初始化完成后运行
 * 系统处于开中断状态!!!!!!
 * 不能在此过程中阻塞 !!!!!!
 */
void usrAppInit(void)
{
	create_process(task1,
			NULL,
			"task1",
			31
		);
	create_process(task2,
			NULL,
			"task2",
			29
		);


	start_shell("shell", 6, NULL);	
}

