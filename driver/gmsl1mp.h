/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Waveshare GMSL-1MP-Camera-A (MAX96701 GMSL1 serializer + on-camera ISP,
 * YUV422 1280x960@30) behind a MAX9296A deserializer.
 *
 * The driver is split so the same serdes core can sit under different host
 * frameworks:
 *   gmsl1mp_serdes.c  - host-independent: shared deserializer object, global
 *                       deserializer init, per-link bring-up, stream gate.
 *   gmsl1mp_v4l2.c    - generic V4L2 async subdev frontend (Raspberry Pi 5
 *                       rp1-cfe, any mainline CSI-2 receiver).
 */
#ifndef __GMSL1MP_H__
#define __GMSL1MP_H__

#include <linux/i2c.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>

#define GMSL1MP_NUM_LINKS	2	/* MAX9296A GMSL link A / link B */
#define GMSL1MP_NUM_PORTS	2	/* MAX9296A MIPI output port A / B */

#define GMSL1MP_SER_DEFAULT_ADDR 0x40	/* MAX96701 power-on 7-bit address */

/* CSI-2 data type of the camera ISP output: YUV422 8-bit */
#define GMSL1MP_CSI2_DT_YUV422_8B 0x1e

/* Per-camera link configuration, filled in by the host frontend from DT. */
struct gmsl1mp_link_cfg {
	unsigned int link;	/* 0 = GMSL link A (HAT "GMSL0"), 1 = link B */
	unsigned int port;	/* 0 = MIPI port A, 1 = MIPI port B */
	unsigned int vc;	/* CSI-2 virtual channel emitted on @port */
	unsigned int lanes;	/* 1, 2 or 4 */
	u64 link_freq;		/* CSI-2 DDR clock in Hz (lane rate / 2) */
	u8 ser_addr;		/* 7-bit serializer address after remap */
};

/* One per physical MAX9296A, shared by up to two cameras. */
struct gmsl1mp_deser {
	struct list_head node;
	struct i2c_adapter *adap;
	u8 addr;		/* 7-bit */
	struct device *dev;	/* the I2C adapter's device, for logging */
	struct mutex lock;	/* serialises all bus traffic + state below */
	unsigned int refcount;
	unsigned int links_used;	/* bitmask of links with a camera */
	unsigned int ports_users[GMSL1MP_NUM_PORTS];	/* cameras per port */
	unsigned int ports_lanes[GMSL1MP_NUM_PORTS];
	u64 ports_freq[GMSL1MP_NUM_PORTS];
	unsigned int streaming;		/* number of cameras streaming */
};

struct gmsl1mp_deser *gmsl1mp_deser_get(struct device *dev,
					 struct i2c_adapter *adap, u8 addr);
void gmsl1mp_deser_put(struct gmsl1mp_deser *des);

int gmsl1mp_link_add(struct gmsl1mp_deser *des, struct device *dev,
		     const struct gmsl1mp_link_cfg *cfg);
void gmsl1mp_link_remove(struct gmsl1mp_deser *des,
			 const struct gmsl1mp_link_cfg *cfg);

int gmsl1mp_stream(struct gmsl1mp_deser *des,
		   const struct gmsl1mp_link_cfg *cfg, bool on);

/* Text dump of link/pipe/PHY state for sysfs. */
ssize_t gmsl1mp_status(struct gmsl1mp_deser *des,
		       const struct gmsl1mp_link_cfg *cfg, char *buf);

int gmsl1mp_des_read(struct gmsl1mp_deser *des, u16 reg, u8 *val);
int gmsl1mp_des_write(struct gmsl1mp_deser *des, u16 reg, u8 val);

#endif /* __GMSL1MP_H__ */
