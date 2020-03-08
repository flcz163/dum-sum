#include <dim-sum/beehive.h>
#include <dim-sum/delay.h>
#include <dim-sum/netdev.h>
#include <dim-sum/virtio.h>
#include <dim-sum/virtio_net.h>
#include <dim-sum/init.h>
#include <dim-sum/scatterlist.h>
#include <dim-sum/workqueue.h>
#include <dim-sum/mutex.h>
#include <dim-sum/percpu.h>
#include <lwip/inet.h>

#include <asm/page.h>

#if (65536/PAGE_SIZE + 1) < 16
#define MAX_SKB_FRAGS 16UL
#else
#define MAX_SKB_FRAGS (65536/PAGE_SIZE + 1)
#endif

#define VLAN_HLEN	4		/* The additional bytes required by VLAN
					 * (in addition to the Ethernet header)
					 */

#define MAX_PACKET_LEN (ETH_HLEN + VLAN_HLEN + ETH_DATA_LEN)

struct virtnet_stats {
	u64 tx_bytes;
	u64 tx_packets;

	u64 rx_bytes;
	u64 rx_packets;
};

/* Internal representation of a send virtqueue */
struct send_queue {
	/* Virtqueue associated with this send _queue */
	struct virtqueue *vq;

	/* TX: fragments + linear part + virtio header */
	struct scatterlist sg[MAX_SKB_FRAGS + 2];

	/* Name of the send queue: output.$index */
	char name[40];
};

/* Internal representation of a receive virtqueue */
struct receive_queue {
	/* Virtqueue associated with this receive_queue */
	struct virtqueue *vq;

	//struct napi_struct napi;

	/* Number of input buffers, and max we've ever had. */
	unsigned int num, max;

	/* Chain pages by the private ptr. */
	struct page_frame *pages;

	/* RX: fragments + linear part + virtio header */
	struct scatterlist sg[MAX_SKB_FRAGS + 2];

	/* Name of this receive queue: input.$index */
	char name[40];
};

struct virtnet_info {
	struct virtio_device *vdev;
	struct virtqueue *cvq;
	struct send_queue *sq;
	struct receive_queue *rq;
	unsigned int status;

	char mac[ETH_ALEN];

	/* Max # of queue pairs supported by the device */
	u16 max_queue_pairs;

	/* # of queue pairs currently used by the driver */
	u16 curr_queue_pairs;

	/* I like... big packets and I cannot lie! */
	bool big_packets;

	/* Host will merge rx buffers for big packets (shake it! shake it!) */
	bool mergeable_rx_bufs;

	/* Has control virtqueue */
	bool has_cvq;

	/* Host can handle any s/g split between our header and packet data */
	bool any_header_sg;

	/* enable config space updates */
	bool config_enable;

	/* Active statistics */
	struct virtnet_stats __percpu *stats;

	/* Work struct for refilling if we run low on memory. */
	struct work_struct refill;

	/* Work struct for config space updates */
	struct work_struct config_work;

	/* Lock for config space updates */
	struct mutex config_lock;

	/* Does the affinity hint is set for virtqueues? */
	bool affinity_hint_set;

	/* Per-cpu variable to show the space from CPU to virtqueue */
	int __percpu *vq_index;
	struct smp_lock packet_lock;
	struct double_list packet_head;
	struct dim_sum_netdev *netdev;
};


struct padded_vnet_hdr {
	struct virtio_net_hdr hdr;
	/*
	 * virtio_net_hdr should be in a separated sg buffer because of a
	 * QEMU bug, and data sg buffer shares same page with this header sg.
	 * This padding makes next sg 16 byte aligned after virtio_net_hdr.
	 */
	char padding[6];
};

struct skb_vnet_hdr {
	union {
		struct virtio_net_hdr hdr;
		struct virtio_net_hdr_mrg_rxbuf mhdr;
	};
};

struct virtnet_packet {
	char buf[MAX_PACKET_LEN];
	int len;
	int cur;
	struct skb_vnet_hdr hdr;
	struct double_list list;
};


