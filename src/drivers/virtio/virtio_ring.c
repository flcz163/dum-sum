#include <dim-sum/beehive.h>
#include <dim-sum/virtio.h>
#include <dim-sum/virtio_ring.h>
#include <dim-sum/virtio_config.h>
#include <dim-sum/device.h>
#include <dim-sum/irq.h>
#include <dim-sum/scatterlist.h>
#include <dim-sum/timer.h>

struct vring_virtqueue
{
	struct virtqueue vq;

	/* Actual memory layout for this queue */
	struct vring vring;

	/* Can we use weak barriers? */
	bool weak_barriers;

	/* Other side has made a mess, don't try any more. */
	bool broken;

	/* Host supports indirect buffers */
	bool indirect;

	/* Host publishes avail event idx */
	bool event;

	/* Head of free buffer list. */
	unsigned int free_head;
	/* Number we've added since last sync. */
	unsigned int num_added;

	/* Last used index we've seen. */
	u16 last_used_idx;

	/* How to notify other side. FIXME: commonalize hcalls! */
	void (*notify)(struct virtqueue *vq);

	/* Tokens for callbacks. */
	void *data[];
};

#define to_vvq(_vq) container_of(_vq, struct vring_virtqueue, vq)

static inline struct scatterlist *sg_next_chained(struct scatterlist *sg,
						  unsigned int *count)
{
	return sg_next(sg);
}

static inline struct scatterlist *sg_next_arr(struct scatterlist *sg,
					      unsigned int *count)
{
	if (--(*count) == 0)
		return NULL;
	return sg + 1;
}

/* Set up an indirect table of descriptors and add it to the queue. */
static inline int vring_add_indirect(struct vring_virtqueue *vq,
				     struct scatterlist *sgs[],
				     struct scatterlist *(*next)
				       (struct scatterlist *, unsigned int *),
				     unsigned int total_sg,
				     unsigned int total_out,
				     unsigned int total_in,
				     unsigned int out_sgs,
				     unsigned int in_sgs,
				     paf_t paf)
{
	struct vring_desc *desc;
	unsigned head;
	struct scatterlist *sg;
	int i, n;

	/*
	 * We require lowmem mappings for the descriptors because
	 * otherwise linear_virt_to_phys will give us bogus addresses in the
	 * virtqueue.
	 */
	paf &= ~(__PAF_USER | __PAF_EMERG);

	desc = kmalloc(total_sg * sizeof(struct vring_desc), paf);
	if (!desc)
		return -ENOMEM;

	/* Transfer entries from the sg lists into the indirect page */
	i = 0;
	for (n = 0; n < out_sgs; n++) {
		for (sg = sgs[n]; sg; sg = next(sg, &total_out)) {
			desc[i].flags = VRING_DESC_F_NEXT;
			desc[i].addr = sg_phys(sg);
			desc[i].len = sg->length;
			desc[i].next = i+1;
			i++;
		}
	}
	for (; n < (out_sgs + in_sgs); n++) {
		for (sg = sgs[n]; sg; sg = next(sg, &total_in)) {
			desc[i].flags = VRING_DESC_F_NEXT|VRING_DESC_F_WRITE;
			desc[i].addr = sg_phys(sg);
			desc[i].len = sg->length;
			desc[i].next = i+1;
			i++;
		}
	}
	BUG_ON(i != total_sg);

	/* Last one doesn't continue. */
	desc[i-1].flags &= ~VRING_DESC_F_NEXT;
	desc[i-1].next = 0;

	/* We're about to use a buffer */
	vq->vq.num_free--;

	/* Use a single buffer which doesn't continue */
	head = vq->free_head;
	vq->vring.desc[head].flags = VRING_DESC_F_INDIRECT;
	vq->vring.desc[head].addr = linear_virt_to_phys(desc);
	vq->vring.desc[head].len = i * sizeof(struct vring_desc);

	/* Update free pointer */
	vq->free_head = vq->vring.desc[head].next;

	return head;
}

