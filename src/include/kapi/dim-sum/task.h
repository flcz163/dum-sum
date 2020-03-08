#ifndef __KAPI_DIM_SUM_TASK_H
#define __KAPI_DIM_SUM_TASK_H

#define TaskId unsigned long
typedef  int (*TASK_FUNCTPTR)(void *data) ;
extern TaskId create_process(int (*TaskFn)(void *data),
			void *pData,
			char *strName,
			int prio
		);
#endif /* __KAPI_DIM_SUM_TASK_H */