// SPDX-License-Identifier: GPL-2.0
/*
 * MAX9296A (GMSL1 mode) + MAX96701 serdes core for the Waveshare
 * GMSL-1MP-Camera-A.
 *
 * The register sequence is the one Waveshare's Raspberry Pi 5 module
 * (gmsl-1mp-camera-a.ko, GPL) programs, recovered and decoded in
 * docs/REGISTER-SEQUENCE.md. It is split here into:
 *   - a global deserializer init, run once per MAX9296A by whichever camera
 *     probes first;
 *   - a per-link step (MIPI port/VC routing, serializer address remap and
 *     serializer configuration), run once per camera;
 *   - a refcounted CSI output gate for streaming.
 *
 * All bus traffic goes over the camera node's I2C adapter to fixed 7-bit
 * addresses (deserializer, serializers behind the GMSL I2C tunnel), under the
 * per-deserializer mutex.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

#include "gmsl1mp.h"

/* ---- MAX9296A registers (16-bit address) ---- */
#define DES_REG_CTRL0		0x0010	/* bit7 RESET_ALL */
#define DES_CTRL0_RESET_ALL	0x80
#define DES_REG_DEV_ID		0x000d
#define DES_DEV_ID_MAX9296A	0x94
#define DES_REG_BACKTOP12	0x0313	/* CSI_OUT_EN */
#define DES_CSI_OUT_ON		0x42	/* value Waveshare uses */
#define DES_CSI_OUT_OFF		0x00
#define DES_REG_GMSL1_EN	0x0f00	/* bit0 link A, bit1 link B */
#define DES_REG_GMSL1_LOCK(l)	((l) ? 0x0ccb : 0x0bcb)	/* bit0 LOCKED */
/* MIPI controller lane count, bits[7:6] = lanes - 1.
 * Port A is driven by controller 1, port B by controller 2. */
#define DES_REG_LANES(p)	((p) ? 0x048a : 0x044a)
/* Per-port D-PHY PLL: bit5 = override enable, bits[4:0] = rate / 100 Mbps */
#define DES_REG_DPLL(p)		((p) ? 0x0323 : 0x0320)
#define DES_DPLL_OVERRIDE	0x20
/* Video pipe mapping block: pipe X (link A) at 0x040b, pipe Y (link B) at
 * 0x044b. +0 map enable, +2.. src/dst pairs, +0x22 destination controller
 * (2 bits per map). */
#define DES_PIPE_BASE(l)	((l) ? 0x044b : 0x040b)
#define DES_PIPE_MAP_EN		0x00
#define DES_PIPE_MAP0_SRC	0x02
#define DES_PIPE_DPHY_DEST	0x22

/* ---- MAX96701 registers (8-bit address) ---- */
#define SER_REG_ADDR		0x00	/* own 8-bit I2C address */
#define SER_REG_ID		0x1e

#define GMSL1MP_DELAY_MS	200	/* Waveshare's settle time, every step */

struct reg16 {
	u16 reg;
	u8 val;
	u16 delay_ms;	/* sleep after the write */
};

/*
 * Global deserializer init. Taken verbatim (order and values) from the
 * common prefix/suffix of Waveshare's gmsl_1ch_linka/linkb/2ch tables; the
 * pipe mapping, lane count and PHY rate parts moved to the per-link step.
 */
