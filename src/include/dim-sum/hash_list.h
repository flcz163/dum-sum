#ifndef __DIM_SUM_HASH_LIST_H
#define __DIM_SUM_HASH_LIST_H

#include <dim-sum/double_list.h>

static inline void hash_list_init_bucket(struct hash_list_bucket *ptr)
{
	list_init(&ptr->head);
}

static inline void hash_list_init_node(struct hash_list_node *h)
{
	list_init(&h->node);
}

static inline int hash_node_is_hashed(const struct hash_list_node *h)
{
	return list_is_empty(&h->node);
}

static inline int hash_node_is_unhashed(const struct hash_list_node *h)
{
	return !hash_node_is_hashed(h);
}

static inline int hlist_is_empty(const struct hash_list_bucket *h)
{
	return list_is_empty(&h->head);
}

static inline void __hlist_del(struct hash_list_node *n)
{
	list_del(&n->node);
}

static inline void hlist_del(struct hash_list_node *n)
{
	list_del_init(&n->node);
}

static inline void hlist_del_init(struct hash_list_node *n)
{
	list_del_init(&n->node);
}

static inline void hlist_add_head(struct hash_list_node *n, struct hash_list_bucket *h)
{
	list_insert_behind(&n->node, &h->head);
}

/* next must be != NULL */
static inline void hlist_add_before(struct hash_list_node *n,
					struct hash_list_node *next)
{
	list_insert_front(&n->node, &next->node);
}

static inline void hlist_add_behind(struct hash_list_node *n,
				    struct hash_list_node *prev)
{
	list_insert_behind(&n->node, &prev->node);
}

#define hlist_entry(ptr, type, member) container_of(ptr,type,member)

#define hlist_for_each(pos, h) \
		for (pos = container_of((h)->head.next, struct hash_list_node, node);	\
			&pos->node != &(h)->head;	\
			pos = container_of(pos->node.next, struct hash_list_node, node))

#define hlist_container(ptr, type, member) \
	container_of(ptr, type, member.node)

#define hlist_first_container(ptr, type, member) \
	hlist_container((ptr)->head.next, type, member)

static inline void
init_hash_list(void *addr, int mem_size, int *hash_mask, int *hash_bits)
{
	unsigned int nr_hash;
	int bits, mask;
	struct hash_list_bucket *bucket;
	int i;

	nr_hash = mem_size / sizeof(struct hash_list_bucket);

	bits = 0;
	do {
		bits++;
	} while ((nr_hash >> bits) != 0);
	bits--;

	nr_hash = 1UL << bits;
	mask = nr_hash - 1;

	bucket = addr;

	for (i = 0; i < nr_hash; i++)
		hash_list_init_bucket(bucket + i);

	*hash_bits = bits;
	*hash_mask = mask;
}

static inline void lock_hash_bucket(struct hash_list_bucket *head)
{
}
	
static inline void unlock_hash_bucket(struct hash_list_bucket *head)
{
}

#endif /* __DIM_SUM_HASH_LIST_H */