static inline int virtqueue_add(struct virtqueue *_vq,
				struct scatterlist *sgs[],
				struct scatterlist *(*next)
				  (struct scatterlist *, unsigned int *),
				unsigned int total_out,
				unsigned int total_in,
				unsigned int out_sgs,
				unsigned int in_sgs,
				void *data,
				paf_t paf)
{
	struct vring_virtqueue *vq = to_vvq(_vq);
	struct scatterlist *sg;
	unsigned int i, n, avail, prev, total_sg;
	int head;

	BUG_ON(data == NULL);

	total_sg = total_in + total_out;

	/* If the host supports indirect descriptor tables, and we have multiple
	 * buffers, then go indirect. FIXME: tune this threshold */
	if (vq->indirect && total_sg > 1 && vq->vq.num_free) {
		head = vring_add_indirect(vq, sgs, next, total_sg, total_out,
					  total_in,
					  out_sgs, in_sgs, paf);
		if (likely(head >= 0))
			goto add_head;
	}

	BUG_ON(total_sg > vq->vring.num);
	BUG_ON(total_sg == 0);

	if (vq->vq.num_free < total_sg) {
		printk("Can't add buf len %i - avail = %i\n",
			 total_sg, vq->vq.num_free);
		/* FIXME: for historical reasons, we force a notify here if
		 * there are outgoing parts to the buffer.  Presumably the
		 * host should service the ring ASAP. */
		if (out_sgs)
			vq->notify(&vq->vq);

		return -ENOSPC;
	}

	/* We're about to use some buffers from the free list. */
	vq->vq.num_free -= total_sg;

	head = i = vq->free_head;
	for (n = 0; n < out_sgs; n++) {
		for (sg = sgs[n]; sg; sg = next(sg, &total_out)) {
			vq->vring.desc[i].flags = VRING_DESC_F_NEXT;
			vq->vring.desc[i].addr = sg_phys(sg);
			vq->vring.desc[i].len = sg->length;
			prev = i;
			i = vq->vring.desc[i].next;
		}
	}
	for (; n < (out_sgs + in_sgs); n++) {
		for (sg = sgs[n]; sg; sg = next(sg, &total_in)) {
			vq->vring.desc[i].flags = VRING_DESC_F_NEXT|VRING_DESC_F_WRITE;
			vq->vring.desc[i].addr = sg_phys(sg);
			vq->vring.desc[i].len = sg->length;
			prev = i;
			i = vq->vring.desc[i].next;
		}
	}
	/* Last one doesn't continue. */
	vq->vring.desc[prev].flags &= ~VRING_DESC_F_NEXT;

	/* Update free pointer */
	vq->free_head = i;

add_head:
	/* Set token. */
	vq->data[head] = data;

	/* Put entry in available array (but don't update avail->idx until they
	 * do sync). */
	avail = (vq->vring.avail->idx & (vq->vring.num-1));
	vq->vring.avail->ring[avail] = head;

	/* Descriptors and available array need to be set before we expose the
	 * new available array entries. */
	virtio_wmb(vq->weak_barriers);
	vq->vring.avail->idx++;
	vq->num_added++;

	/* This is very unlikely, but theoretically possible.  Kick
	 * just in case. */
	if (unlikely(vq->num_added == (1 << 16) - 1))
		virtqueue_kick(_vq);

	return 0;
}

/**
 * virtqueue_add_sgs - expose buffers to other end
 * @vq: the struct virtqueue we're talking about.
 * @sgs: array of terminated scatterlists.
 * @out_num: the number of scatterlists readable by other side
 * @in_num: the number of scatterlists which are writable (after readable ones)
 * @data: the token identifying the buffer.
 * @paf: how to do memory allocations (if necessary).
 *
 * Caller must ensure we don't call this with other virtqueue operations
 * at the same time (except where noted).
 *
 * Returns zero or a negative error (ie. ENOSPC, ENOMEM).
 */
