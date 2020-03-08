#ifndef __DIM_SUM_NOTIFIER_H
#define __DIM_SUM_NOTIFIER_H

#include <dim-sum/double_list.h>

/**
 * 通知链表结点。
 */
struct notifier_data
{
	/**
	 * 需要执行的函数
	 */
	int (*callback)(struct notifier_data *this, unsigned long id, void *data);
	/**
	 * 通过此字段将其链接到链表中
	 */
	struct double_list *list;
	/**
	 * 函数优先级，但是，在实际的代码中，所有注册的节点不会设置priority，而是使用默认的 0。
	 * 这就意味着，节点的执行顺序是它注册的顺序
	 */
	int priority;
};
#endif /* __DIM_SUM_NOTIFIER_H */