static void virtnet_config_changed_work(void *data)
{
	struct virtnet_info *vi = (struct virtnet_info *)data;
	
	u16 v;

	mutex_lock(&vi->config_lock);
	if (!vi->config_enable)
		goto done;

	if (virtio_config_val(vi->vdev, VIRTIO_NET_F_STATUS,
			      offsetof(struct virtio_net_config, status),
			      &v) < 0)
		goto done;

	if (v & VIRTIO_NET_S_ANNOUNCE) {
		//netdev_notify_peers(vi->dev);
		//virtnet_ack_link_announce(vi);
	}

	/* Ignore unknown (future) status bits */
	v &= VIRTIO_NET_S_LINK_UP;

	if (vi->status == v)
		goto done;

	vi->status = v;

	if (vi->status & VIRTIO_NET_S_LINK_UP) {
		//netif_carrier_on(vi->dev);
		//netif_tx_wake_all_queues(vi->dev);
	} else {
		//netif_carrier_off(vi->dev);
		//netif_tx_stop_all_queues(vi->dev);
	}
done:
	mutex_unlock(&vi->config_lock);
}

static struct page_frame *get_a_page(struct receive_queue *rq, paf_t paf_mask)
{
	struct page_frame *p = rq->pages;

	if (p) {
		rq->pages = (struct page_frame *)p->private;
		/* clear private here, it is used to chain pages */
		p->private = 0;
	} else
		p = alloc_page_frame(paf_mask);
	return p;
}

static int add_recvbuf_small(struct receive_queue *rq, paf_t paf)
{
	//struct virtnet_info *vi = rq->vq->vdev->priv;
	struct virtnet_packet *packet;
	int err;
	struct skb_vnet_hdr *hdr;

	packet = kmalloc(sizeof(*packet), paf);
	if (unlikely(!packet))
		return -ENOMEM;

#if 0
	hdr = kmalloc(sizeof hdr->hdr, paf);
	sg_set_buf(rq->sg, &hdr->hdr, sizeof hdr->hdr);
	
	sg_set_buf(rq->sg + 1, skb, MAX_PACKET_LEN);

	err = virtqueue_add_inbuf(rq->vq, rq->sg, 2, skb, paf);
#else
	memset(packet, 0, sizeof(*packet));
	list_init(&packet->list);
	sg_set_buf(rq->sg, &packet->hdr, sizeof hdr->hdr);
	sg_set_buf(rq->sg + 1, packet->buf, MAX_PACKET_LEN);

	err = virtqueue_add_inbuf(rq->vq, rq->sg, 2, packet, paf);
#endif

	if (err < 0)
		kfree(packet);

	return err;
}

/*
 * Returns false if we couldn't fill entirely (OOM).
 *
 * Normally run in the receive path, but can also be run from ndo_open
 * before we're receiving packets, or from refill_work which is
 * careful to disable receiving (using napi_disable).
 */
static bool try_fill_recv(struct receive_queue *rq, paf_t paf)
{
	//struct virtnet_info *vi = rq->vq->vdev->priv;
	int err;
	bool oom;

	do {
		err = add_recvbuf_small(rq, paf);

		oom = err == -ENOMEM;
		if (err)
			break;
		++rq->num;
	} while (rq->vq->num_free && 0);
	if (unlikely(rq->num > rq->max))
		rq->max = rq->num;
	virtqueue_kick(rq->vq);
	return !oom;
}

static void refill_work(void *data)
{
	struct virtnet_info *vi = (struct virtnet_info *)data;
	bool still_empty;
	int i;

	for (i = 0; i < vi->curr_queue_pairs; i++) {
		struct receive_queue *rq = &vi->rq[i];

		//napi_disable(&rq->napi);
		still_empty = !try_fill_recv(rq, PAF_KERNEL);
		//virtnet_napi_enable(rq);

		/* In theory, this can happen: if we don't get any buffers in
		 * we will *never* try to fill again.
		 */
		if (still_empty)
			schedule_delayed_work(&vi->refill, HZ/2);
	}
}

static int virtnet_alloc_queues(struct virtnet_info *vi)
{
	int i;

	vi->sq = kzalloc(sizeof(*vi->sq) * vi->max_queue_pairs, PAF_KERNEL);
	if (!vi->sq)
		goto err_sq;
	vi->rq = kzalloc(sizeof(*vi->rq) * vi->max_queue_pairs, PAF_KERNEL);
	if (!vi->rq)
		goto err_rq;

	INIT_WORK(&vi->refill, refill_work, vi);
	for (i = 0; i < vi->max_queue_pairs; i++) {
		vi->rq[i].pages = NULL;

		sg_init_table(vi->rq[i].sg, ARRAY_SIZE(vi->rq[i].sg));
		sg_init_table(vi->sq[i].sg, ARRAY_SIZE(vi->sq[i].sg));
	}

	return 0;

err_rq:
	kfree(vi->sq);
err_sq:
	return -ENOMEM;
}


