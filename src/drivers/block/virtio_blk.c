#include <dim-sum/beehive.h>
#include <dim-sum/fs.h>
#include <dim-sum/disk.h>
#include <dim-sum/blk_dev.h>
#include <dim-sum/disk.h>
#include <dim-sum/irq.h>
#include <dim-sum/smp_lock.h>
#include <dim-sum/mutex.h>
#include <dim-sum/virtio.h>
#include <dim-sum/virtio_blk.h>
#include <dim-sum/scatterlist.h>
#include <dim-sum/sched.h>
#include <dim-sum/string.h>
#include <dim-sum/wait.h>
#include <dim-sum/idr.h>
#include <dim-sum/blkio.h>
#include <scsi/scsi_cmd.h>

#define PART_BITS 4

static DEFINE_IDR(vd_index_ida, 1 << MINORBITS);

enum {
	VBLK_IS_FLUSH		= 1,
	VBLK_REQ_FLUSH		= 2,
	VBLK_REQ_DATA		= 4,
	VBLK_REQ_FUA		= 8,
	VBLK_REQ_TEST		= 32,
};

struct virtio_blk
{
	struct virtio_device *vdev;
	struct virtqueue *vq;
	struct wait_queue queue_wait;

	/* The disk structure for the kernel. */
	struct disk_device *disk;

	struct beehive_allotter *mem_cache;

	/* What host tells us, plus 2 for header & tailer. */
	unsigned int sg_elems;

	/* Ida index - used to track minor number allocations. */
	int index;

	/* Scatterlist: can be too big for stack. */
	struct scatterlist sg[/*sg_elems*/];
};

struct virtblk_req
{
	struct blk_request *req;
	struct virtio_blk_outhdr out_hdr;
	struct virtio_scsi_inhdr in_hdr;
	struct virtio_blk *vblk;
	int flags;
	u8 status;
	struct scatterlist sg[];
};

static inline int virtblk_result(struct virtblk_req *vbr)
{
	switch (vbr->status) {
	case VIRTIO_BLK_S_OK:
		return 0;
	case VIRTIO_BLK_S_UNSUPP:
		return -ENOTTY;
	default:
		return -EIO;
	}
}

static inline struct virtblk_req *virtblk_alloc_req(struct virtio_blk *vblk,
						    paf_t paf_mask)
{
	struct virtblk_req *vbr;

	vbr = beehive_alloc(vblk->mem_cache, paf_mask);
	if (!vbr)
		return NULL;

	vbr->vblk = vblk;

	return vbr;
}

static int __virtblk_add_req(struct virtqueue *vq,
			     struct virtblk_req *vbr,
			     struct scatterlist *data_sg,
			     bool have_data)
{
	struct scatterlist hdr, status, cmd, sense, inhdr, *sgs[6];
	unsigned int num_out = 0, num_in = 0;
	int type = vbr->out_hdr.type & ~VIRTIO_BLK_T_OUT;

	sg_init_one(&hdr, &vbr->out_hdr, sizeof(vbr->out_hdr));
	sgs[num_out++] = &hdr;

	/*
	 * If this is a packet command we need a couple of additional headers.
	 * Behind the normal outhdr we put a segment with the scsi command
	 * block, and before the normal inhdr we put the sense data and the
	 * inhdr with additional status information.
	 */
	if (type == VIRTIO_BLK_T_SCSI_CMD) {
		sg_init_one(&cmd, vbr->req->dev_cmd_buf, vbr->req->dev_cmd_len);
		sgs[num_out++] = &cmd;
	}

	if (have_data) {
		if (vbr->out_hdr.type & VIRTIO_BLK_T_OUT)
			sgs[num_out++] = data_sg;
		else
			sgs[num_out + num_in++] = data_sg;
	}

	if (type == VIRTIO_BLK_T_SCSI_CMD) {
		sg_init_one(&sense, vbr->req->sense, SCSI_SENSE_BUFFERSIZE);
		sgs[num_out + num_in++] = &sense;
		sg_init_one(&inhdr, &vbr->in_hdr, sizeof(vbr->in_hdr));
		sgs[num_out + num_in++] = &inhdr;
	}

	sg_init_one(&status, &vbr->status, sizeof(vbr->status));
	sgs[num_out + num_in++] = &status;

	return virtqueue_add_sgs(vq, sgs, num_out, num_in, vbr, PAF_ATOMIC);
}

