// SPDX-License-Identifier: GPL-2.0
/*
 * MAX96701 (GMSL1 serializer) side of the Waveshare GMSL-1MP-Camera-A, and
 * the camera type's GMSL1 settings for the MAX9296A.
 *
 * Values are the ones Waveshare's Raspberry Pi 5 module programs, recovered
 * in docs/REGISTER-SEQUENCE.md.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/kernel.h>

#include "gmsl1mp.h"

#define SER_REG_ADDR		0x00	/* own 8-bit I2C address */
#define SER_REG_ID		0x1e
#define GMSL1MP_DELAY_MS	200	/* Waveshare's settle time */

/*
 * MAX9296A GMSL1 block settings for this camera (offsets into the link's
 * 0x0b00 / 0x0c00 block), and the soft BPP for its pipe: YUV422 8-bit is
 * carried as 8-bit words on this GMSL1 link.
 */
static const struct gmsl_des_reg gmsl1mp_gmsl1_regs[] = {
	{ 0x06, 0x87 },
	{ 0x0d, 0xb2 },
	{ 0x07, 0x86 },
	{ 0x0f, 0x01 },
	{ 0x96, 0x1b },
};

const struct gmsl_des_gmsl1_cfg gmsl1mp_gmsl1_cfg = {
	.regs = gmsl1mp_gmsl1_regs,
	.num_regs = ARRAY_SIZE(gmsl1mp_gmsl1_regs),
	.bpp = 8,
	.yuv422_8_10_mux = true,	/* without it the chroma is wrong (green/red image) */
};

struct reg8 {
	u8 reg;
	u8 val;
	u16 delay_ms;
};

/* MAX96701 configuration, identical for every Waveshare table. */
static const struct reg8 ser_init[] = {
	{ 0x04, 0x47, GMSL1MP_DELAY_MS },	/* config link, serial off */
	{ 0x0f, 0xbf, GMSL1MP_DELAY_MS },
	{ 0x07, 0x84 },
	{ 0x40, 0x0d },
	{ 0x04, 0x87, GMSL1MP_DELAY_MS },	/* serial link on */
};

static int xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int n)
{
	int ret, tries = 3;

	do {
		ret = i2c_transfer(adap, msgs, n);
		if (ret == n)
			return 0;
		usleep_range(1000, 2000);
	} while (--tries);
	return ret < 0 ? ret : -EIO;
}

static int ser_write(struct i2c_adapter *adap, u8 addr, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	struct i2c_msg msg = { .addr = addr, .len = 2, .buf = buf };

	return xfer(adap, &msg, 1);
}

static int ser_read(struct i2c_adapter *adap, u8 addr, u8 reg, u8 *val)
{
	struct i2c_msg msgs[2] = {
		{ .addr = addr, .len = 1, .buf = &reg },
		{ .addr = addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};

	return xfer(adap, msgs, 2);
}

int gmsl1mp_ser_id(struct i2c_adapter *adap, u8 addr, u8 *id)
{
	return ser_read(adap, addr, SER_REG_ID, id);
}

int gmsl1mp_ser_setup(struct device *dev, struct i2c_adapter *adap, u8 addr,
		      u32 link)
{
	size_t i;
	u8 v;
	int ret;

	/*
	 * Fresh camera: serializer at its default address, move it. Already
	 * moved (driver reload without a camera power cycle): use it as is.
	 * The deserializer brings links up one at a time, so only this link's
	 * MAX96701 can be at the default address here.
	 */
	if (addr != GMSL1MP_SER_DEFAULT_ADDR &&
	    !ser_read(adap, GMSL1MP_SER_DEFAULT_ADDR, SER_REG_ADDR, &v)) {
		ret = ser_write(adap, GMSL1MP_SER_DEFAULT_ADDR, SER_REG_ADDR, addr << 1);
		if (ret)
			return ret;
		msleep(GMSL1MP_DELAY_MS);
	}

	ret = ser_read(adap, addr, SER_REG_ID, &v);
	if (ret) {
		dev_err(dev, "no serializer answering at 0x%02x or 0x%02x on link %c\n",
			GMSL1MP_SER_DEFAULT_ADDR, addr, 'A' + link);
		return -ENODEV;
	}
	dev_info(dev, "link %c: serializer at 0x%02x, id 0x%02x\n", 'A' + link, addr, v);

	for (i = 0; i < ARRAY_SIZE(ser_init); i++) {
		ret = ser_write(adap, addr, ser_init[i].reg, ser_init[i].val);
		if (ret) {
			dev_err(dev, "serializer 0x%02x: write 0x%02x failed (%d)\n",
				addr, ser_init[i].reg, ret);
			return ret;
		}
		if (ser_init[i].delay_ms)
			msleep(ser_init[i].delay_ms);
	}
	return 0;
}
