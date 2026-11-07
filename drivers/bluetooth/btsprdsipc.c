// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Otto Pflüger
 * Based on btqcomsmd.c and some UART drivers
 */

#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/random.h>
#include <linux/rpmsg.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "hci_uart.h"

/* Offset of the BD address inside the pskey config blob */
#define BTSPRDSIPC_PSKEY_BDADDR_OFF	20

struct btsprdsipc {
	struct hci_dev *hdev;
	struct hci_uart hu;
	struct rpmsg_endpoint *ept;
	struct sk_buff *rx_skb;
	size_t rf_cfg_len;
	u8 *rf_cfg;
	size_t pskey_cfg_len;
	u8 *pskey_cfg;
};

static const struct h4_recv_pkt sprd_recv_pkts[] = {
	{ H4_RECV_ACL,      .recv = hci_recv_frame },
	{ H4_RECV_SCO,      .recv = hci_recv_frame },
	{ H4_RECV_EVENT,    .recv = hci_recv_frame },
};

static int btsprdsipc_callback(struct rpmsg_device *rpdev, void *data,
			       int count, void *priv, u32 addr)
{
	struct btsprdsipc *bts = dev_get_drvdata(&rpdev->dev);

	bts->rx_skb = h4_recv_buf(&bts->hu, bts->rx_skb, data, count,
				  sprd_recv_pkts, ARRAY_SIZE(sprd_recv_pkts));
	if (IS_ERR(bts->rx_skb)) {
		int err = PTR_ERR(bts->rx_skb);
		bt_dev_err(bts->hdev, "frame reassembly failed (%d)", err);
		return count;
	}

	bts->hdev->stat.byte_rx += count;
	return count;
}

/*
 * The controller firmware rejects "Write Default Link Policy Settings" with
 * status 0x12 (invalid parameters) although its supported-commands bitmap
 * claims the command, and HCI initialisation gives up on the first failing
 * command, so hci0 could never be powered on ("Opcode 0x080f failed: -22").
 * The default link policy only matters for ACL link role/mode negotiation,
 * which the controller handles by itself, so answer the command locally with
 * a successful Command Complete instead of sending it.
 */
static int btsprdsipc_fake_cmd_complete(struct hci_dev *hdev, struct sk_buff *cmd)
{
	struct hci_ev_cmd_complete *ev;
	struct hci_event_hdr *hdr;
	struct sk_buff *skb;

	skb = bt_skb_alloc(sizeof(*hdr) + sizeof(*ev) + 1, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	hdr = skb_put(skb, sizeof(*hdr));
	hdr->evt = HCI_EV_CMD_COMPLETE;
	hdr->plen = sizeof(*ev) + 1;
	ev = skb_put(skb, sizeof(*ev));
	ev->ncmd = 1;
	ev->opcode = cpu_to_le16(hci_skb_opcode(cmd));
	skb_put_u8(skb, 0x00);	/* status: success */

	hci_skb_pkt_type(skb) = HCI_EVENT_PKT;
	kfree_skb(cmd);

	return hci_recv_frame(hdev, skb);
}

static int btsprdsipc_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btsprdsipc *bts = hci_get_drvdata(hdev);
	unsigned int len;
	int ret;

	if (hci_skb_pkt_type(skb) == HCI_COMMAND_PKT &&
	    hci_skb_opcode(skb) == HCI_OP_WRITE_DEF_LINK_POLICY)
		return btsprdsipc_fake_cmd_complete(hdev, skb);

	/* Prepend skb with frame type */
	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);

	/* The content length needs to be aligned to 8 bytes */
	len = ALIGN(skb->len, 8);
	if (len != skb->len) {
		ret = __skb_pad(skb, len - skb->len, false);
		if (ret)
			return ret;
	}

	ret = rpmsg_send(bts->ept, skb->data, len);
	if (ret) {
		hdev->stat.err_tx++;
	} else {
		hdev->stat.byte_tx += skb->len;

		switch (hci_skb_pkt_type(skb)) {
		case HCI_COMMAND_PKT:
			hdev->stat.cmd_tx++;
			break;
		case HCI_ACLDATA_PKT:
			hdev->stat.acl_tx++;
			break;
		case HCI_SCODATA_PKT:
			hdev->stat.sco_tx++;
			break;
		}

		kfree_skb(skb);
	}

	return ret;
}

static int btsprdsipc_open(struct hci_dev *hdev)
{
	return 0;
}

static int btsprdsipc_close(struct hci_dev *hdev)
{
	return 0;
}

