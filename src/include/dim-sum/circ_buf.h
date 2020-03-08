#ifndef __DIM_SUM_CIRC_BUF_H
#define __DIM_SUM_CIRC_BUF_H

struct circ_buf {
	char *buf;
	/**
	 * head == tail 表示空
	 * head + 1 == tail 表示满
	 */
	int head;
	int tail;
};

/* 缓存区已用节点数量 */
#define CIRC_CNT(head,tail,size) (((head) - (tail)) & ((size)-1))

/* 缓存区空闲节点数量 */
#define CIRC_SPACE(head,tail,size) CIRC_CNT((tail),((head)+1),(size))

/* 从当前位置到结尾(tail或者缓冲区末尾处)的数量  */
#define CIRC_CNT_TO_END(head,tail,size) \
	({int end = (size) - (tail); \
	  int n = ((head) + end) & ((size)-1); \
	  n < end ? n : end;})

/* 从当前位置到结尾(head或者缓冲区末尾处)，空闲空闲数量  */
#define CIRC_SPACE_TO_END(head,tail,size) \
	({int end = (size) - 1 - (head); \
	  int n = (end + (tail)) & ((size)-1); \
	  n <= end ? n : end+1;})

#endif /* __DIM_SUM_CIRC_BUF_H */