int virtqueue_add_sgs(struct virtqueue *_vq,
		      struct scatterlist *sgs[],
		      unsigned int out_sgs,
		      unsigned int in_sgs,
		      void *data,
		      paf_t paf)
{
	unsigned int i, total_out, total_in;

	/* Count them first. */
	for (i = total_out = total_in = 0; i < out_sgs; i++) {
		struct scatterlist *sg;
		for (sg = sgs[i]; sg; sg = sg_next(sg))
			total_out++;
	}
	for (; i < out_sgs + in_sgs; i++) {
		struct scatterlist *sg;
		for (sg = sgs[i]; sg; sg = sg_next(sg))
			total_in++;
	}

	return virtqueue_add(_vq, sgs, sg_next_chained,
			     total_out, total_in, out_sgs, in_sgs, data, paf);
}

/**
 * virtqueue_add_outbuf - expose output buffers to other end
 * @vq: the struct virtqueue we're talking about.
 * @sgs: array of scatterlists (need not be terminated!)
 * @num: the number of scatterlists readable by other side
 * @data: the token identifying the buffer.
 * @paf: how to do memory allocations (if necessary).
 *
 * Caller must ensure we don't call this with other virtqueue operations
 * at the same time (except where noted).
 *
 * Returns zero or a negative error (ie. ENOSPC, ENOMEM).
 */
int virtqueue_add_outbuf(struct virtqueue *vq,
			 struct scatterlist sg[], unsigned int num,
			 void *data,
			 paf_t paf)
{
	return virtqueue_add(vq, &sg, sg_next_arr, num, 0, 1, 0, data, paf);
}

/**
 * virtqueue_add_inbuf - expose input buffers to other end
 * @vq: the struct virtqueue we're talking about.
 * @sgs: array of scatterlists (need not be terminated!)
 * @num: the number of scatterlists writable by other side
 * @data: the token identifying the buffer.
 * @paf: how to do memory allocations (if necessary).
 *
 * Caller must ensure we don't call this with other virtqueue operations
 * at the same time (except where noted).
 *
 * Returns zero or a negative error (ie. ENOSPC, ENOMEM).
 */
int virtqueue_add_inbuf(struct virtqueue *vq,
			struct scatterlist sg[], unsigned int num,
			void *data,
			paf_t paf)
{
	return virtqueue_add(vq, &sg, sg_next_arr, 0, num, 0, 1, data, paf);
}

/**
 * virtqueue_kick_prepare - first half of split virtqueue_kick call.
 * @vq: the struct virtqueue
 *
 * Instead of virtqueue_kick(), you can do:
 *	if (virtqueue_kick_prepare(vq))
 *		virtqueue_notify(vq);
 *
 * This is sometimes useful because the virtqueue_kick_prepare() needs
 * to be serialized, but the actual virtqueue_notify() call does not.
 */
bool virtqueue_kick_prepare(struct virtqueue *_vq)
{
	struct vring_virtqueue *vq = to_vvq(_vq);
	u16 new, old;
	bool needs_kick;

	/* We need to expose available array entries before checking avail
	 * event. */
	virtio_mb(vq->weak_barriers);

	old = vq->vring.avail->idx - vq->num_added;
	new = vq->vring.avail->idx;
	vq->num_added = 0;

	if (vq->event) {
		needs_kick = vring_need_event(vring_avail_event(&vq->vring),
					      new, old);
	} else {
		needs_kick = !(vq->vring.used->flags & VRING_USED_F_NO_NOTIFY);
	}

	return needs_kick;
}

/**
 * virtqueue_notify - second half of split virtqueue_kick call.
 * @vq: the struct virtqueue
 *
 * This does not need to be serialized.
 */
void virtqueue_notify(struct virtqueue *_vq)
{
	struct vring_virtqueue *vq = to_vvq(_vq);

	/* Prod other side to tell it about changes. */
	vq->notify(_vq);
}