static int btsprdsipc_set_bdaddr(struct hci_dev *hdev, const bdaddr_t *bdaddr)
{
	const u8 enable_cmd[] = { 0, 9, 1 };
	struct btsprdsipc *bts = hci_get_drvdata(hdev);
	struct sk_buff *skb;

	/* It seems that this must always be the first command sent to the
	 * controller in order to actually initialize the BDADDR, so all
	 * initialization is done below and not in setup().
	 */
	memcpy(bts->pskey_cfg + BTSPRDSIPC_PSKEY_BDADDR_OFF, bdaddr, sizeof(*bdaddr));
	skb = __hci_cmd_sync(hdev, 0xfca0, bts->pskey_cfg_len, bts->pskey_cfg,
			     HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		goto err;
	kfree_skb(skb);

	skb = __hci_cmd_sync(hdev, 0xfca2, bts->rf_cfg_len, bts->rf_cfg,
			     HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		goto err;
	kfree_skb(skb);

	skb = __hci_cmd_sync(hdev, 0xfca1, sizeof(enable_cmd), enable_cmd,
			     HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		goto err;
	kfree_skb(skb);

	skb = __hci_cmd_sync(hdev, HCI_OP_RESET, 0, NULL, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		goto err;
	kfree_skb(skb);

	return 0;

err:
	bt_dev_err(hdev, "initialization sequence failed (%ld)", PTR_ERR(skb));
	return PTR_ERR(skb);
}

static int btsprdsipc_probe(struct rpmsg_device *rpdev)
{
	const struct firmware *fw;
	struct btsprdsipc *bts;
	struct hci_dev *hdev;
	bdaddr_t addr;
	int ret;

	bts = devm_kzalloc(&rpdev->dev, sizeof(*bts), GFP_KERNEL);
	if (!bts)
		return -ENOMEM;

	ret = request_firmware(&fw, "sprd/bt_config_pskey.bin", &rpdev->dev);
	if (ret < 0) {
		dev_err(&rpdev->dev, "firmware request for pskey config failed (%d)", ret);
		return ret;
	}

	/* set_bdaddr() patches the BD address in at offset 20 */
	if (fw->size < BTSPRDSIPC_PSKEY_BDADDR_OFF + sizeof(bdaddr_t)) {
		dev_err(&rpdev->dev, "pskey config is too short (%zu bytes)\n",
			fw->size);
		release_firmware(fw);
		return -EINVAL;
	}

	bts->pskey_cfg_len = fw->size;
	bts->pskey_cfg = devm_kmemdup(&rpdev->dev, fw->data, fw->size, GFP_KERNEL);
	release_firmware(fw);
	if (!bts->pskey_cfg)
		return -ENOMEM;

	ret = request_firmware(&fw, "sprd/bt_config_rf.bin", &rpdev->dev);
	if (ret < 0) {
		dev_err(&rpdev->dev, "firmware request for rf config failed (%d)", ret);
		return ret;
	}

	bts->rf_cfg_len = fw->size;
	bts->rf_cfg = devm_kmemdup(&rpdev->dev, fw->data, fw->size, GFP_KERNEL);
	release_firmware(fw);
	if (!bts->rf_cfg)
		return -ENOMEM;

	hdev = hci_alloc_dev();
	if (!hdev)
		return -ENOMEM;

	hci_set_drvdata(hdev, bts);
	bts->hdev = hdev;
	bts->hu.hdev = hdev;
	bts->ept = rpdev->ept;
	SET_HCIDEV_DEV(hdev, &rpdev->dev);

	/*
	 * The dtsi leaves local-bd-address for the bootloader to fill in.
	 * U-Boot's ft_board_setup() writes it from its "bdaddr" environment
	 * variable, which is unset on this board, and string_to_enetaddr(NULL)
	 * zero-fills - so the property ends up all-zero. The HCI core rejects
	 * an all-zero (or missing) property, leaves public_addr zero and
	 * therefore never calls set_bdaddr(), which is where this driver
	 * sends the pskey and RF configuration - so the controller would
	 * never be initialised. Fall back to a random, locally administered
	 * address. It is not stable across boots (nor are the pairings)
	 * until the bootloader or the dtsi supplies a real one.
	 */
	if (device_property_read_u8_array(&rpdev->dev, "local-bd-address",
					  (u8 *)&addr, sizeof(addr)) ||
	    !bacmp(&addr, BDADDR_ANY)) {
		get_random_bytes(&addr, sizeof(addr));
		/* The binding stores local-bd-address least significant byte
		 * first, so the first octet of the printed address - where the
		 * multicast and locally administered bits live - is the last
		 * byte. Setting them on byte 0 would leave it a real multicast
		 * address half the time. */
		((u8 *)&addr)[5] = (((u8 *)&addr)[5] & 0xfc) | 0x02;
		dev_warn(&rpdev->dev, "no local-bd-address, using %pMR\n",
			 &addr);
	}
	bacpy(&hdev->public_addr, &addr);

	hdev->manufacturer = 1855;
	hdev->bus = HCI_SMD;
	hdev->open = btsprdsipc_open;
	hdev->close = btsprdsipc_close;
	hdev->send = btsprdsipc_send;
	hdev->set_bdaddr = btsprdsipc_set_bdaddr;

	hci_set_quirk(hdev, HCI_QUIRK_USE_BDADDR_PROPERTY);

	dev_set_drvdata(&rpdev->dev, bts);

	ret = hci_register_dev(hdev);
	if (ret < 0) {
		dev_set_drvdata(&rpdev->dev, NULL);
		goto hci_free_dev;
	}

	return 0;

hci_free_dev:
	hci_free_dev(hdev);

	return ret;
}

static void btsprdsipc_remove(struct rpmsg_device *rpdev)
{
	struct btsprdsipc *bts = dev_get_drvdata(&rpdev->dev);

	hci_unregister_dev(bts->hdev);
	hci_free_dev(bts->hdev);
}

static const struct of_device_id btsprdsipc_of_match[] = {
	{ .compatible = "sprd,bluetooth-sipc", },
	{ },
};
MODULE_DEVICE_TABLE(of, btsprdsipc_of_match);

static struct rpmsg_driver btsprdsipc_driver = {
	.probe = btsprdsipc_probe,
	.remove = btsprdsipc_remove,
	.callback = btsprdsipc_callback,
	.drv = {
		.name = "btsprdsipc",
		.of_match_table = btsprdsipc_of_match,
	},
};

module_rpmsg_driver(btsprdsipc_driver);

MODULE_AUTHOR("Otto Pflüger <otto.pflueger@abscue.de>");
MODULE_DESCRIPTION("Unisoc SIPC HCI driver");
MODULE_LICENSE("GPL v2");
