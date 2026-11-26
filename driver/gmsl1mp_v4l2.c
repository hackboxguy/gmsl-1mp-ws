// SPDX-License-Identifier: GPL-2.0
/*
 * Generic V4L2 frontend for the Waveshare GMSL-1MP-Camera-A: one async
 * sensor subdev per camera (= per GMSL link), each wired by DT to its own
 * CSI-2 receiver endpoint. Targets mainline-style receivers (Raspberry Pi 5
 * rp1-cfe, ...). Requires kernel >= 6.8 (subdev state / init_state API).
 *
 * DT node (one per camera, on the I2C bus the MAX9296A sits on):
 *
 *   camera@42 {
 *       compatible = "waveshare,gmsl-1mp-camera-a";
 *       reg = <0x42>;          // serializer 7-bit address after remap
 *       deser-addr = <0x48>;   // MAX9296A 7-bit address (default 0x48)
 *       gmsl-link = <0>;       // 0 = link A (HAT GMSL0), 1 = link B
 *       deser-port = <1>;      // MAX9296A MIPI port: 0 = A, 1 = B
 *       vc-id = <0>;           // CSI-2 virtual channel (default 0)
 *       port { endpoint {
 *           data-lanes = <1 2 3 4>;
 *           link-frequencies = /bits/ 64 <300000000>;
 *           remote-endpoint = <&csi_ep>;
 *       }; };
 *   };
 */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/version.h>
#include <media/mipi-csi2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "gmsl1mp.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
#error "gmsl1mp needs Linux >= 6.8"
#endif

#define GMSL1MP_WIDTH		1280
#define GMSL1MP_HEIGHT		960
#define GMSL1MP_MBUS_CODE	MEDIA_BUS_FMT_UYVY8_1X16
#define GMSL1MP_BPP		16

struct gmsl1mp {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct gmsl1mp_deser *des;
	struct gmsl1mp_link_cfg cfg;
	s64 link_freq_menu[1];
	bool streaming;
};

static inline struct gmsl1mp *to_gmsl1mp(struct v4l2_subdev *sd)
{
	return container_of(sd, struct gmsl1mp, sd);
}

static void gmsl1mp_default_fmt(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = GMSL1MP_WIDTH;
	fmt->height = GMSL1MP_HEIGHT;
	fmt->code = GMSL1MP_MBUS_CODE;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SMPTE170M;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_709;
}

static int gmsl1mp_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	gmsl1mp_default_fmt(v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static int gmsl1mp_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index)
		return -EINVAL;
	code->code = GMSL1MP_MBUS_CODE;
	return 0;
}

static int gmsl1mp_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->pad || fse->index || fse->code != GMSL1MP_MBUS_CODE)
		return -EINVAL;
	fse->min_width = fse->max_width = GMSL1MP_WIDTH;
	fse->min_height = fse->max_height = GMSL1MP_HEIGHT;
	return 0;
}

/* Single fixed mode: any request is answered with it. */
static int gmsl1mp_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *f = v4l2_subdev_state_get_format(state, 0);

	if (fmt->pad)
		return -EINVAL;
	gmsl1mp_default_fmt(f);
	fmt->format = *f;
	return 0;
}

static int gmsl1mp_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
				  struct v4l2_mbus_frame_desc *fd)
{
	struct gmsl1mp *priv = to_gmsl1mp(sd);

	if (pad)
		return -EINVAL;
	memset(fd, 0, sizeof(*fd));
	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;
	fd->num_entries = 1;
	fd->entry[0].stream = 0;
	fd->entry[0].pixelcode = GMSL1MP_MBUS_CODE;
	fd->entry[0].bus.csi2.vc = priv->cfg.vc;
	fd->entry[0].bus.csi2.dt = MIPI_CSI2_DT_YUV422_8B;
	return 0;
}

static int gmsl1mp_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct gmsl1mp *priv = to_gmsl1mp(sd);
	int ret;

	if (priv->streaming == !!enable)
		return 0;
	ret = gmsl1mp_stream(priv->des, &priv->cfg, enable);
	if (!ret)
		priv->streaming = !!enable;
	return ret;
}

static const struct v4l2_subdev_video_ops gmsl1mp_video_ops = {
	.s_stream = gmsl1mp_s_stream,
};

static const struct v4l2_subdev_pad_ops gmsl1mp_pad_ops = {
	.enum_mbus_code = gmsl1mp_enum_mbus_code,
	.enum_frame_size = gmsl1mp_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = gmsl1mp_set_fmt,
	.get_frame_desc = gmsl1mp_get_frame_desc,
};

static const struct v4l2_subdev_core_ops gmsl1mp_core_ops = {
	.log_status = v4l2_ctrl_subdev_log_status,
};

static const struct v4l2_subdev_ops gmsl1mp_subdev_ops = {
	.core = &gmsl1mp_core_ops,
	.video = &gmsl1mp_video_ops,
	.pad = &gmsl1mp_pad_ops,
};

static const struct v4l2_subdev_internal_ops gmsl1mp_internal_ops = {
	.init_state = gmsl1mp_init_state,
};

static const struct media_entity_operations gmsl1mp_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/* ------------------------------------------------------------------ */

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(to_i2c_client(dev));
	struct gmsl1mp *priv = to_gmsl1mp(sd);

	return gmsl1mp_status(priv->des, &priv->cfg, buf);
}
static DEVICE_ATTR_RO(status);