static const struct reg16 des_global_init[] = {
	{ 0x0010, 0x80, GMSL1MP_DELAY_MS },	/* RESET_ALL */
	{ 0x0313, 0x00 },			/* CSI output off */
	{ 0x0011, 0x0f },
	{ 0x0b06, 0x87 }, { 0x0c06, 0x87 },	/* GMSL1 link A / B setup */
	{ 0x0b0d, 0xb2 }, { 0x0c0d, 0xb2 },
	{ 0x0b07, 0x86 }, { 0x0c07, 0x86 },
	{ 0x0322, 0x30 },
	{ 0x0005, 0x80 },
	/* MFP4: drive high, then low (camera-side reset/power pulse) */
	{ 0x02bc, 0x90, GMSL1MP_DELAY_MS },
	{ 0x02bc, 0x80 },
	{ 0x02bd, 0x84, GMSL1MP_DELAY_MS },
	{ 0x0006, 0x1f },			/* both links in GMSL1 mode */
	{ 0x0010, 0x35 },
	{ 0x0012, 0x10, GMSL1MP_DELAY_MS },
	{ 0x0330, 0x04 },			/* MIPI PHY: 2 x 4-lane ports */
	{ 0x0314, 0x00 },
	{ 0x0319, 0x08 },			/* link-B / 2ch tables only */
	{ 0x0316, 0x5e },
	{ 0x0317, 0x0e },
	{ 0x031d, 0xc0 },
	{ 0x0325, 0x80 },
	{ 0x0b0f, 0x01 }, { 0x0c0f, 0x01 },
	{ 0x0b96, 0x1b }, { 0x0c96, 0x1b },
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

static LIST_HEAD(deser_list);
static DEFINE_MUTEX(deser_list_lock);

/* ------------------------------------------------------------------ */
/* Raw I2C                                                             */

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

static int des_write(struct gmsl1mp_deser *des, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xff, val };
	struct i2c_msg msg = { .addr = des->addr, .len = 3, .buf = buf };
	int ret = xfer(des->adap, &msg, 1);

	if (ret)
		dev_err(des->dev, "des 0x%02x: write 0x%04x=0x%02x failed (%d)\n",
			des->addr, reg, val, ret);
	return ret;
}

