#include <dim-sum/smp_seq_lock.h>

void smp_seq_write_lock(struct smp_seq_lock *lock)
{
	smp_lock(&lock->lock);
	lock->sequence++;
	smp_wmb();
}

void smp_seq_write_unlock(struct smp_seq_lock *lock)
{
	smp_wmb();
	lock->sequence++;
	smp_unlock(&lock->lock);
}

int smp_seq_write_trylock(struct smp_seq_lock *lock)
{
	int ret;

	ret = smp_trylock(&lock->lock);
	if (ret) {
		lock->sequence++;
		smp_wmb();
	}

	return ret;
}