static int gmsl1mp_parse_dt(struct gmsl1mp *priv, struct i2c_client *client,
			    u8 *deser_addr)
{
	struct device *dev = &client->dev;
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *epn;
	u32 v;
	int ret;

	priv->cfg.ser_addr = client->addr;

	v = 0x48;
	device_property_read_u32(dev, "deser-addr", &v);
	*deser_addr = v;
	v = 0;
	device_property_read_u32(dev, "gmsl-link", &v);
	priv->cfg.link = v;
	v = 0;
	device_property_read_u32(dev, "deser-port", &v);
	priv->cfg.port = v;
	v = 0;
	device_property_read_u32(dev, "vc-id", &v);
	priv->cfg.vc = v;

	if (priv->cfg.link > 1 || priv->cfg.port > 1 || priv->cfg.vc > 3)
		return dev_err_probe(dev, -EINVAL,
				     "bad gmsl-link/deser-port/vc-id\n");
	if (*deser_addr == client->addr ||
	    client->addr == GMSL1MP_SER_DEFAULT_ADDR)
		return dev_err_probe(dev, -EINVAL,
				     "reg (0x%02x) must be the serializer's remap address, not 0x%02x/0x%02x\n",
				     client->addr, *deser_addr,
				     GMSL1MP_SER_DEFAULT_ADDR);

	epn = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!epn)
		return dev_err_probe(dev, -EINVAL, "no endpoint\n");
	ret = v4l2_fwnode_endpoint_alloc_parse(epn, &ep);
	fwnode_handle_put(epn);
	if (ret)
		return dev_err_probe(dev, ret, "endpoint parse failed\n");

	priv->cfg.lanes = ep.bus.mipi_csi2.num_data_lanes;
	if (ep.nr_of_link_frequencies != 1) {
		ret = dev_err_probe(dev, -EINVAL,
				    "need exactly one link-frequencies entry\n");
		goto out;
	}
	priv->cfg.link_freq = ep.link_frequencies[0];
	priv->link_freq_menu[0] = ep.link_frequencies[0];
out:
	v4l2_fwnode_endpoint_free(&ep);
	return ret;
}

static int gmsl1mp_init_controls(struct gmsl1mp *priv)
{
	struct v4l2_ctrl_handler *h = &priv->ctrls;
	struct v4l2_ctrl *c;
	u64 pixel_rate = div_u64(priv->cfg.link_freq * 2 * priv->cfg.lanes,
				 GMSL1MP_BPP);

	v4l2_ctrl_handler_init(h, 2);
	c = v4l2_ctrl_new_int_menu(h, NULL, V4L2_CID_LINK_FREQ, 0, 0,
				   priv->link_freq_menu);
	if (c)
		c->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(h, NULL, V4L2_CID_PIXEL_RATE, pixel_rate, pixel_rate,
			  1, pixel_rate);
	if (h->error)
		return h->error;
	priv->sd.ctrl_handler = h;
	return 0;
}

static int gmsl1mp_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gmsl1mp *priv;
	u8 deser_addr;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;

	ret = gmsl1mp_parse_dt(priv, client, &deser_addr);
	if (ret)
		return ret;

	priv->des = gmsl1mp_deser_get(dev, client->adapter, deser_addr);
	if (IS_ERR(priv->des))
		return dev_err_probe(dev, PTR_ERR(priv->des),
				     "deserializer init failed\n");

	ret = gmsl1mp_link_add(priv->des, dev, &priv->cfg);
	if (ret)
		goto err_put;

	v4l2_i2c_subdev_init(&priv->sd, client, &gmsl1mp_subdev_ops);
	snprintf(priv->sd.name, sizeof(priv->sd.name), "gmsl1mp %c %d-%04x",
		 'a' + priv->cfg.link, i2c_adapter_id(client->adapter),
		 client->addr);
	priv->sd.internal_ops = &gmsl1mp_internal_ops;
	priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	priv->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	priv->sd.entity.ops = &gmsl1mp_entity_ops;

	ret = gmsl1mp_init_controls(priv);
	if (ret)
		goto err_ctrls;

	priv->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&priv->sd.entity, 1, &priv->pad);
	if (ret)
		goto err_ctrls;

	priv->sd.state_lock = priv->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&priv->sd);
	if (ret)
		goto err_entity;

	ret = device_create_file(dev, &dev_attr_status);
	if (ret)
		goto err_state;

	ret = v4l2_async_register_subdev_sensor(&priv->sd);
	if (ret) {
		dev_err_probe(dev, ret, "async subdev register failed\n");
		goto err_file;
	}
	return 0;

err_file:
	device_remove_file(dev, &dev_attr_status);
err_state:
	v4l2_subdev_cleanup(&priv->sd);
err_entity:
	media_entity_cleanup(&priv->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&priv->ctrls);
	gmsl1mp_link_remove(priv->des, &priv->cfg);
err_put:
	gmsl1mp_deser_put(priv->des);
	return ret;
}

static void gmsl1mp_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gmsl1mp *priv = to_gmsl1mp(sd);

	v4l2_async_unregister_subdev(sd);
	device_remove_file(&client->dev, &dev_attr_status);
	if (priv->streaming)
		gmsl1mp_stream(priv->des, &priv->cfg, false);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&priv->ctrls);
	gmsl1mp_link_remove(priv->des, &priv->cfg);
	gmsl1mp_deser_put(priv->des);
}

static const struct of_device_id gmsl1mp_of_match[] = {
	{ .compatible = "waveshare,gmsl-1mp-camera-a" },
	{ }
};
MODULE_DEVICE_TABLE(of, gmsl1mp_of_match);

static struct i2c_driver gmsl1mp_driver = {
	.driver = {
		.name = "gmsl1mp",
		.of_match_table = gmsl1mp_of_match,
	},
	.probe = gmsl1mp_probe,
	.remove = gmsl1mp_remove,
};
module_i2c_driver(gmsl1mp_driver);

MODULE_DESCRIPTION("Waveshare GMSL-1MP-Camera-A (MAX96701) via MAX9296A");
MODULE_LICENSE("GPL");