__maybe_unused static void virtblk_add_req(struct virtblk_req *vbr, bool have_data)
{
	struct virtio_blk *vblk = vbr->vblk;
	DEFINE_WAIT(wait);
	int ret;

	smp_lock_irq(vblk->disk->queue->queue_lock);
	while (unlikely((ret = __virtblk_add_req(vblk->vq, vbr, vbr->sg,
						 have_data)) < 0)) {
		prepare_to_wait_exclusive(&vblk->queue_wait, &wait,
					  TASK_UNINTERRUPTIBLE);

		smp_unlock_irq(vblk->disk->queue->queue_lock);
		io_schedule();
		smp_lock_irq(vblk->disk->queue->queue_lock);

		finish_wait(&vblk->queue_wait, &wait);
	}

	virtqueue_kick(vblk->vq);
	smp_unlock_irq(vblk->disk->queue->queue_lock);
}

static inline void virtblk_request_done(struct virtblk_req *vbr)
{
	struct virtio_blk *vblk = vbr->vblk;
	struct blk_request *req = vbr->req;
	int error = virtblk_result(vbr);

	blkdev_finish_request(req, error);
	beehive_free(vblk->mem_cache, vbr);
}

static struct page_frame *test_page;
static char *test_addr;
struct virtio_blk *test_vblk;
/* xby_debug */
static void virtblk_test_done(struct virtblk_req *vbr)
{
	int i;
	//dump_stack();
	for (i = 0; i < 512; i++)
		printk("[%d]", test_addr[i]);
	for (i = 0; i < 512; i++)
		printk("%c", test_addr[i]);
	printk("\n");
	while (1);
}

static void virtblk_done(struct virtqueue *vq)
{
	struct virtio_blk *vblk = vq->vdev->priv;
	bool bio_done = false, req_done = false;
	struct virtblk_req *vbr;
	//unsigned long flags;
	unsigned int len;

	//smp_lock_irqsave(vblk->disk->queue->queue_lock, flags);
	do {
		virtqueue_disable_cb(vq);
		while ((vbr = virtqueue_get_buf(vblk->vq, &len)) != NULL) {
			if (vbr->flags & VBLK_REQ_TEST) {
				virtblk_test_done(vbr);
				bio_done = true;
			} else {
				virtblk_request_done(vbr);
				req_done = true;
			}
		}
	} while (!virtqueue_enable_cb(vq));
	/* In case queue is stopped waiting for more buffers. */
	if (req_done)
		blkdev_start_quest(vblk->disk->queue);
	//smp_unlock_irqrestore(vblk->disk->queue->queue_lock, flags);

	if (bio_done)
		wake_up(&vblk->queue_wait);
}

static bool do_req(struct blk_request_queue *q, struct virtio_blk *vblk,
		   struct blk_request *req)
{
	unsigned int num;
	struct virtblk_req *vbr;

	vbr = virtblk_alloc_req(vblk, PAF_ATOMIC);
	if (!vbr)
		/* When another request finishes we'll try again. */
		return false;

	vbr->req = req;


	vbr->out_hdr.type = 0;
	vbr->out_hdr.sector = blk_rq_pos(vbr->req);
	vbr->out_hdr.ioprio = 0;//req_get_ioprio(vbr->req);

	num = blk_gather_bio(q, vbr->req, vblk->sg);

	if (num) {
		if (blk_request_is_write(vbr->req) == WRITE)
			vbr->out_hdr.type |= VIRTIO_BLK_T_OUT;
		else
			vbr->out_hdr.type |= VIRTIO_BLK_T_IN;
	}

	if (__virtblk_add_req(vblk->vq, vbr, vblk->sg, num) < 0) {
		beehive_free(vblk->mem_cache, vbr);
		return false;
	}

	return true;
}

static void virtblk_request(struct blk_request_queue *q)
{
	struct virtio_blk *vblk = q->queuedata;
	struct blk_request *req;
	unsigned int issued = 0;

	while ((req = iosched_get_first_request(q)) != NULL) {
		//unsigned long offset = req->sector << 9;
		//size_t len = req->sectors_seg_drv << 9;
		
		BUG_ON(req->segcount_hw + 2 > vblk->sg_elems);

		/* If this request fails, stop queue and wait for something to
		   finish to restart it. */
		if (!do_req(q, vblk, req)) {
			blk_end_request(req, 0);
		} else {
			issued++;
		}
		blkdev_dequeue_request(req);
	}
	if (issued)
		virtqueue_kick(vblk->vq);
}

static int virtblk_ioctl(struct block_device *bdev, fmode_t mode,
			     unsigned int cmd, unsigned long data)
{
	struct disk_device *disk = bdev->disk;
	struct virtio_blk *vblk = disk->private_data;

	/*
	 * Only allow the generic SCSI ioctls if the host can support it.
	 */
	if (!virtio_has_feature(vblk->vdev, VIRTIO_BLK_F_SCSI))
		return -ENOTTY;

	return scsi_cmd_blk_ioctl(bdev, mode, cmd,
				  (void __user *)data);
}