/**
 * virtqueue_kick - update after add_buf
 * @vq: the struct virtqueue
 *
 * After one or more virtqueue_add_* calls, invoke this to kick
 * the other side.
 *
 * Caller must ensure we don't call this with other virtqueue
 * operations at the same time (except where noted).
 */
void virtqueue_kick(struct virtqueue *vq)
{
	if (virtqueue_kick_prepare(vq))
		virtqueue_notify(vq);
}

static void detach_buf(struct vring_virtqueue *vq, unsigned int head)
{
	unsigned int i;

	/* Clear data ptr. */
	vq->data[head] = NULL;

	/* Put back on free list: find end */
	i = head;

	/* Free the indirect table */
	if (vq->vring.desc[i].flags & VRING_DESC_F_INDIRECT)
		kfree(linear_phys_to_virt(vq->vring.desc[i].addr));

	while (vq->vring.desc[i].flags & VRING_DESC_F_NEXT) {
		i = vq->vring.desc[i].next;
		vq->vq.num_free++;
	}

	vq->vring.desc[i].next = vq->free_head;
	vq->free_head = head;
	/* Plus final descriptor */
	vq->vq.num_free++;
}

static inline bool more_used(const struct vring_virtqueue *vq)
{
	return vq->last_used_idx != vq->vring.used->idx;
}

/**
 * virtqueue_get_buf - get the next used buffer
 * @vq: the struct virtqueue we're talking about.
 * @len: the length written into the buffer
 *
 * If the driver wrote data into the buffer, @len will be set to the
 * amount written.  This means you don't need to clear the buffer
 * beforehand to ensure there's no data leakage in the case of short
 * writes.
 *
 * Caller must ensure we don't call this with other virtqueue
 * operations at the same time (except where noted).
 *
 * Returns NULL if there are no used buffers, or the "data" token
 * handed to virtqueue_add_*().
 */
void *virtqueue_get_buf(struct virtqueue *_vq, unsigned int *len)
{
	struct vring_virtqueue *vq = to_vvq(_vq);
	void *ret;
	unsigned int i;
	u16 last_used;

	if (unlikely(vq->broken)) {
		return NULL;
	}

	if (!more_used(vq)) {
		return NULL;
	}

	/* Only get used array entries after they have been exposed by host. */
	virtio_rmb(vq->weak_barriers);

	last_used = (vq->last_used_idx & (vq->vring.num - 1));
	i = vq->vring.used->ring[last_used].id;
	*len = vq->vring.used->ring[last_used].len;

	if (unlikely(i >= vq->vring.num)) {
		return NULL;
	}
	if (unlikely(!vq->data[i])) {
		return NULL;
	}

	/* detach_buf clears data, so grab it now. */
	ret = vq->data[i];
	detach_buf(vq, i);
	vq->last_used_idx++;
	/* If we expect an interrupt for the next entry, tell host
	 * by writing event index and flush out the write before
	 * the read in the next get_buf call. */
	if (!(vq->vring.avail->flags & VRING_AVAIL_F_NO_INTERRUPT)) {
		vring_used_event(&vq->vring) = vq->last_used_idx;
		virtio_mb(vq->weak_barriers);
	}

	return ret;
}

/**
 * virtqueue_disable_cb - disable callbacks
 * @vq: the struct virtqueue we're talking about.
 *
 * Note that this is not necessarily synchronous, hence unreliable and only
 * useful as an optimization.
 *
 * Unlike other operations, this need not be serialized.
 */
void virtqueue_disable_cb(struct virtqueue *_vq)
{
	struct vring_virtqueue *vq = to_vvq(_vq);

	vq->vring.avail->flags |= VRING_AVAIL_F_NO_INTERRUPT;
}

