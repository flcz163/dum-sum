#ifndef __ASM_GENERIC_SCATTERLIST_H
#define __ASM_GENERIC_SCATTERLIST_H

/**
 * 描述分散、聚集映射中，每个缓冲页面。
 */
struct scatterlist {
	/**
	 * 缓冲所在页面。
	 */
	unsigned long	page_link;
	/**
	 * 缓冲区在页内的偏移。
	 */
	unsigned int	offset;
	/**
	 * 缓冲区在页内的长度。
	 */
	unsigned int	length;
};

#endif /* __ASM_GENERIC_SCATTERLIST_H */