static const struct block_device_operations virtblk_fops = {
	.ioctl  = virtblk_ioctl,
};

static int init_vq(struct virtio_blk *vblk)
{
	int err = 0;

	/* We expect one virtqueue, for output. */
	vblk->vq = virtio_find_single_vq(vblk->vdev, virtblk_done, "requests");
	if (IS_ERR(vblk->vq))
		err = PTR_ERR(vblk->vq);

	return err;
}

/*
 * Legacy naming scheme used for virtio devices.  We are stuck with it for
 * virtio blk but don't ever use it for any new driver.
 */
static int virtblk_name_format(char *prefix, int index, char *buf, int buflen)
{
	const int base = 'z' - 'a' + 1;
	char *begin = buf + strlen(prefix);
	char *end = buf + buflen;
	char *p;
	int unit;

	p = end - 1;
	*p = '\0';
	unit = base;
	do {
		if (p == begin)
			return -EINVAL;
		*--p = 'a' + (index % unit);
		index = (index / unit) - 1;
	} while (index >= 0);

	memmove(begin, p, end - p);
	memcpy(buf, prefix, strlen(prefix));

	return 0;
}

__maybe_unused static int virtblk_get_cache_mode(struct virtio_device *vdev)
{
	u8 writeback;
	int err;

	err = virtio_config_val(vdev, VIRTIO_BLK_F_CONFIG_WCE,
				offsetof(struct virtio_blk_config, wce),
				&writeback);
	if (err)
		writeback = virtio_has_feature(vdev, VIRTIO_BLK_F_WCE);

	return writeback;
}

static int virtblk_probe(struct virtio_device *vdev)
{
	struct virtio_blk *vblk;
	struct blk_request_queue *q;
	int err, index;
	int pool_size;

	u64 cap;
	u32 v, blk_size, sg_elems, opt_io_size;
	u16 min_io_size;
	u8 physical_block_exp, alignment_offset;

test_page = alloc_page_frame(PAF_KERNEL);
test_addr = page_address(test_page);

	err = idr_get_new(&vd_index_ida, NULL, &index);
	if (err < 0)
		goto out;

	index = err;

	/* We need to know how many segments before we allocate. */
	err = virtio_config_val(vdev, VIRTIO_BLK_F_SEG_MAX,
				offsetof(struct virtio_blk_config, seg_max),
				&sg_elems);

	/* We need at least one SG element, whatever they say. */
	if (err || !sg_elems)
		sg_elems = 1;

	/* We need an extra sg elements at head and tail. */
	sg_elems += 2;
	vdev->priv = vblk = kmalloc(sizeof(*vblk) +
				    sizeof(vblk->sg[0]) * sg_elems, PAF_KERNEL);
	if (!vblk) {
		err = -ENOMEM;
		goto out_free_index;
	}

	init_waitqueue(&vblk->queue_wait);
	vblk->vdev = vdev;
	vblk->sg_elems = sg_elems;
	sg_init_table(vblk->sg, vblk->sg_elems);

	err = init_vq(vblk);
	if (err)
		goto out_free_vblk;

	pool_size = sizeof(struct virtblk_req);
	vblk->mem_cache = beehive_create("virtblk_req_cache",
					 pool_size,
					 0,
					 BEEHIVE_RECLAIM_ABLE|BEEHIVE_PANIC,
					 NULL);
	if (!vblk->mem_cache) {
		err = -ENOMEM;
		goto out_free_vq;
	}

	/* FIXME: How many partitions?  How long is a piece of string? */
	vblk->disk = alloc_disk(1 << PART_BITS);
	if (!vblk->disk) {
		err = -ENOMEM;
		goto out_mempool;
	}

	q = vblk->disk->queue = blk_create_queue(virtblk_request, NULL);
	if (!q) {
		err = -ENOMEM;
		goto out_put_disk;
	}

	q->queuedata = vblk;

	virtblk_name_format("vd", index, vblk->disk->name, DISK_NAME_LEN);
	printk("xby_debug in virtblk_probe, %s\n", vblk->disk->name);

	memcpy(vblk->disk->sysobj_name, vblk->disk->name, DISK_NAME_LEN);
	vblk->disk->private_data = vblk;
	vblk->disk->fops = &virtblk_fops;
	vblk->index = index;

	/* If disk is read-only in the host, the guest should obey */
	if (virtio_has_feature(vdev, VIRTIO_BLK_F_RO))
		set_disk_readonly(vblk->disk, 1);

	/* Host must always specify the capacity. */
	vdev->config->get(vdev, offsetof(struct virtio_blk_config, capacity),
			  &cap, sizeof(cap));

	/* If capacity is too big, truncate with warning. */
	if ((sector_t)cap != cap) {
		printk("Capacity %llu too large: truncating\n",
			 (unsigned long long)cap);
		cap = (sector_t)-1;
	}
	set_capacity(vblk->disk, cap);

	/* We can handle whatever the host told us to handle. */
	blkdev_set_hw_max_pages(q, vblk->sg_elems-2);

	/* No need to bounce any requests */
	blk_set_dma_limit(q, BLKDEV_LIMIT_ANY);

	/* No real sector limit. */
	blk_set_request_max_sectors(q, -1U);

	/* Host can optionally specify maximum segment size and number of
	 * segments. */
	err = virtio_config_val(vdev, VIRTIO_BLK_F_SIZE_MAX,
				offsetof(struct virtio_blk_config, size_max),
				&v);
	if (!err)
		blk_set_request_max_size(q, v);
	else
		blk_set_request_max_size(q, -1U);

	/* Host can optionally specify the block size of the device */
	err = virtio_config_val(vdev, VIRTIO_BLK_F_BLK_SIZE,
				offsetof(struct virtio_blk_config, blk_size),
				&blk_size);
#if 0
	if (!err)
		blk_queue_logical_block_size(q, blk_size);
	else
		blk_size = queue_logical_block_size(q);
#endif

	/* Use topology information if available */
	err = virtio_config_val(vdev, VIRTIO_BLK_F_TOPOLOGY,
			offsetof(struct virtio_blk_config, physical_block_exp),
			&physical_block_exp);
#if 0
	if (!err && physical_block_exp)
		blk_queue_physical_block_size(q,
				blk_size * (1 << physical_block_exp));
#endif

	err = virtio_config_val(vdev, VIRTIO_BLK_F_TOPOLOGY,
			offsetof(struct virtio_blk_config, alignment_offset),
			&alignment_offset);
#if 0
	if (!err && alignment_offset)
		blk_queue_alignment_offset(q, blk_size * alignment_offset);
#endif

	err = virtio_config_val(vdev, VIRTIO_BLK_F_TOPOLOGY,
			offsetof(struct virtio_blk_config, min_io_size),
			&min_io_size);
#if 0
	if (!err && min_io_size)
		blk_queue_io_min(q, blk_size * min_io_size);
#endif

	err = virtio_config_val(vdev, VIRTIO_BLK_F_TOPOLOGY,
			offsetof(struct virtio_blk_config, opt_io_size),
			&opt_io_size);
#if 0
	if (!err && opt_io_size)
		blk_queue_io_opt(q, blk_size * opt_io_size);
#endif

	add_disk(vblk->disk);

	if (err)
		goto out_del_disk;


test_vblk = vblk;
	return 0;

out_del_disk:
	delete_disk(vblk->disk);
	blk_loosen_queue(vblk->disk->queue);
out_put_disk:
	loosen_disk(vblk->disk);
out_mempool:
	beehive_destroy(vblk->mem_cache);
out_free_vq:
	vdev->config->del_vqs(vdev);
out_free_vblk:
	kfree(vblk);
out_free_index:
	idr_remove(&vd_index_ida, index);
out:
	return err;
}

