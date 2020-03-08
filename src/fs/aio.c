#include <dim-sum/aio.h>
#include <dim-sum/fs.h>
#include <dim-sum/sched.h>

ssize_t fastcall wait_on_async_io(struct async_io_desc *aio)
{
	while (aio->users) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		if (!aio->users)
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);

	return aio->user_data;
}
