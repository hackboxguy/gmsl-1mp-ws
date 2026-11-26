/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Client interface of the gmsl_max9296a deserializer driver.
 *
 * The MAX9296A driver owns the chip: reset, power over coax, link mode per
 * link (GMSL1/GMSL2), link enables, pipes and DT/VC maps, MIPI ports and the
 * per-port PHY standby that starts a port's output. Camera drivers are
 * clients: they look the ops table up once at probe, claim their GMSL link
 * (which brings it up), set up their own serializer and sensor, and start or
 * stop their MIPI port from s_stream().
 *
 * Camera repositories carry a copy of this header. The ops table is versioned
 * and checked at probe, so a mismatched copy fails cleanly instead of crashing.
 *
 * Usage in a client probe:
 *
 *	ops = gmsl_des_ops_get();          // NULL: module not loaded -> -EPROBE_DEFER
 *	des = ops->get(des_np, dev);       // ERR_PTR(-EPROBE_DEFER) until bound
 *	ops->link_info(des, link, &info);  // check endpoint lanes / rate against it
 *	ops->link_claim(des, link, dev, gmsl1, timeout_ms);
 *	... set up serializer (remap etc.) and sensor ...
 *	ops->link_release(des, link);
 *
 * and in s_stream(): ops->port_stream(des, link, on) after the sensor starts.
 */
#ifndef __GMSL_MAX9296A_H__
#define __GMSL_MAX9296A_H__

#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/types.h>

#define GMSL_DES_OPS_VERSION	1

#define GMSL_DES_NUM_LINKS	2	/* link A / link B */
#define GMSL_DES_NUM_PORTS	2	/* MIPI port A / port B */
#define GMSL_DES_NUM_PIPES	4	/* video pipes X, Y, Z, U */

enum gmsl_des_mode {
	GMSL_DES_MODE_GMSL1 = 1,
	GMSL_DES_MODE_GMSL2 = 2,
};

/* Per-link configuration from the deserializer's device-tree node. */
struct gmsl_des_link_info {
	u32 mode;		/* enum gmsl_des_mode */
	u32 pipe;		/* 0..3 = X, Y, Z, U */
	u32 stream_id;		/* GMSL2: serializer TX stream ID */
	u32 data_type;		/* CSI-2 data type of the video */
	u32 vc;			/* CSI-2 virtual channel on the MIPI port */
	u32 mipi_port;		/* 0 = port A, 1 = port B */
	u32 lanes;		/* of that MIPI port */
	u64 link_freq;		/* of that MIPI port, Hz (lane rate / 2) */
};

/*
 * One register write into the link's GMSL1 register block (0x0b00 for link A,
 * 0x0c00 for link B): @reg is the offset inside that block.
 */
struct gmsl_des_reg {
	u16 reg;
	u8 val;
	u16 delay_ms;		/* sleep after the write */
};

/*
 * Camera-type settings for a GMSL1 link, handed over by the camera driver at
 * claim time. The registers are written before the link is switched to GMSL1
 * mode (the order matters: switching first can hang the deserializer's I2C).
 */
struct gmsl_des_gmsl1_cfg {
	const struct gmsl_des_reg *regs;	/* GMSL1 block, link-relative */
	unsigned int num_regs;
	u8 bpp;			/* soft BPP override for the link's pipe */
	bool yuv422_8_10_mux;	/* YUV422 8/10-bit muxed mode on the link's pipe */
};

struct gmsl_des;

struct gmsl_des_ops {
	u32 version;		/* GMSL_DES_OPS_VERSION */

	/* Reference the deserializer described by @np; @client = camera device. */
	struct gmsl_des *(*get)(struct device_node *np, struct device *client);
	void (*put)(struct gmsl_des *des, struct device *client);

	int (*link_info)(struct gmsl_des *des, u32 link,
			 struct gmsl_des_link_info *info);

	/*
	 * Bring @link up for @client: GMSL1 settings (@gmsl1, required for a
	 * GMSL1 link, NULL for GMSL2), link mode, pipe and maps, link enable,
	 * data-path reset, wait for lock. Returns with link bring-up held
	 * exclusively, so no other link comes up while the client sets up its
	 * serializer (e.g. moves it off a default address); end that with
	 * link_release(). Fails with -EBUSY while any MIPI port is streaming.
	 */
	int (*link_claim)(struct gmsl_des *des, u32 link, struct device *client,
			  const struct gmsl_des_gmsl1_cfg *gmsl1,
			  unsigned int timeout_ms);
	void (*link_release)(struct gmsl_des *des, u32 link);
	/* Give the link up (client remove). */
	void (*link_drop)(struct gmsl_des *des, u32 link);

	int (*link_locked)(struct gmsl_des *des, u32 link);

	/*
	 * Start (@on) or stop the MIPI port @link's video goes out on: releases
	 * or holds the port's PHYs in standby, refcounted per port. Call it from
	 * s_stream() once the CSI-2 receiver is running.
	 */
	int (*port_stream)(struct gmsl_des *des, u32 link, bool on);

	/* Text dump of the link's deserializer state, for a client sysfs file. */
	ssize_t (*status)(struct gmsl_des *des, u32 link, char *buf);
};

extern const struct gmsl_des_ops gmsl_max9296a_ops;

/*
 * Look the ops table up without a link-time dependency on the module.
 * Returns NULL if the module is not (yet) available; pair with
 * gmsl_des_ops_put().
 */
static inline const struct gmsl_des_ops *gmsl_des_ops_get(void)
{
	const struct gmsl_des_ops *ops;

	ops = symbol_get(gmsl_max9296a_ops);
	if (!ops) {
		request_module("gmsl_max9296a");
		ops = symbol_get(gmsl_max9296a_ops);
	}
	return ops;
}

static inline void gmsl_des_ops_put(void)
{
	symbol_put(gmsl_max9296a_ops);
}

#endif /* __GMSL_MAX9296A_H__ */
