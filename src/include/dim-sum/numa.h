#ifndef _DIM_SUM_NUMA_H
#define _DIM_SUM_NUMA_H

struct memory_node;

#ifdef CONFIG_NODES_SHIFT
#define MEM_NODES_SHIFT     CONFIG_NODES_SHIFT
#else
#define MEM_NODES_SHIFT     0
#endif

#define nr_node_ids		1

#define MAX_NUMNODES    (1 << MEM_NODES_SHIFT)

#define	NUMA_NO_NODE	(-1)

#define num_online_nodes()	1
#define num_possible_nodes()	1
#define node_online(node)	((node) == 0)
#define node_possible(node)	((node) == 0)

extern struct memory_node sole_memory_node;
#define MEMORY_NODE(nid)		(&sole_memory_node)
/**
 * 获得当前CPU所在的NUMA节点
 * 目前不支持NUMA，统一返回0
 */
#define numa_node_id()		0

#endif /* _DIM_SUM_NUMA_H */
