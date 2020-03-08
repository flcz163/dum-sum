#include <dim-sum/beehive.h>
#include <dim-sum/err.h>
#include <dim-sum/irq.h>
#include <dim-sum/irq_mapping.h>
#include <dim-sum/mm.h>
#include <dim-sum/smp_lock.h>
#include <dim-sum/virtio.h>
#include <dim-sum/virtio_config.h>
#include <dim-sum/virtio_mmio.h>
#include <dim-sum/virtio_ring.h>

#include <asm/io.h>
#include <asm/page.h>

/* The alignment to use between consumer and producer parts of vring.
 * Currently hardcoded to the page size. */
#define VIRTIO_MMIO_VRING_ALIGN		PAGE_SIZE

#define to_virtio_mmio_device(_plat_dev) \
	container_of(_plat_dev, struct virtio_mmio_device, vdev)

struct virtio_mmio_device {
	struct virtio_device vdev;
	struct platform_device *pdev;

	void __iomem *base;
	unsigned long version;

	/* a list of queues so we can dispatch IRQs */
	struct smp_lock lock;
	struct double_list virtqueues;
};

struct virtio_mmio_vq_info {
	/* the actual virtqueue */
	struct virtqueue *vq;

	/* the number of entries in the queue */
	unsigned int num;

	/* the virtual address of the ring queue */
	void *queue;

	/* the list node for the virtqueues list */
	struct double_list node;
};

/* Configuration interface */

static u32 vm_get_features(struct virtio_device *vdev)
{
	struct virtio_mmio_device *vm_dev = to_virtio_mmio_device(vdev);

	/* TODO: Features > 32 bits */
	writel(0, vm_dev->base + VIRTIO_MMIO_HOST_FEATURES_SEL);

	return readl(vm_dev->base + VIRTIO_MMIO_HOST_FEATURES);
}

static void vm_finalize_features(struct virtio_device *vdev)
{
	struct virtio_mmio_device *vm_dev = to_virtio_mmio_device(vdev);
	int i;

	/* Give virtio_ring a chance to accept features. */
	vring_transport_features(vdev);

	for (i = 0; i < ARRAY_SIZE(vdev->features); i++) {
		writel(i, vm_dev->base + VIRTIO_MMIO_GUEST_FEATURES_SEL);
		writel(vdev->features[i],
				vm_dev->base + VIRTIO_MMIO_GUEST_FEATURES);
	}
}

static void vm_get(struct virtio_device *vdev, unsigned offset,
		   void *buf, unsigned len)
{
	struct virtio_mmio_device *vm_dev = to_virtio_mmio_device(vdev);
	u8 *ptr = buf;
	int i;

	for (i = 0; i < len; i++)
		ptr[i] = readb(vm_dev->base + VIRTIO_MMIO_CONFIG + offset + i);
}

static void vm_set(struct virtio_device *vdev, unsigned offset,
		   const void *buf, unsigned len)
{
	struct virtio_mmio_device *vm_dev = to_virtio_mmio_device(vdev);
	const u8 *ptr = buf;
	int i;

	for (i = 0; i < len; i++)
		writeb(ptr[i], vm_dev->base + VIRTIO_MMIO_CONFIG + offset + i);
}

static u8 vm_get_status(struct virtio_device *vdev)
{
	struct virtio_mmio_device *vm_dev = to_virtio_mmio_device(vdev);

	return readl(vm_dev->base + VIRTIO_MMIO_STATUS) & 0xff;
}

static void vm_set_status(struct virtio_device *vdev, u8 status)
{
	struct virtio_mmio_device *vm_dev = to_virtio_mmio_device(vdev);

	/* We should never be setting status to 0. */
	BUG_ON(status == 0);

	writel(status, vm_dev->base + VIRTIO_MMIO_STATUS);
}

static void vm_reset(struct virtio_device *vdev)
{
	struct virtio_mmio_device *vm_dev = to_virtio_mmio_device(vdev);

	/* 0 status means a reset. */
	writel(0, vm_dev->base + VIRTIO_MMIO_STATUS);
}



/* Transport interface */

/* the notify function used when creating a virt queue */
static void vm_notify(struct virtqueue *vq)
{
	struct virtio_mmio_device *vm_dev = to_virtio_mmio_device(vq->vdev);

	/* We write the queue's selector into the notification register to
	 * signal the other end */
	writel(vq->index, vm_dev->base + VIRTIO_MMIO_QUEUE_NOTIFY);
}