static int vq2rxq(struct virtqueue *vq)
{
	return vq->index / 2;
}

static int rxq2vq(int rxq)
{
	return rxq * 2;
}

static int txq2vq(int txq)
{
	return txq * 2 + 1;
}

static void receive_buf(struct virtnet_info *vi, struct receive_queue *rq,
			void *buf, unsigned int len)
{
	//struct dim_sum_netdev *dev = vi->netdev;
	//struct virtnet_stats *stats = this_cpu_ptr(vi->stats);
	//struct page *page;
	//struct skb_vnet_hdr *hdr;
	//char *skb;
	//char cb[48];
	struct virtnet_packet *packet;
	unsigned long flags;

	if (unlikely(len < sizeof(struct virtio_net_hdr) + ETH_HLEN)) {
		//dev->stats.rx_length_errors++;
		kfree(buf);
		return;
	}

	//skb = buf;
	//len -= sizeof(struct virtio_net_hdr);
	//skb = buf + sizeof(struct virtio_net_hdr);

	//hdr = cb;

	packet = buf;//kmalloc(sizeof(*packet), PAF_ATOMIC);
	if (!packet) {
		kfree(buf);
		return;
	}

	list_init(&packet->list);
	//packet->buf = skb;
	packet->len = len;
	packet->cur = 0;
	smp_lock_irqsave(&vi->packet_lock, flags);
	list_insert_behind(&packet->list, &vi->packet_head);
	smp_unlock_irqrestore(&vi->packet_lock, flags);

	return;
}

static int virtnet_receive(struct receive_queue *rq)
{
	struct virtnet_info *vi = rq->vq->vdev->priv;
	unsigned int len, received = 0;
	void *buf;

	while ((buf = virtqueue_get_buf(rq->vq, &len)) != NULL) {
		receive_buf(vi, rq, buf, len);
		received++;
	}

	if (!try_fill_recv(rq, PAF_ATOMIC))
		schedule_delayed_work(&vi->refill, 0);

	return received;
}

static void skb_recv_done(struct virtqueue *rvq)
{
	struct virtnet_info *vi = rvq->vdev->priv;
	struct receive_queue *rq = &vi->rq[vq2rxq(rvq)];

	/* Schedule NAPI, Suppress further interrupts if successful. */
	//virtqueue_disable_cb(rvq);
	virtnet_receive(rq);
}


static void skb_xmit_done(struct virtqueue *vq)
{
	//struct virtnet_info *vi = vq->vdev->priv;

	/* Suppress further interrupts. */
	//virtqueue_disable_cb(vq);

	/* We were probably waiting for more output buffers. */
	//netif_wake_subqueue(vi->dev, vq2txq(vq));
}


static int virtnet_find_vqs(struct virtnet_info *vi)
{
	vq_callback_t **callbacks;
	struct virtqueue **vqs;
	int ret = -ENOMEM;
	int i, total_vqs;
	const char **names;

	/* We expect 1 RX virtqueue followed by 1 TX virtqueue, followed by
	 * possible N-1 RX/TX queue pairs used in multiqueue mode, followed by
	 * possible control vq.
	 */
	total_vqs = vi->max_queue_pairs * 2 +
		    virtio_has_feature(vi->vdev, VIRTIO_NET_F_CTRL_VQ);

	/* Allocate space for find_vqs parameters */
	vqs = kzalloc(total_vqs * sizeof(*vqs), PAF_KERNEL);
	if (!vqs)
		goto err_vq;
	callbacks = kmalloc(total_vqs * sizeof(*callbacks), PAF_KERNEL);
	if (!callbacks)
		goto err_callback;
	names = kmalloc(total_vqs * sizeof(*names), PAF_KERNEL);
	if (!names)
		goto err_names;

	/* Parameters for control virtqueue, if any */
	if (vi->has_cvq) {
		callbacks[total_vqs - 1] = NULL;
		names[total_vqs - 1] = "control";
	}

	/* Allocate/initialize parameters for send/receive virtqueues */
	for (i = 0; i < vi->max_queue_pairs; i++) {
		callbacks[rxq2vq(i)] = skb_recv_done;
		callbacks[txq2vq(i)] = skb_xmit_done;
		sprintf(vi->rq[i].name, "input.%d", i);
		sprintf(vi->sq[i].name, "output.%d", i);
		names[rxq2vq(i)] = vi->rq[i].name;
		names[txq2vq(i)] = vi->sq[i].name;
	}

	ret = vi->vdev->config->find_vqs(vi->vdev, total_vqs, vqs, callbacks,
					 names);
	if (ret)
		goto err_find;

	if (vi->has_cvq) {
		vi->cvq = vqs[total_vqs - 1];
#if 0
		if (virtio_has_feature(vi->vdev, VIRTIO_NET_F_CTRL_VLAN))
			vi->dev->features |= NETIF_F_HW_VLAN_CTAG_FILTER;
#endif
	}

	for (i = 0; i < vi->max_queue_pairs; i++) {
		vi->rq[i].vq = vqs[rxq2vq(i)];
		vi->sq[i].vq = vqs[txq2vq(i)];
	}

	kfree(names);
	kfree(callbacks);
	kfree(vqs);

	return 0;

err_find:
	kfree(names);
err_names:
	kfree(callbacks);
err_callback:
	kfree(vqs);
err_vq:
	return ret;
}