/**
 * virtqueue_enable_cb_prepare - restart callbacks after disable_cb
 * @vq: the struct virtqueue we're talking about.
 *
 * This re-enables callbacks; it returns current queue state
 * in an opaque unsigned value. This value should be later tested by
 * virtqueue_poll, to detect a possible race between the driver checking for
 * more work, and enabling callbacks.
 *
 * Caller must ensure we don't call this with other virtqueue
 * operations at the same time (except where noted).
 */
unsigned virtqueue_enable_cb_prepare(struct virtqueue *_vq)
{
	struct vring_virtqueue *vq = to_vvq(_vq);
	u16 last_used_idx;

	/* We optimistically turn back on interrupts, then check if there was
	 * more to do. */
	/* Depending on the VIRTIO_RING_F_EVENT_IDX feature, we need to
	 * either clear the flags bit or point the event index at the next
	 * entry. Always do both to keep code simple. */
	vq->vring.avail->flags &= ~VRING_AVAIL_F_NO_INTERRUPT;
	vring_used_event(&vq->vring) = last_used_idx = vq->last_used_idx;
	return last_used_idx;
}

/**
 * virtqueue_poll - query pending used buffers
 * @vq: the struct virtqueue we're talking about.
 * @last_used_idx: virtqueue state (from call to virtqueue_enable_cb_prepare).
 *
 * Returns "true" if there are pending used buffers in the queue.
 *
 * This does not need to be serialized.
 */
bool virtqueue_poll(struct virtqueue *_vq, unsigned last_used_idx)
{
	struct vring_virtqueue *vq = to_vvq(_vq);

	virtio_mb(vq->weak_barriers);
	return (u16)last_used_idx != vq->vring.used->idx;
}

/**
 * virtqueue_enable_cb - restart callbacks after disable_cb.
 * @vq: the struct virtqueue we're talking about.
 *
 * This re-enables callbacks; it returns "false" if there are pending
 * buffers in the queue, to detect a possible race between the driver
 * checking for more work, and enabling callbacks.
 *
 * Caller must ensure we don't call this with other virtqueue
 * operations at the same time (except where noted).
 */
bool virtqueue_enable_cb(struct virtqueue *_vq)
{
	unsigned last_used_idx = virtqueue_enable_cb_prepare(_vq);
	return !virtqueue_poll(_vq, last_used_idx);
}

/**
 * virtqueue_enable_cb_delayed - restart callbacks after disable_cb.
 * @vq: the struct virtqueue we're talking about.
 *
 * This re-enables callbacks but hints to the other side to delay
 * interrupts until most of the available buffers have been processed;
 * it returns "false" if there are many pending buffers in the queue,
 * to detect a possible race between the driver checking for more work,
 * and enabling callbacks.
 *
 * Caller must ensure we don't call this with other virtqueue
 * operations at the same time (except where noted).
 */
bool virtqueue_enable_cb_delayed(struct virtqueue *_vq)
{
	struct vring_virtqueue *vq = to_vvq(_vq);
	u16 bufs;

	/* We optimistically turn back on interrupts, then check if there was
	 * more to do. */
	/* Depending on the VIRTIO_RING_F_USED_EVENT_IDX feature, we need to
	 * either clear the flags bit or point the event index at the next
	 * entry. Always do both to keep code simple. */
	vq->vring.avail->flags &= ~VRING_AVAIL_F_NO_INTERRUPT;
	/* TODO: tune this threshold */
	bufs = (u16)(vq->vring.avail->idx - vq->last_used_idx) * 3 / 4;
	vring_used_event(&vq->vring) = vq->last_used_idx + bufs;
	virtio_mb(vq->weak_barriers);
	if (unlikely((u16)(vq->vring.used->idx - vq->last_used_idx) > bufs)) {
		return false;
	}

	return true;
}

/**
 * virtqueue_detach_unused_buf - detach first unused buffer
 * @vq: the struct virtqueue we're talking about.
 *
 * Returns NULL or the "data" token handed to virtqueue_add_*().
 * This is not valid on an active queue; it is useful only for device
 * shutdown.
 */