static int des_read(struct gmsl1mp_deser *des, u16 reg, u8 *val)
{
	u8 buf[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[2] = {
		{ .addr = des->addr, .len = 2, .buf = buf },
		{ .addr = des->addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};

	return xfer(des->adap, msgs, 2);
}

static int ser_write(struct gmsl1mp_deser *des, u8 addr, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	struct i2c_msg msg = { .addr = addr, .len = 2, .buf = buf };
	int ret = xfer(des->adap, &msg, 1);

	if (ret)
		dev_err(des->dev, "ser 0x%02x: write 0x%02x=0x%02x failed (%d)\n",
			addr, reg, val, ret);
	return ret;
}

static int ser_read(struct gmsl1mp_deser *des, u8 addr, u8 reg, u8 *val)
{
	struct i2c_msg msgs[2] = {
		{ .addr = addr, .len = 1, .buf = &reg },
		{ .addr = addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};

	return xfer(des->adap, msgs, 2);
}

/* Exported (locked) accessors for frontends / debug. */
int gmsl1mp_des_read(struct gmsl1mp_deser *des, u16 reg, u8 *val)
{
	int ret;

	mutex_lock(&des->lock);
	ret = des_read(des, reg, val);
	mutex_unlock(&des->lock);
	return ret;
}

int gmsl1mp_des_write(struct gmsl1mp_deser *des, u16 reg, u8 val)
{
	int ret;

	mutex_lock(&des->lock);
	ret = des_write(des, reg, val);
	mutex_unlock(&des->lock);
	return ret;
}

static int des_write_table(struct gmsl1mp_deser *des,
			   const struct reg16 *t, size_t n)
{
	size_t i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = des_write(des, t[i].reg, t[i].val);
		if (ret)
			return ret;
		if (t[i].delay_ms)
			msleep(t[i].delay_ms);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Shared deserializer object                                          */

struct gmsl1mp_deser *gmsl1mp_deser_get(struct device *dev,
					 struct i2c_adapter *adap, u8 addr)
{
	struct gmsl1mp_deser *des;
	u8 id;
	int ret;

	mutex_lock(&deser_list_lock);
	list_for_each_entry(des, &deser_list, node) {
		if (des->adap == adap && des->addr == addr) {
			des->refcount++;
			goto out;
		}
	}

	des = kzalloc(sizeof(*des), GFP_KERNEL);
	if (!des) {
		des = ERR_PTR(-ENOMEM);
		goto out;
	}
	des->adap = adap;
	des->addr = addr;
	des->dev = &adap->dev;
	des->refcount = 1;
	mutex_init(&des->lock);

	ret = des_read(des, DES_REG_DEV_ID, &id);
	if (ret) {
		dev_err(dev, "no MAX9296A at 0x%02x on %s (%d)\n",
			addr, adap->name, ret);
		goto err;
	}
	if (id != DES_DEV_ID_MAX9296A)
		dev_warn(dev, "deserializer 0x%02x: device id 0x%02x, expected 0x%02x (MAX9296A)\n",
			 addr, id, DES_DEV_ID_MAX9296A);

	dev_info(dev, "MAX9296A at 0x%02x: global init\n", addr);
	ret = des_write_table(des, des_global_init,
			      ARRAY_SIZE(des_global_init));
	if (ret)
		goto err;
	/* No GMSL1 link enabled until a camera claims one. */
	ret = des_write(des, DES_REG_GMSL1_EN, 0x00);
	if (ret)
		goto err;

	list_add(&des->node, &deser_list);
	goto out;

err:
	mutex_destroy(&des->lock);
	kfree(des);
	des = ERR_PTR(ret);
out:
	mutex_unlock(&deser_list_lock);
	return des;
}
EXPORT_SYMBOL_GPL(gmsl1mp_deser_get);

void gmsl1mp_deser_put(struct gmsl1mp_deser *des)
{
	mutex_lock(&deser_list_lock);
	if (--des->refcount == 0) {
		des_write(des, DES_REG_BACKTOP12, DES_CSI_OUT_OFF);
		list_del(&des->node);
		mutex_destroy(&des->lock);
		kfree(des);
	}
	mutex_unlock(&deser_list_lock);
}
EXPORT_SYMBOL_GPL(gmsl1mp_deser_put);

/* ------------------------------------------------------------------ */
/* Per link                                                            */

static int des_config_port(struct gmsl1mp_deser *des, struct device *dev,
			   const struct gmsl1mp_link_cfg *cfg)
{
	unsigned int p = cfg->port;
	u64 mbps = div_u64(cfg->link_freq * 2, 1000000);
	unsigned int step = div_u64(mbps, 100);
	u8 lanes_val;
	int ret;

	if (des->ports_users[p]) {
		/* Port already set up by the other camera (VC sharing). */
		if (des->ports_lanes[p] != cfg->lanes ||
		    des->ports_freq[p] != cfg->link_freq) {
			dev_err(dev, "port %c already configured with %u lanes @ %llu Hz\n",
				'A' + p, des->ports_lanes[p], des->ports_freq[p]);
			return -EINVAL;
		}
		return 0;
	}

	if (step < 1 || step > 25 || step * 100 != mbps) {
		dev_err(dev, "link-frequency %llu Hz -> %llu Mbps/lane: not a multiple of 100 Mbps in 100..2500\n",
			cfg->link_freq, mbps);
		return -EINVAL;
	}

	ret = des_read(des, DES_REG_LANES(p), &lanes_val);
	if (ret)
		return ret;
	lanes_val = (lanes_val & 0x3f) | ((cfg->lanes - 1) << 6);
	ret = des_write(des, DES_REG_LANES(p), lanes_val);
	if (ret)
		return ret;
	ret = des_write(des, DES_REG_DPLL(p), DES_DPLL_OVERRIDE | step);
	if (ret)
		return ret;

	des->ports_lanes[p] = cfg->lanes;
	des->ports_freq[p] = cfg->link_freq;
	return 0;
}

static int des_map_pipe(struct gmsl1mp_deser *des,
			const struct gmsl1mp_link_cfg *cfg)
{
	u16 base = DES_PIPE_BASE(cfg->link);
	u8 vc = cfg->vc << 6;
	/* map0: video, map1: frame start (DT 0x00), map2: frame end (DT 0x01) */
	const u8 dt[3] = { GMSL1MP_CSI2_DT_YUV422_8B, 0x00, 0x01 };
	/* controller 1 (port A) = 0b01, controller 2 (port B) = 0b10 per map */
	u8 dest = cfg->port ? 0x2a : 0x15;
	int i, ret;

	for (i = 0; i < 3; i++) {
		ret = des_write(des, base + DES_PIPE_MAP0_SRC + 2 * i, dt[i]);
		if (ret)
			return ret;
		ret = des_write(des, base + DES_PIPE_MAP0_SRC + 2 * i + 1,
				vc | dt[i]);
		if (ret)
			return ret;
	}
	ret = des_write(des, base + DES_PIPE_DPHY_DEST, dest);
	if (ret)
		return ret;
	return des_write(des, base + DES_PIPE_MAP_EN, 0x07);
}

static int link_wait_lock(struct gmsl1mp_deser *des, unsigned int link)
{
	unsigned int i;
	u8 v = 0;

	for (i = 0; i < 50; i++) {
		if (!des_read(des, DES_REG_GMSL1_LOCK(link), &v) && (v & 1))
			return 0;
		msleep(20);
	}
	return -ENOLINK;
}

static int ser_setup(struct gmsl1mp_deser *des, struct device *dev,
		     const struct gmsl1mp_link_cfg *cfg)
{
	u8 addr = cfg->ser_addr, v;
	size_t i;
	int ret;

	/*
	 * Fresh camera: serializer at its default address, move it. Already
	 * moved (driver reload without camera power cycle): use it as is.
	 * Only this link's serializer can be at the default address, since
	 * every other enabled link was remapped first.
	 */
	if (addr != GMSL1MP_SER_DEFAULT_ADDR &&
	    !ser_read(des, GMSL1MP_SER_DEFAULT_ADDR, SER_REG_ADDR, &v)) {
		ret = ser_write(des, GMSL1MP_SER_DEFAULT_ADDR, SER_REG_ADDR,
				addr << 1);
		if (ret)
			return ret;
		msleep(GMSL1MP_DELAY_MS);
	}

	ret = ser_read(des, addr, SER_REG_ID, &v);
	if (ret) {
		dev_err(dev, "no serializer answering at 0x%02x or 0x%02x on link %c\n",
			GMSL1MP_SER_DEFAULT_ADDR, addr, 'A' + cfg->link);
		return -ENODEV;
	}
	dev_info(dev, "link %c: serializer at 0x%02x, id 0x%02x\n",
		 'A' + cfg->link, addr, v);

	for (i = 0; i < ARRAY_SIZE(ser_init); i++) {
		ret = ser_write(des, addr, ser_init[i].reg, ser_init[i].val);
		if (ret)
			return ret;
		if (ser_init[i].delay_ms)
			msleep(ser_init[i].delay_ms);
	}
	return 0;
}

int gmsl1mp_link_add(struct gmsl1mp_deser *des, struct device *dev,
		     const struct gmsl1mp_link_cfg *cfg)
{
	u8 en;
	int ret;

	if (cfg->link >= GMSL1MP_NUM_LINKS || cfg->port >= GMSL1MP_NUM_PORTS ||
	    cfg->vc > 3 || !cfg->lanes || cfg->lanes == 3 || cfg->lanes > 4)
		return -EINVAL;

	mutex_lock(&des->lock);

	if (des->links_used & BIT(cfg->link)) {
		dev_err(dev, "GMSL link %c already claimed by another camera\n",
			'A' + cfg->link);
		ret = -EBUSY;
		goto out;
	}

	ret = des_config_port(des, dev, cfg);
	if (ret)
		goto out;
	ret = des_map_pipe(des, cfg);
	if (ret)
		goto out;

	/* Enable this link alongside the ones already up. */
	en = des->links_used | BIT(cfg->link);
	ret = des_write(des, DES_REG_GMSL1_EN, en);
	if (ret)
		goto out;
	msleep(GMSL1MP_DELAY_MS);

	ret = link_wait_lock(des, cfg->link);
	if (ret) {
		dev_err(dev, "GMSL link %c: no lock - camera not connected / no PoC?\n",
			'A' + cfg->link);
		goto err_disable;
	}

	ret = ser_setup(des, dev, cfg);
	if (ret)
		goto err_disable;

	des->links_used |= BIT(cfg->link);
	des->ports_users[cfg->port]++;
	dev_info(dev, "link %c -> MIPI port %c, VC%u, %u lanes @ %llu Mbps/lane\n",
		 'A' + cfg->link, 'A' + cfg->port, cfg->vc, cfg->lanes,
		 div_u64(cfg->link_freq * 2, 1000000));
	goto out;

err_disable:
	des_write(des, DES_REG_GMSL1_EN, des->links_used);
	des_write(des, DES_PIPE_BASE(cfg->link) + DES_PIPE_MAP_EN, 0x00);
out:
	mutex_unlock(&des->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gmsl1mp_link_add);

void gmsl1mp_link_remove(struct gmsl1mp_deser *des,
			 const struct gmsl1mp_link_cfg *cfg)
{
	mutex_lock(&des->lock);
	des_write(des, DES_PIPE_BASE(cfg->link) + DES_PIPE_MAP_EN, 0x00);
	des->links_used &= ~BIT(cfg->link);
	des->ports_users[cfg->port]--;
	/*
	 * Leave the GMSL link itself enabled: the serializer keeps its
	 * remapped address, which ser_setup() handles on the next probe.
	 */
	mutex_unlock(&des->lock);
}
EXPORT_SYMBOL_GPL(gmsl1mp_link_remove);

int gmsl1mp_stream(struct gmsl1mp_deser *des,
		   const struct gmsl1mp_link_cfg *cfg, bool on)
{
	int ret = 0;

	mutex_lock(&des->lock);
	if (on) {
		if (des->streaming == 0)
			ret = des_write(des, DES_REG_BACKTOP12, DES_CSI_OUT_ON);
		if (!ret)
			des->streaming++;
	} else if (des->streaming) {
		if (--des->streaming == 0)
			ret = des_write(des, DES_REG_BACKTOP12, DES_CSI_OUT_OFF);
	}
	mutex_unlock(&des->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gmsl1mp_stream);

ssize_t gmsl1mp_status(struct gmsl1mp_deser *des,
		       const struct gmsl1mp_link_cfg *cfg, char *buf)
{
	u16 base = DES_PIPE_BASE(cfg->link);
	u8 lock_a = 0, lock_b = 0, en = 0, out = 0, lanes = 0, dpll = 0;
	u8 map_en = 0, dest = 0, dst = 0, ser_id = 0;
	int ser_ok;

	mutex_lock(&des->lock);
	des_read(des, DES_REG_GMSL1_LOCK(0), &lock_a);
	des_read(des, DES_REG_GMSL1_LOCK(1), &lock_b);
	des_read(des, DES_REG_GMSL1_EN, &en);
	des_read(des, DES_REG_BACKTOP12, &out);
	des_read(des, DES_REG_LANES(cfg->port), &lanes);
	des_read(des, DES_REG_DPLL(cfg->port), &dpll);
	des_read(des, base + DES_PIPE_MAP_EN, &map_en);
	des_read(des, base + DES_PIPE_MAP0_SRC + 1, &dst);
	des_read(des, base + DES_PIPE_DPHY_DEST, &dest);
	ser_ok = ser_read(des, cfg->ser_addr, SER_REG_ID, &ser_id);
	mutex_unlock(&des->lock);

	return sysfs_emit(buf,
		"link=%c port=%c vc=%u lanes=%u link_freq=%llu ser_addr=0x%02x\n"
		"gmsl1_lock_a=%u gmsl1_lock_b=%u gmsl1_en=0x%02x csi_out=0x%02x streaming=%u\n"
		"port_lanes_reg=0x%02x port_dpll_reg=0x%02x\n"
		"pipe_map_en=0x%02x map0_dst=0x%02x dphy_dest=0x%02x\n"
		"ser_id=%s0x%02x\n",
		'A' + cfg->link, 'A' + cfg->port, cfg->vc, cfg->lanes,
		cfg->link_freq, cfg->ser_addr,
		lock_a & 1, lock_b & 1, en, out, des->streaming,
		lanes, dpll, map_en, dst, dest,
		ser_ok ? "ERR " : "", ser_id);
}
EXPORT_SYMBOL_GPL(gmsl1mp_status);
