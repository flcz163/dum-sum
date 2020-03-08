#ifndef __DIM_SUM_LIST_H
#define __DIM_SUM_LIST_H

#include <dim-sum/types.h>
#include <dim-sum/stddef.h>
#include <dim-sum/magic.h>
#include <dim-sum/kernel.h>

#define LIST_HEAD_INITIALIZER(name) { &(name), &(name) }

static inline void list_init(struct double_list *list)
{
	list->next = list;
	list->prev = list;
}

static inline int list_is_empty(const struct double_list *head)
{
	return head->next == head;
}

static inline void __list_insert(struct double_list *new,
			      struct double_list *prev,
			      struct double_list *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

static inline void list_insert_front(struct double_list *new, struct double_list *head)
{
	__list_insert(new, head, head->next);
}

static inline void list_insert_behind(struct double_list *new, struct double_list *head)
{
	__list_insert(new, head->prev, head);
}

static inline void __list_link(struct double_list * prev, struct double_list * next)
{
	next->prev = prev;
	prev->next = next;
}

static inline void __list_del_entry(struct double_list *entry)
{
	__list_link(entry->prev, entry->next);
}

static inline void list_del(struct double_list *entry)
{
	__list_link(entry->prev, entry->next);
	entry->next = LIST_UNLINK1;
	entry->prev = LIST_UNLINK2;
}

static inline void list_del_init(struct double_list *entry)
{
	__list_del_entry(entry);
	list_init(entry);
}

static inline void list_move_to_front(struct double_list *list, struct double_list *head)
{
	__list_del_entry(list);
	list_insert_front(list, head);
}

static inline void list_move_to_behind(struct double_list *list,
				  struct double_list *head)
{
	__list_del_entry(list);
	list_insert_behind(list, head);
}

static inline void __list_combine(const struct double_list *list,
				 struct double_list *prev,
				 struct double_list *next)
{
	struct double_list *first = list->next;
	struct double_list *last = list->prev;

	first->prev = prev;
	prev->next = first;

	last->next = next;
	next->prev = last;
}

static inline void list_combine_front(const struct double_list *list,
				struct double_list *head)
{
	if (!list_is_empty(list))
		__list_combine(list, head, head->next);
}

static inline void list_combine_behind(struct double_list *list,
				struct double_list *head)
{
	if (!list_is_empty(list))
		__list_combine(list, head->prev, head);
}

static inline void list_combine_behind_init(struct double_list *list,
					 struct double_list *head)
{
	if (!list_is_empty(list)) {
		__list_combine(list, head->prev, head);
		list_init(list);
	}
}

#define list_container(ptr, type, member) \
	container_of(ptr, type, member)

#define list_first_container(ptr, type, member) \
	list_container((ptr)->next, type, member)

#define list_last_container(ptr, type, member) \
	list_container((ptr)->prev, type, member)

#define list_next_entry(pos, member) \
	list_container((pos)->member.next, typeof(*(pos)), member)

#define list_prev_entry(pos, member) \
	list_container((pos)->member.prev, typeof(*(pos)), member)

#define list_for_each(pos, head) \
	for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_prev(pos, head) \
	for (pos = (head)->prev; pos != (head); pos = pos->prev)

#define list_for_each_safe(pos, n, head) \
	for (pos = (head)->next, n = pos->next; pos != (head); \
		pos = n, n = pos->next)

#define list_for_each_prev_safe(pos, n, head) \
	for (pos = (head)->prev, n = pos->prev; \
	     pos != (head); \
	     pos = n, n = pos->prev)

#define list_for_each_entry(pos, head, member)				\
	for (pos = list_first_container(head, typeof(*pos), member);	\
	     &pos->member != (head);					\
	     pos = list_next_entry(pos, member))

#define list_for_each_entry_safe(pos, n, head, member)			\
	for (pos = list_first_container(head, typeof(*pos), member),	\
		n = list_next_entry(pos, member);			\
	     &pos->member != (head); 					\
	     pos = n, n = list_next_entry(n, member))

#endif /* __DIM_SUM_LIST_H */