/* Notify all virtqueues on an interrupt. */
static enum isr_result vm_interrupt(unsigned int irq, void *opaque)
{
	struct virtio_mmio_device *vm_dev = opaque;
	struct virtio_mmio_vq_info *info;
	struct virtio_driver *vdrv = container_of(vm_dev->vdev.dev.driver,
			struct virtio_driver, driver);
	unsigned long status;
	unsigned long flags;
	enum isr_result ret = ISR_SKIP;

	/* Read and acknowledge interrupts */
	status = readl(vm_dev->base + VIRTIO_MMIO_INTERRUPT_STATUS);
	writel(status, vm_dev->base + VIRTIO_MMIO_INTERRUPT_ACK);

	if (unlikely(status & VIRTIO_MMIO_INT_CONFIG)
			&& vdrv && vdrv->config_changed) {
		vdrv->config_changed(&vm_dev->vdev);
		ret = ISR_EATEN;
	}

	if (likely(status & VIRTIO_MMIO_INT_VRING)) {
		smp_lock_irqsave(&vm_dev->lock, flags);
		list_for_each_entry(info, &vm_dev->virtqueues, node)
			ret |= vring_interrupt(irq, info->vq);
		smp_unlock_irqrestore(&vm_dev->lock, flags);
	}

	return ret;
}



static void vm_del_vq(struct virtqueue *vq)
{
	struct virtio_mmio_device *vm_dev = to_virtio_mmio_device(vq->vdev);
	struct virtio_mmio_vq_info *info = vq->priv;
	unsigned long flags, size;
	unsigned int index = vq->index;

	smp_lock_irqsave(&vm_dev->lock, flags);
	list_del(&info->node);
	smp_unlock_irqrestore(&vm_dev->lock, flags);

	vring_del_virtqueue(vq);

	/* Select and deactivate the queue */
	writel(index, vm_dev->base + VIRTIO_MMIO_QUEUE_SEL);
	writel(0, vm_dev->base + VIRTIO_MMIO_QUEUE_PFN);

	size = PAGE_ALIGN(vring_size(info->num, VIRTIO_MMIO_VRING_ALIGN));
	free_pages_memory((unsigned long)info->queue, size);
	kfree(info);
}

static void vm_del_vqs(struct virtio_device *vdev)
{
	struct virtio_mmio_device *vm_dev = to_virtio_mmio_device(vdev);
	struct virtqueue *vq, *n;

	list_for_each_entry_safe(vq, n, &vdev->vqs, list)
		vm_del_vq(vq);

	unregister_isr_handle(platform_get_irq(vm_dev->pdev, 0), vm_dev);
}



