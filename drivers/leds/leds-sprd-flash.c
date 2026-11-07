// SPDX-License-Identifier: GPL-2.0
/*
 * Torch driver for the dual-LED flash chips on the Infinix X6525: an AW36515
 * at 0x63 and an OCP81375 at 0x64 on i2c5. The register layout was read out of
 * the stock sprd_flash_drv.ko; both chips use the same one:
 *
 *   0x01  enable: bit0 LED1, bit1 LED2, bits 3:2 mode (2 = torch)
 *   0x05  LED1 torch current code
 *   0x06  LED2 torch current code
 *   0x0c  chip id (read at probe: 0x02 for the AW36515, 0x3a for the OCP81375)
 *
 * The vendor driver does not touch any GPIO for these chips. Registers 0x08,
 * 0x02 and 0x07 are written with the vendor's probe values, whose meaning
 * (flash timeout and current limits, probably) is not known.
 *
 * Each LED channel is a plain LED class device in torch mode. Which channel is
 * the rear light and which the front one is not known; the device tree names
 * them neutrally until it has been tried on the phone.
 */

#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/property.h>

#define REG_ENABLE	0x01
#define REG_CFG_2	0x02
#define REG_TORCH(x)	(0x05 + (x))
#define REG_CFG_7	0x07
#define REG_CFG_8	0x08
#define REG_ID		0x0c

#define EN_LED(x)	BIT(x)
#define EN_MODE_TORCH	(2 << 2)

#define NUM_LEDS	2
#define DEFAULT_MAX	0x30

struct sprd_flash;

struct sprd_flash_led {
	struct led_classdev cdev;
	struct sprd_flash *flash;
	int index;
	u8 code;
};

struct sprd_flash {
	struct i2c_client *client;
	struct mutex lock;
	struct sprd_flash_led leds[NUM_LEDS];
	u8 cfg8;
};

struct sprd_flash_info {
	u8 id;
	u8 cfg8;
};

static int sprd_flash_update(struct sprd_flash *f)
{
	u8 en = 0;
	int i, ret;

	for (i = 0; i < NUM_LEDS; i++) {
		ret = i2c_smbus_write_byte_data(f->client, REG_TORCH(i), f->leds[i].code);
		if (ret)
			return ret;
		if (f->leds[i].code)
			en |= EN_LED(i);
	}

	return i2c_smbus_write_byte_data(f->client, REG_ENABLE, en ? en | EN_MODE_TORCH : 0);
}

static int sprd_flash_brightness_set(struct led_classdev *cdev, enum led_brightness b)
{
	struct sprd_flash_led *led = container_of(cdev, struct sprd_flash_led, cdev);
	struct sprd_flash *f = led->flash;
	int ret;

	mutex_lock(&f->lock);
	led->code = b;
	ret = sprd_flash_update(f);
	mutex_unlock(&f->lock);

	return ret;
}

static void sprd_flash_shutdown(struct i2c_client *client)
{
	struct sprd_flash *f = i2c_get_clientdata(client);

	i2c_smbus_write_byte_data(f->client, REG_ENABLE, 0);
}

static int sprd_flash_probe(struct i2c_client *client)
{
	const struct sprd_flash_info *info = i2c_get_match_data(client);
	struct device *dev = &client->dev;
	struct sprd_flash *f;
	int id, ret;

	f = devm_kzalloc(dev, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	f->client = client;
	mutex_init(&f->lock);
	i2c_set_clientdata(client, f);

	id = i2c_smbus_read_byte_data(client, REG_ID);
	if (id < 0)
		return dev_err_probe(dev, id, "cannot read the chip id\n");
	if (id != info->id)
		dev_warn(dev, "chip id %#x, expected %#x: continuing\n", id, info->id);

	/* the vendor's probe sequence */
	ret = i2c_smbus_write_byte_data(client, REG_ENABLE, 0);
	ret |= i2c_smbus_write_byte_data(client, REG_CFG_8, info->cfg8);
	ret |= i2c_smbus_write_byte_data(client, REG_CFG_2, 0x49);
	ret |= i2c_smbus_write_byte_data(client, REG_CFG_7, 0x09);
	ret |= i2c_smbus_write_byte_data(client, REG_TORCH(0), 0);
	ret |= i2c_smbus_write_byte_data(client, REG_TORCH(1), 0);
	if (ret)
		return dev_err_probe(dev, -EIO, "chip did not take the initial set-up\n");

	device_for_each_child_node_scoped(dev, child) {
		struct led_init_data init = { .fwnode = child };
		struct sprd_flash_led *led;
		u32 reg, max = DEFAULT_MAX;

		if (fwnode_property_read_u32(child, "reg", &reg) || reg >= NUM_LEDS) {
			dev_warn(dev, "LED node without a valid reg (0 or 1)\n");
			continue;
		}
		fwnode_property_read_u32(child, "awinic,max-torch-code", &max);

		led = &f->leds[reg];
		led->flash = f;
		led->index = reg;
		led->cdev.max_brightness = min_t(u32, max, 0x7f);
		led->cdev.brightness_set_blocking = sprd_flash_brightness_set;
		led->cdev.flags = LED_CORE_SUSPENDRESUME | LED_DEV_CAP_FLASH;

		ret = devm_led_classdev_register_ext(dev, &led->cdev, &init);
		if (ret)
			return dev_err_probe(dev, ret, "cannot register LED %u\n", reg);
	}

	dev_info(dev, "dual-LED torch driver ready (chip id %#x)\n", id);
	return 0;
}

static const struct sprd_flash_info aw36515_info = { .id = 0x02, .cfg8 = 0x1a };
static const struct sprd_flash_info ocp81375_info = { .id = 0x3a, .cfg8 = 0x19 };

static const struct of_device_id sprd_flash_of_match[] = {
	{ .compatible = "awinic,aw36515", .data = &aw36515_info },
	{ .compatible = "sprd,flash-ocp81375", .data = &ocp81375_info },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_flash_of_match);

static struct i2c_driver sprd_flash_driver = {
	.driver = {
		.name = "leds-sprd-flash",
		.of_match_table = sprd_flash_of_match,
	},
	.probe = sprd_flash_probe,
	.shutdown = sprd_flash_shutdown,
};
module_i2c_driver(sprd_flash_driver);

MODULE_DESCRIPTION("Torch driver for the AW36515 and OCP81375 flash chips");
MODULE_LICENSE("GPL");
