// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Virtio SPI Controller Driver
 *
 * The Virtio SPI Specification Patch this driver follows:
 * https://lore.kernel.org/all/20230901032933.32434-1-quic_haixcui@quicinc.com/
 * With some fixes in comments. bus_num is removed from config since it's
 * unnecessary.
 *
 * Copyright (c) 2023 Intel Corporation. All rights reserved.
 */

#include <linux/acpi.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/spi/spi.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_config.h>
#include <linux/virtio_spi.h>

#define VIRTIO_MAX_XFER_SIZE		0xffffff

/**
 * struct virtio_spi - virtio SPI data
 * @vdev: virtio device for this controller
 * @controller: spi controller for the spi core
 * @vq: the virtio virtqueue for communication
 */
struct virtio_spi {
	struct virtio_device *vdev;
	struct spi_controller *controller;
	struct virtqueue *vq;
};

/**
 * struct virtio_spi_req - the virtio SPI request structure
 * @completion: completion of virtio SPI message
 * @head: the transfer header of the virtio SPI message
 * @tx_buf: the buffer from which it's written
 * @rx_buf: the buffer into which data is read
 * @result: the result of the virtio I2C message
 */
struct virtio_spi_req {
	struct completion completion;
	struct virtio_spi_transfer_head head;
	const void *tx_buf;
	void *rx_buf;
	struct virtio_spi_transfer_result result;
};

static void virtio_spi_xfer_done(struct virtqueue *vq)
{
	struct virtio_spi_req *req;
	unsigned int len;

	while ((req = virtqueue_get_buf(vq, &len)))
		complete(&req->completion);
}

static void virtio_spi_del_vqs(struct virtio_device *vdev)
{
	vdev->config->reset(vdev);
	vdev->config->del_vqs(vdev);
}

static int virtio_spi_setup_vqs(struct virtio_spi *vspi)
{
	struct virtio_device *vdev = vspi->vdev;

	vspi->vq = virtio_find_single_vq(vdev, virtio_spi_xfer_done, "xfer");
	return PTR_ERR_OR_ZERO(vspi->vq);
}

static size_t virtio_spi_max_transfer_size(struct spi_device *spi)
{
	return VIRTIO_MAX_XFER_SIZE;
}

static int virtio_spi_transfer_one(struct spi_controller *controller,
				  struct spi_device *spi,
				  struct spi_transfer *xfer)
{
	struct virtio_spi *vspi = spi_controller_get_devdata(controller);
	struct virtio_spi_req *req;
	struct scatterlist *sgs[4], head, tx_buf, rx_buf, res;
	int ret = 0;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	init_completion(&req->completion);
	req->head.slave_id = spi->chip_select[0];
	req->head.bits_per_word = spi->bits_per_word;
	req->head.cs_change = xfer->cs_change;
	req->head.tx_nbits = xfer->tx_nbits;
	req->head.rx_nbits = xfer->rx_nbits;
	req->head.mode = spi->mode;
	req->head.freq = xfer->speed_hz;
	req->head.word_delay_ns = spi_delay_to_ns(&xfer->word_delay, xfer);
	req->head.cs_setup_ns = spi_delay_to_ns(&spi->cs_setup, xfer);
	req->head.cs_delay_hold_ns = spi_delay_to_ns(&spi->cs_hold, xfer);
	req->head.cs_change_delay_inactive_ns = spi_delay_to_ns(&spi->cs_inactive, xfer) + spi_delay_to_ns(&xfer->cs_change_delay, xfer);
	sg_init_one(&head, &req->head, sizeof(req->head));
	sgs[0] = &head;

	req->tx_buf = xfer->tx_buf;
	sg_init_one(&tx_buf, req->tx_buf, xfer->len);
	sgs[1] = &tx_buf;
	req->rx_buf = xfer->rx_buf;
	sg_init_one(&rx_buf, req->rx_buf, xfer->len);
	sgs[2] = &rx_buf;

	sg_init_one(&res, &req->result, sizeof(req->result));
	sgs[3] = &res;

	ret = virtqueue_add_sgs(vspi->vq, sgs, 2, 2, req, GFP_KERNEL);
	if (ret) {
		dev_err(&vspi->vdev->dev, "fail to add sgs to vq!\n");
		goto err_free;
	}

	virtqueue_kick(vspi->vq);

	wait_for_completion(&req->completion);

	if (req->result.result != VIRTIO_SPI_TRANS_OK) {
		dev_err(&vspi->vdev->dev, "result is bad!\n");
		ret = -EIO;
	}

err_free:
	kfree(req);
	return ret;
}

static int virtio_spi_probe(struct virtio_device *vdev)
{
	struct virtio_spi *vspi;
	struct spi_controller *controller;
	int ret = 0;

	controller = spi_alloc_master(&vdev->dev, sizeof(struct virtio_spi));
	if (!controller) {
		dev_err(&vdev->dev, "Unable to allocate SPI controller\n");
		return -ENOMEM;
	}
	vspi = spi_controller_get_devdata(controller);
	vdev->priv = vspi;
	vspi->vdev = vdev;
	vspi->controller = controller;

	ret = virtio_spi_setup_vqs(vspi);
	if (ret)
		goto err_free_controller;

	controller->transfer_one = virtio_spi_transfer_one;
	controller->num_chipselect = virtio_cread16(vdev,
			offsetof(struct virtio_spi_config, num_cs));
	controller->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH | SPI_LSB_FIRST;
	controller->bits_per_word_mask = SPI_BPW_MASK(8);
	controller->max_transfer_size = virtio_spi_max_transfer_size;

	/*
	 * Setup ACPI node for controlled devices which will be probed through
	 * ACPI.
	 */
	ACPI_COMPANION_SET(&vdev->dev, ACPI_COMPANION(vdev->dev.parent));

	ret = devm_spi_register_controller(&vdev->dev, controller);
	if (ret) {
		dev_err(&vdev->dev, "cannot register SPI controller\n");
		goto err_del_vqs;
	}

	return 0;

err_del_vqs:
	virtio_spi_del_vqs(vdev);
err_free_controller:
	spi_controller_put(controller);
	return ret;
}

static void virtio_spi_remove(struct virtio_device *vdev)
{
	struct virtio_spi *vspi = vdev->priv;

	virtio_spi_del_vqs(vdev);
	spi_controller_put(vspi->controller);
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_SPI, VIRTIO_DEV_ANY_ID },
	{}
};

#ifdef CONFIG_PM_SLEEP
static int virtio_spi_freeze(struct virtio_device *vdev)
{
	virtio_spi_del_vqs(vdev);
	return 0;
}

static int virtio_spi_restore(struct virtio_device *vdev)
{
	return virtio_spi_setup_vqs(vdev->priv);
}
#endif

static struct virtio_driver virtio_spi_driver = {
	.id_table		= id_table,
	.probe			= virtio_spi_probe,
	.remove			= virtio_spi_remove,
	.driver			= {
		.name	= "virtio-spi",
	},
#ifdef CONFIG_PM_SLEEP
	.freeze = virtio_spi_freeze,
	.restore = virtio_spi_restore,
#endif
};
module_virtio_driver(virtio_spi_driver);

MODULE_AUTHOR("Qiang Zhang <qiang4.zhang@intel.com>");
MODULE_DESCRIPTION("Virtio SPI controller driver");
MODULE_LICENSE("GPL");