static struct virtqueue *vm_setup_vq(struct virtio_device *vdev, unsigned index,
				  void (*callback)(struct virtqueue *vq),
				  const char *name)
{
	struct virtio_mmio_device *vm_dev = to_virtio_mmio_device(vdev);
	struct virtio_mmio_vq_info *info;
	struct virtqueue *vq;
	unsigned long flags, size;
	int err;

	if (!name)
		return NULL;

	/* Select the queue we're interested in */
	writel(index, vm_dev->base + VIRTIO_MMIO_QUEUE_SEL);

	/* Queue shouldn't already be set up. */
	if (readl(vm_dev->base + VIRTIO_MMIO_QUEUE_PFN)) {
		err = -ENOENT;
		goto error_available;
	}

	/* Allocate and fill out our active queue description */
	info = kmalloc(sizeof(*info), PAF_KERNEL);
	if (!info) {
		err = -ENOMEM;
		goto error_kmalloc;
	}

	/* Allocate pages for the queue - start with a queue as big as
	 * possible (limited by maximum size allowed by device), drop down
	 * to a minimal size, just big enough to fit descriptor table
	 * and two rings (which makes it "alignment_size * 2")
	 */
	info->num = readl(vm_dev->base + VIRTIO_MMIO_QUEUE_NUM_MAX);

	/* If the device reports a 0 entry queue, we won't be able to
	 * use it to perform I/O, and vring_new_virtqueue() can't create
	 * empty queues anyway, so don't bother to set up the device.
	 */
	if (info->num == 0) {
		err = -ENOENT;
		goto error_alloc_pages;
	}

	while (1) {
		size = PAGE_ALIGN(vring_size(info->num,
				VIRTIO_MMIO_VRING_ALIGN));
		/* Did the last iter shrink the queue below minimum size? */
		if (size < VIRTIO_MMIO_VRING_ALIGN * 2) {
			err = -ENOMEM;
			goto error_alloc_pages;
		}

		info->queue = (void *)alloc_pages_memory(PAF_KERNEL | __PAF_ZERO, get_order(size));
		if (info->queue)
			break;

		info->num /= 2;
	}

	/* Activate the queue */
	writel(info->num, vm_dev->base + VIRTIO_MMIO_QUEUE_NUM);
	writel(VIRTIO_MMIO_VRING_ALIGN,
			vm_dev->base + VIRTIO_MMIO_QUEUE_ALIGN);
	writel(linear_virt_to_phys(info->queue) >> PAGE_SHIFT,
			vm_dev->base + VIRTIO_MMIO_QUEUE_PFN);

	/* Create the vring */
	vq = vring_new_virtqueue(index, info->num, VIRTIO_MMIO_VRING_ALIGN, vdev,
				 true, info->queue, vm_notify, callback, name);
	
	if (!vq) {
		err = -ENOMEM;
		goto error_new_virtqueue;
	}

	vq->priv = info;
	info->vq = vq;

	smp_lock_irqsave(&vm_dev->lock, flags);
	list_insert_front(&info->node, &vm_dev->virtqueues);
	smp_unlock_irqrestore(&vm_dev->lock, flags);

	return vq;

error_new_virtqueue:
	writel(0, vm_dev->base + VIRTIO_MMIO_QUEUE_PFN);
	free_pages_memory((unsigned long)info->queue, get_order(size));
error_alloc_pages:
	kfree(info);
error_kmalloc:
error_available:
	return ERR_PTR(err);
}

static int vm_find_vqs(struct virtio_device *vdev, unsigned nvqs,
		       struct virtqueue *vqs[],
		       vq_callback_t *callbacks[],
		       const char *names[])
{
	struct virtio_mmio_device *vm_dev = to_virtio_mmio_device(vdev);
	unsigned int irq = platform_get_irq(vm_dev->pdev, 0);
	int i, err;

	err = register_isr_handle(irq, vm_interrupt, IRQFLAG_SHARED,
			dev_name(&vdev->dev), vm_dev);
	if (err)
		return err;
	
	for (i = 0; i < nvqs; ++i) {
		vqs[i] = vm_setup_vq(vdev, i, callbacks[i], names[i]);
		if (IS_ERR(vqs[i])) {
			vm_del_vqs(vdev);
			return PTR_ERR(vqs[i]);
		}
	}

	return 0;
}

static const char *vm_bus_name(struct virtio_device *vdev)
{
	struct virtio_mmio_device *vm_dev = to_virtio_mmio_device(vdev);

	return vm_dev->pdev->name;
}

static const struct virtio_config_ops virtio_mmio_config_ops = {
	.get		= vm_get,
	.set		= vm_set,
	.get_status	= vm_get_status,
	.set_status	= vm_set_status,
	.reset		= vm_reset,
	.find_vqs	= vm_find_vqs,
	.del_vqs	= vm_del_vqs,
	.get_features	= vm_get_features,
	.finalize_features = vm_finalize_features,
	.bus_name	= vm_bus_name,
};

static int virtio_mmio_setup(int irq, phys_addr_t start, phys_addr_t end)
{
	struct virtio_mmio_device *vm_dev;
	unsigned long magic;
	struct platform_device *pdev;

	pdev = kzalloc(sizeof(*pdev), PAF_KERNEL);
	if (!pdev)
		return  -ENOMEM;
	pdev->irq = irq;

	vm_dev = kzalloc(sizeof(*vm_dev), PAF_KERNEL);
	if (!vm_dev)
		return  -ENOMEM;
	
	//vm_dev->vdev.dev.parent = &pdev->dev;
	vm_dev->vdev.config = &virtio_mmio_config_ops;
	vm_dev->pdev = pdev;
	list_init(&vm_dev->virtqueues);
	smp_lock_init(&vm_dev->lock);

	vm_dev->base = (void *)start;
	vm_dev->vdev.base = (void *)start;
	if (vm_dev->base == NULL)
		return -EFAULT;

	/* Check magic value */
	magic = readl(vm_dev->base + VIRTIO_MMIO_MAGIC_VALUE);
	if (memcmp(&magic, "virt", 4) != 0) {
		return -ENODEV;
	}

	/* Check device version */
	vm_dev->version = readl(vm_dev->base + VIRTIO_MMIO_VERSION);
	if (vm_dev->version != 1) {
		return -ENXIO;
	}

	vm_dev->vdev.id.device = readl(vm_dev->base + VIRTIO_MMIO_DEVICE_ID);
	if (vm_dev->vdev.id.device == 0)
		return -ENODEV;

	vm_dev->vdev.id.vendor = readl(vm_dev->base + VIRTIO_MMIO_VENDOR_ID);

	printk("xby_debug in virtio_mmio_setup, %d, %x.\n",
		vm_dev->vdev.id.device,
		vm_dev->vdev.id.vendor);
	writel(PAGE_SIZE, vm_dev->base + VIRTIO_MMIO_GUEST_PAGE_SIZE);

	platform_set_drvdata(pdev, vm_dev);

	return register_virtio_device(&vm_dev->vdev);
}

