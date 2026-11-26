/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Waveshare GMSL-1MP-Camera-A: MAX96701 GMSL1 serializer + on-camera ISP,
 * YUV422 1280x960@30.
 *
 * The camera is a client of the gmsl_max9296a deserializer driver
 * (gmsl_max9296a.h, a copy of the header from the gmsl-max9296a repo):
 *   gmsl1mp_ser.c   - MAX96701 address remap and configuration, plus the
 *                     camera's GMSL1 settings handed to the deserializer;
 *   gmsl1mp_v4l2.c  - V4L2 async sensor subdev.
 */
#ifndef __GMSL1MP_H__
#define __GMSL1MP_H__

#include <linux/i2c.h>
#include <linux/types.h>

#include "gmsl_max9296a.h"

#define GMSL1MP_SER_DEFAULT_ADDR 0x40	/* MAX96701 power-on 7-bit address */

/* GMSL1 deserializer settings this camera type needs on its link. */
extern const struct gmsl_des_gmsl1_cfg gmsl1mp_gmsl1_cfg;

/* Move the serializer from its default address to @addr and configure it. */
int gmsl1mp_ser_setup(struct device *dev, struct i2c_adapter *adap, u8 addr,
		      u32 link);
int gmsl1mp_ser_id(struct i2c_adapter *adap, u8 addr, u8 *id);

#endif /* __GMSL1MP_H__ */