static void virtnet_free_queues(struct virtnet_info *vi)
{
	kfree(vi->rq);
	kfree(vi->sq);
}

static int init_vqs(struct virtnet_info *vi)
{
	int ret;

	/* Allocate send & receive queues */
	ret = virtnet_alloc_queues(vi);
	if (ret)
		goto err;

	ret = virtnet_find_vqs(vi);
	if (ret)
		goto err_free;

	//get_online_cpus();
	//virtnet_set_affinity(vi);
	//put_online_cpus();

	return 0;

err_free:
	virtnet_free_queues(vi);
err:
	return ret;
}


static void free_unused_bufs(struct virtnet_info *vi)
{
	void *buf;
	int i;

	for (i = 0; i < vi->max_queue_pairs; i++) {
		struct virtqueue *vq = vi->sq[i].vq;
		while ((buf = virtqueue_detach_unused_buf(vq)) != NULL)
			kfree(buf);
	}

	for (i = 0; i < vi->max_queue_pairs; i++) {
		struct virtqueue *vq = vi->rq[i].vq;

		while ((buf = virtqueue_detach_unused_buf(vq)) != NULL) {
			kfree(buf);
			--vi->rq[i].num;
		}
		BUG_ON(vi->rq[i].num != 0);
	}
}


static void free_receive_bufs(struct virtnet_info *vi)
{
	int i;

	for (i = 0; i < vi->max_queue_pairs; i++) {
		while (vi->rq[i].pages)
			free_page_frames(get_a_page(&vi->rq[i], PAF_KERNEL), 0);
	}
}


static void virtnet_del_vqs(struct virtnet_info *vi)
{
	struct virtio_device *vdev = vi->vdev;

	//virtnet_clean_affinity(vi, -1);

	vdev->config->del_vqs(vdev);

	virtnet_free_queues(vi);
}

static int xmit_skb(struct send_queue *sq, char *skb, int len)
{
	struct skb_vnet_hdr *hdr;
	struct virtnet_packet *packet;
	//struct virtnet_info *vi = sq->vq->vdev->priv;
	unsigned num_sg;
	unsigned hdr_len;
	//bool can_push;

	packet = kmalloc(sizeof(*packet), PAF_KERNEL);
	if (unlikely(!packet))
		return -ENOMEM;

	memset(packet, 0, sizeof(*packet));
	list_init(&packet->list);

	memcpy(packet->buf, skb, len);
	hdr_len = sizeof hdr->hdr;
	hdr = &packet->hdr;

	hdr->hdr.flags = 0;
	hdr->hdr.csum_offset = hdr->hdr.csum_start = 0;
	
	hdr->hdr.gso_type = VIRTIO_NET_HDR_GSO_NONE;
	hdr->hdr.gso_size = hdr->hdr.hdr_len = 0;

	sg_set_buf(sq->sg, hdr, hdr_len);
	sg_set_buf(sq->sg + 1, &packet->buf[0], len);
	num_sg = 2;

	return virtqueue_add_outbuf(sq->vq, sq->sg, num_sg, packet, PAF_ATOMIC);
}

static void free_old_xmit_skbs(struct send_queue *sq)
{
	struct virtnet_packet *packet;
	unsigned int len;
	//struct virtnet_info *vi = sq->vq->vdev->priv;
	
	while ((packet = virtqueue_get_buf(sq->vq, &len)) != NULL) {
		list_del_init(&packet->list);
		//kfree(packet);
	}
}