/* Platform device */

static int virtio_mmio_probe(struct platform_device *pdev)
{
/* TO-DO */
#if 0
	struct virtio_mmio_device *vm_dev;
	struct resource *mem;
	unsigned long magic;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem)
		return -EINVAL;

	if (!devm_request_mem_region(&pdev->dev, mem->start,
			resource_size(mem), pdev->name))
		return -EBUSY;

	vm_dev = devm_kzalloc(&pdev->dev, sizeof(*vm_dev), PAF_KERNEL);
	if (!vm_dev)
		return  -ENOMEM;

	vm_dev->vdev.dev.parent = &pdev->dev;
	vm_dev->vdev.config = &virtio_mmio_config_ops;
	vm_dev->pdev = pdev;
	list_init(&vm_dev->virtqueues);
	smp_lock_init(&vm_dev->lock);

	vm_dev->base = devm_ioremap(&pdev->dev, mem->start, resource_size(mem));
	if (vm_dev->base == NULL)
		return -EFAULT;

	/* Check magic value */
	magic = readl(vm_dev->base + VIRTIO_MMIO_MAGIC_VALUE);
	if (memcmp(&magic, "virt", 4) != 0) {
		dev_warn(&pdev->dev, "Wrong magic value 0x%08lx!\n", magic);
		return -ENODEV;
	}

	/* Check device version */
	vm_dev->version = readl(vm_dev->base + VIRTIO_MMIO_VERSION);
	if (vm_dev->version != 1) {
		dev_err(&pdev->dev, "Version %ld not supported!\n",
				vm_dev->version);
		return -ENXIO;
	}

	vm_dev->vdev.id.device = readl(vm_dev->base + VIRTIO_MMIO_DEVICE_ID);
	vm_dev->vdev.id.vendor = readl(vm_dev->base + VIRTIO_MMIO_VENDOR_ID);

	writel(PAGE_SIZE, vm_dev->base + VIRTIO_MMIO_GUEST_PAGE_SIZE);

	platform_set_drvdata(pdev, vm_dev);

	return register_virtio_device(&vm_dev->vdev);
#endif
	return 0;
}

static int virtio_mmio_remove(struct platform_device *pdev)
{
	struct virtio_mmio_device *vm_dev = platform_get_drvdata(pdev);

	unregister_virtio_device(&vm_dev->vdev);

	return 0;
}

static struct platform_driver virtio_mmio_driver = {
	.probe		= virtio_mmio_probe,
	.remove		= virtio_mmio_remove,
};

static struct irq_configure virtio_mmio_of_desc[] =
	{
		[0] = {3, {0, 47, 1}, NULL},
		[1] = {3, {0, 46, 1}, NULL},
	};

int __init virtio_mmio_init(void)
{
	unsigned long base = (unsigned long)ioremap(0xa000000, 16 * 1024);
	
	platform_driver_register(&virtio_mmio_driver);

	init_one_hwirq(&virtio_mmio_of_desc[0]);
	init_one_hwirq(&virtio_mmio_of_desc[1]);

	/* Õ¯ø® */
	virtio_mmio_setup(78, base + 0x3c00, base  + 0x3c00 + 0x1ff);
	/* ¥≈≈Ã */
	virtio_mmio_setup(79, base + 0x3e00, base  + 0x3e00 + 0x1ff);

	return 0;
}