void *virtqueue_detach_unused_buf(struct virtqueue *_vq)
{
	struct vring_virtqueue *vq = to_vvq(_vq);
	unsigned int i;
	void *buf;

	for (i = 0; i < vq->vring.num; i++) {
		if (!vq->data[i])
			continue;
		/* detach_buf clears data, so grab it now. */
		buf = vq->data[i];
		detach_buf(vq, i);
		vq->vring.avail->idx--;
		return buf;
	}
	/* That should have freed everything. */
	BUG_ON(vq->vq.num_free != vq->vring.num);

	return NULL;
}

enum isr_result vring_interrupt(int irq, void *_vq)
{
	struct vring_virtqueue *vq = to_vvq(_vq);

	if (!more_used(vq)) {
		return ISR_SKIP;
	}

	if (unlikely(vq->broken))
		return ISR_EATEN;

	if (vq->vq.callback)
		vq->vq.callback(&vq->vq);

	return ISR_EATEN;
}

struct virtqueue *vring_new_virtqueue(unsigned int index,
				      unsigned int num,
				      unsigned int vring_align,
				      struct virtio_device *vdev,
				      bool weak_barriers,
				      void *pages,
				      void (*notify)(struct virtqueue *),
				      void (*callback)(struct virtqueue *),
				      const char *name)
{
	struct vring_virtqueue *vq;
	unsigned int i;

	/* We assume num is a power of 2. */
	if (num & (num - 1)) {
		return NULL;
	}

	vq = kmalloc(sizeof(*vq) + sizeof(void *)*num, PAF_KERNEL);
	if (!vq)
		return NULL;

	vring_init(&vq->vring, num, pages, vring_align);
	vq->vq.callback = callback;
	vq->vq.vdev = vdev;
	vq->vq.name = name;
	vq->vq.num_free = num;
	vq->vq.index = index;
	vq->notify = notify;
	vq->weak_barriers = weak_barriers;
	vq->broken = false;
	vq->last_used_idx = 0;
	vq->num_added = 0;
	list_insert_behind(&vq->vq.list, &vdev->vqs);

	vq->indirect = virtio_has_feature(vdev, VIRTIO_RING_F_INDIRECT_DESC);
	vq->event = virtio_has_feature(vdev, VIRTIO_RING_F_EVENT_IDX);

	/* No callback?  Tell other side not to bother us. */
	if (!callback)
		vq->vring.avail->flags |= VRING_AVAIL_F_NO_INTERRUPT;

	/* Put everything in free lists. */
	vq->free_head = 0;
	for (i = 0; i < num-1; i++) {
		vq->vring.desc[i].next = i+1;
		vq->data[i] = NULL;
	}
	vq->data[i] = NULL;

	return &vq->vq;
}

void vring_del_virtqueue(struct virtqueue *vq)
{
	list_del(&vq->list);
	kfree(to_vvq(vq));
}

/* Manipulates transport-specific feature bits. */
void vring_transport_features(struct virtio_device *vdev)
{
	unsigned int i;

	for (i = VIRTIO_TRANSPORT_F_START; i < VIRTIO_TRANSPORT_F_END; i++) {
		switch (i) {
		case VIRTIO_RING_F_INDIRECT_DESC:
			break;
		case VIRTIO_RING_F_EVENT_IDX:
			break;
		default:
			/* We don't understand this bit. */
			atomic_clear_bit(i, vdev->features);
		}
	}
}

/**
 * virtqueue_get_vring_size - return the size of the virtqueue's vring
 * @vq: the struct virtqueue containing the vring of interest.
 *
 * Returns the size of the vring.  This is mainly used for boasting to
 * userspace.  Unlike other operations, this need not be serialized.
 */
unsigned int virtqueue_get_vring_size(struct virtqueue *_vq)
{

	struct vring_virtqueue *vq = to_vvq(_vq);

	return vq->vring.num;
}