static int virtnet_send_pkt(struct dim_sum_netdev *netdev, void *packet, int length)
{
	struct virtnet_info *vi = netdev->priv;
	int qnum = 0;
	struct send_queue *sq = &vi->sq[qnum];
	int err;

	/* Free up any pending old buffers before queueing new ones. */
	free_old_xmit_skbs(sq);

	/* Try to transmit */
	err = xmit_skb(sq, packet, length);

	/* This should not happen! */
	if (unlikely(err)) {
		//kfree(skb);
		return -1;
	}
	virtqueue_kick(sq->vq);

	virtqueue_enable_cb_delayed(sq->vq);

	return 0;
}

static int virtnet_recv_pkt(struct dim_sum_netdev *netdev, void *skb, int *length)
{
	struct virtnet_info *priv = netdev->priv;
	int len;
	unsigned long flags;

	smp_lock_irqsave(&priv->packet_lock, flags);
	if (list_is_empty(&priv->packet_head)) {
		len = -1;
		smp_unlock_irqrestore(&priv->packet_lock, flags);
		msleep(1);
		return len;
	} else {
		struct virtnet_packet *packet = 
			list_first_container(&priv->packet_head, struct virtnet_packet, list);
		len = packet->len - packet->cur;
		if (len > 2048)
			len = 2048;
		memcpy(skb, packet->buf + packet->cur, len);
		*length = len;
		packet->cur += len;
		
		if (packet->cur >= packet->len) {
			//kfree(packet->buf);
			list_del_init(&packet->list);
			//kfree(packet);
		}
	}

	smp_unlock_irqrestore(&priv->packet_lock, flags);
	return len;
}
static int virtnet_initialize(struct dim_sum_netdev *netdev)
{
	return 0;
}

static void virtnet_halt_netdev(struct dim_sum_netdev *netdev)
{
}

static int virtnet_register(struct virtnet_info *priv)
{
	struct dim_sum_netdev *virtnet_device;
	//int idx = 0, i;
	struct in_addr tmpaddr;
	unsigned char mac_addr[6];

	mac_addr[0] = 0x52;
	mac_addr[1] = 0x54;
	mac_addr[2] = 0x00;
	mac_addr[3] = 0x4a;
	mac_addr[4] = 0x1e;
	mac_addr[5] = 0xd4;
	
	virtnet_device = kmalloc_app(sizeof(struct dim_sum_netdev));
	if (!virtnet_device) {
		printk("cpsw_net_init, ");
		return -1;
	}
	memset(virtnet_device, 0, sizeof(*virtnet_device));

	strcpy(virtnet_device->name, "virtio-net");
	virtnet_device->initialize = virtnet_initialize;
	virtnet_device->send_pkt = virtnet_send_pkt;
	virtnet_device->recv_pkt = virtnet_recv_pkt;
	virtnet_device->halt_netdev = virtnet_halt_netdev;
	memcpy(virtnet_device->enetaddr, mac_addr, sizeof(mac_addr));
	/** 初始化IP地址为10.0.0.88/24 **/
	inet_aton("10.0.0.88", &tmpaddr);
	virtnet_device->ipaddr.addr = tmpaddr.s_addr;
	inet_aton("255.255.255.0", &tmpaddr);
	virtnet_device->netmask.addr = tmpaddr.s_addr;

	virtnet_device->priv = priv;
	priv->netdev = virtnet_device;
	dim_sum_netdev_register(virtnet_device);

	return 1;

}