static void virtblk_remove(struct virtio_device *vdev)
{
	struct virtio_blk *vblk = vdev->priv;
	//int index = vblk->index;
	//int refc;

	delete_disk(vblk->disk);
	blk_loosen_queue(vblk->disk->queue);

	/* Stop all the virtqueues. */
	vdev->config->reset(vdev);

	//refc = accurate_read(&disk_to_dev(vblk->disk)->kobj.kref.refcount);
	loosen_disk(vblk->disk);
	beehive_destroy(vblk->mem_cache);
	vdev->config->del_vqs(vdev);
	kfree(vblk);

#if 0
	/* Only free device id if we don't have any users */
	if (refc == 1)
		ida_simple_remove(&vd_index_ida, index);
#endif
}

static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_BLOCK, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features[] = {
	VIRTIO_BLK_F_SEG_MAX, VIRTIO_BLK_F_SIZE_MAX, VIRTIO_BLK_F_GEOMETRY,
	VIRTIO_BLK_F_RO, VIRTIO_BLK_F_BLK_SIZE, VIRTIO_BLK_F_SCSI,
	VIRTIO_BLK_F_WCE, VIRTIO_BLK_F_TOPOLOGY, VIRTIO_BLK_F_CONFIG_WCE
};

static struct virtio_driver virtio_blk = {
	.feature_table		= features,
	.feature_table_size	= ARRAY_SIZE(features),
	.driver.name		= "virtio_blk",
	.id_table		= id_table,
	.probe			= virtblk_probe,
	.remove			= virtblk_remove,
};

int __init virtio_blk_init(void)
{
	int error;

	error = register_virtio_driver(&virtio_blk);

	return error;
}