static int virtnet_probe(struct virtio_device *vdev)
{
	int i, err;
	struct virtnet_info *vi;
	u16 max_queue_pairs;

	/* Find if host supports multiqueue virtio_net device */
	err = virtio_config_val(vdev, VIRTIO_NET_F_MQ,
				offsetof(struct virtio_net_config,
				max_virtqueue_pairs), &max_queue_pairs);

	/* We need at least 2 queue's */
	if (err || max_queue_pairs < VIRTIO_NET_CTRL_MQ_VQ_PAIRS_MIN ||
	    max_queue_pairs > VIRTIO_NET_CTRL_MQ_VQ_PAIRS_MAX ||
	    !virtio_has_feature(vdev, VIRTIO_NET_F_CTRL_VQ))
		max_queue_pairs = 1;

	/* Set up our device-specific information */
	vi = kmalloc(sizeof(struct virtnet_info), PAF_KERNEL);
	
	/* Configuration may specify what MAC to use.  Otherwise random. */
	virtio_config_val_len(vdev, VIRTIO_NET_F_MAC,
				  offsetof(struct virtio_net_config, mac),
				  vi->mac, ETH_ALEN);
	list_init(&vi->packet_head);
	smp_lock_init(&vi->packet_lock);
	vi->vdev = vdev;
	vdev->priv = vi;
	vi->stats = alloc_percpu(struct virtnet_stats);
	err = -ENOMEM;
	if (vi->stats == NULL)
		goto free;

	vi->vq_index = alloc_percpu(int);
	if (vi->vq_index == NULL)
		goto free_stats;

	mutex_init(&vi->config_lock);
	vi->config_enable = true;
	INIT_WORK(&vi->config_work, virtnet_config_changed_work, vi);

#if 1
	/* If we can receive ANY GSO packets, we must allocate large ones. */
	if (virtio_has_feature(vdev, VIRTIO_NET_F_GUEST_TSO4) ||
	    virtio_has_feature(vdev, VIRTIO_NET_F_GUEST_TSO6) ||
	    virtio_has_feature(vdev, VIRTIO_NET_F_GUEST_ECN))
		vi->big_packets = true;

	if (virtio_has_feature(vdev, VIRTIO_NET_F_MRG_RXBUF))
		vi->mergeable_rx_bufs = true;
#endif

	if (virtio_has_feature(vdev, VIRTIO_F_ANY_LAYOUT))
		vi->any_header_sg = true;

	if (virtio_has_feature(vdev, VIRTIO_NET_F_CTRL_VQ))
		vi->has_cvq = true;

	/* Use single tx/rx queue pair as default */
	vi->curr_queue_pairs = 1;
	vi->max_queue_pairs = max_queue_pairs;

	/* Allocate/initialize the rx/tx queues, and invoke find_vqs */
	err = init_vqs(vi);
	if (err)
		goto free_index;

	/* Last of all, set up some receive buffers. */
	for (i = 0; i < vi->curr_queue_pairs; i++) {
		try_fill_recv(&vi->rq[i], PAF_KERNEL);

		/* If we didn't even get one input buffer, we're useless. */
		if (vi->rq[i].num == 0) {
			free_unused_bufs(vi);
			err = -ENOMEM;
			goto free_recv_bufs;
		}
	}

	virtnet_register(vi);
	
	return 0;

free_recv_bufs:
	free_receive_bufs(vi);
//free_vqs:
	cancel_delayed_work(&vi->refill);
	virtnet_del_vqs(vi);
free_index:
	free_percpu(vi->vq_index);
free_stats:
	free_percpu(vi->stats);
free:
	return err;
}


static void virtnet_remove(struct virtio_device *vdev)
{
}

static void virtnet_config_changed(struct virtio_device *vdev)
{
}

static unsigned int features[] = {
        VIRTIO_NET_F_CSUM, VIRTIO_NET_F_GUEST_CSUM,
        VIRTIO_NET_F_GSO, VIRTIO_NET_F_MAC,
        VIRTIO_NET_F_HOST_TSO4, VIRTIO_NET_F_HOST_UFO, VIRTIO_NET_F_HOST_TSO6,
        VIRTIO_NET_F_HOST_ECN, /*VIRTIO_NET_F_GUEST_TSO4, VIRTIO_NET_F_GUEST_TSO6,
        VIRTIO_NET_F_GUEST_ECN, */VIRTIO_NET_F_GUEST_UFO,
        /*VIRTIO_NET_F_MRG_RXBUF, */VIRTIO_NET_F_STATUS, VIRTIO_NET_F_CTRL_VQ,
        VIRTIO_NET_F_CTRL_RX, VIRTIO_NET_F_CTRL_VLAN,
        VIRTIO_NET_F_GUEST_ANNOUNCE, VIRTIO_NET_F_MQ,
        VIRTIO_NET_F_CTRL_MAC_ADDR,
        VIRTIO_F_ANY_LAYOUT,
};

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_NET, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver virtio_net_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name =	"virtio-net",
	.id_table =	id_table,
	.probe =	virtnet_probe,
	.remove =	virtnet_remove,
	.config_changed = virtnet_config_changed,
};

int __init virtio_net_driver_init(void)
{
	return register_virtio_driver(&virtio_net_driver);
}

__maybe_unused static void virtio_net_driver_exit(void)
{
	unregister_virtio_driver(&virtio_net_driver);
}
