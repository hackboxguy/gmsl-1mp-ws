// SPDX-License-Identifier: GPL-2.0
/*
 * Generic V4L2 frontend for the Waveshare GMSL-1MP-Camera-A: one async
 * sensor subdev per camera (= per GMSL link), each wired by DT to its own
 * CSI-2 receiver endpoint. Targets mainline-style receivers (Raspberry Pi 5
 * rp1-cfe, ...). Requires kernel >= 6.8 (subdev state / init_state API).
 *
 * The camera is a client of the gmsl_max9296a deserializer driver, which
 * owns link, pipe, MIPI port and VC routing (from its own DT node).
 *
 * DT node (one per camera, on the I2C bus the MAX9296A sits on):
 *
 *   camera@42 {
 *       compatible = "waveshare,gmsl-1mp-camera-a";
 *       reg = <0x42>;                       // serializer 7-bit address after remap
 *       gmsl,deserializer = <&max9296a>;    // "gmsl,max9296a-hub" node
 *       gmsl,link = <0>;                    // 0 = link A (HAT GMSL0), 1 = link B
 *       port { endpoint {
 *           data-lanes = <1 2 3 4>;         // must match the deserializer's MIPI port
 *           link-frequencies = /bits/ 64 <300000000>;
 *           remote-endpoint = <&csi_ep>;
 *       }; };
 *   };
 */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/sysfs.h>
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
	const struct gmsl_des_ops *ops;
	struct gmsl_des *des;
	struct gmsl_des_link_info info;
	u32 link;
	u8 ser_addr;
	u32 lanes;
	u64 link_freq;
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
	fd->entry[0].bus.csi2.vc = priv->info.vc;
	fd->entry[0].bus.csi2.dt = MIPI_CSI2_DT_YUV422_8B;
	return 0;
}

static int gmsl1mp_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct gmsl1mp *priv = to_gmsl1mp(sd);
	int ret;

	if (priv->streaming == !!enable)
		return 0;
	/*
	 * The serializer streams continuously; starting means starting the
	 * deserializer's MIPI port now that the receiver runs. A failure here
	 * is logged but not returned: the stream has nothing else to fail on.
	 */
	ret = priv->ops->port_stream(priv->des, priv->link, enable);
	if (ret)
		dev_warn(priv->dev, "MIPI port %s failed (%d)\n",
			 enable ? "start" : "stop", ret);
	priv->streaming = !!enable;
	return 0;
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
	struct i2c_client *client = to_i2c_client(dev);
	ssize_t n;
	u8 id = 0;
	int ret;

	n = priv->ops->status(priv->des, priv->link, buf);
	ret = gmsl1mp_ser_id(client->adapter, priv->ser_addr, &id);
	return n + sysfs_emit_at(buf, n, "ser_addr=0x%02x ser_id=%s0x%02x\n",
				 priv->ser_addr, ret ? "ERR " : "", id);
}
static DEVICE_ATTR_RO(status);

static int gmsl1mp_parse_dt(struct gmsl1mp *priv, struct i2c_client *client,
			    struct device_node **des_np)
{
	struct device *dev = &client->dev;
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *epn;
	int ret;

	priv->ser_addr = client->addr;
	if (client->addr == GMSL1MP_SER_DEFAULT_ADDR)
		return dev_err_probe(dev, -EINVAL,
				     "reg must be the serializer's remap address, not 0x%02x\n",
				     GMSL1MP_SER_DEFAULT_ADDR);

	*des_np = of_parse_phandle(dev->of_node, "gmsl,deserializer", 0);
	if (!*des_np)
		return dev_err_probe(dev, -EINVAL, "no gmsl,deserializer phandle\n");
	priv->link = 0;
	of_property_read_u32(dev->of_node, "gmsl,link", &priv->link);

	epn = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!epn) {
		ret = dev_err_probe(dev, -EINVAL, "no endpoint\n");
		goto err_np;
	}
	ret = v4l2_fwnode_endpoint_alloc_parse(epn, &ep);
	fwnode_handle_put(epn);
	if (ret) {
		dev_err_probe(dev, ret, "endpoint parse failed\n");
		goto err_np;
	}
	priv->lanes = ep.bus.mipi_csi2.num_data_lanes;
	if (ep.nr_of_link_frequencies != 1) {
		ret = dev_err_probe(dev, -EINVAL, "need exactly one link-frequencies entry\n");
		v4l2_fwnode_endpoint_free(&ep);
		goto err_np;
	}
	priv->link_freq = ep.link_frequencies[0];
	priv->link_freq_menu[0] = ep.link_frequencies[0];
	v4l2_fwnode_endpoint_free(&ep);
	return 0;

err_np:
	of_node_put(*des_np);
	return ret;
}

/* Look up the deserializer, check its view of our link, bring the link up. */
static int gmsl1mp_des_attach(struct gmsl1mp *priv, struct i2c_client *client,
			      struct device_node *des_np)
{
	struct device *dev = priv->dev;
	int ret;

	priv->ops = gmsl_des_ops_get();
	if (!priv->ops)
		return dev_err_probe(dev, -EPROBE_DEFER, "gmsl_max9296a not loaded\n");
	if (priv->ops->version != GMSL_DES_OPS_VERSION) {
		ret = dev_err_probe(dev, -EINVAL, "gmsl_max9296a interface v%u, expected v%u\n",
				    priv->ops->version, GMSL_DES_OPS_VERSION);
		goto err_ops;
	}
	priv->des = priv->ops->get(des_np, dev);
	if (IS_ERR(priv->des)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->des), "deserializer not ready\n");
		goto err_ops;
	}
	ret = priv->ops->link_info(priv->des, priv->link, &priv->info);
	if (ret) {
		dev_err_probe(dev, ret, "link %u not described by the deserializer\n", priv->link);
		goto err_put;
	}
	if (priv->info.mode != GMSL_DES_MODE_GMSL1 ||
	    priv->info.data_type != MIPI_CSI2_DT_YUV422_8B ||
	    priv->info.lanes != priv->lanes || priv->info.link_freq != priv->link_freq) {
		ret = dev_err_probe(dev, -EINVAL,
				    "deserializer link %c must be gmsl1, DT 0x1e, %u lanes @ %llu Hz (is mode %u, DT 0x%02x, %u lanes @ %llu Hz)\n",
				    'A' + priv->link, priv->lanes, priv->link_freq,
				    priv->info.mode, priv->info.data_type,
				    priv->info.lanes, priv->info.link_freq);
		goto err_put;
	}

	ret = priv->ops->link_claim(priv->des, priv->link, dev, &gmsl1mp_gmsl1_cfg, 1000);
	if (ret) {
		dev_err_probe(dev, ret, "link %c bring-up failed\n", 'A' + priv->link);
		goto err_put;
	}
	ret = gmsl1mp_ser_setup(dev, client->adapter, priv->ser_addr, priv->link);
	priv->ops->link_release(priv->des, priv->link);
	if (ret)
		goto err_drop;
	return 0;

err_drop:
	priv->ops->link_drop(priv->des, priv->link);
err_put:
	priv->ops->put(priv->des, dev);
err_ops:
	gmsl_des_ops_put();
	return ret;
}

static void gmsl1mp_des_detach(struct gmsl1mp *priv)
{
	priv->ops->link_drop(priv->des, priv->link);
	priv->ops->put(priv->des, priv->dev);
	gmsl_des_ops_put();
}

static int gmsl1mp_init_controls(struct gmsl1mp *priv)
{
	struct v4l2_ctrl_handler *h = &priv->ctrls;
	struct v4l2_ctrl *c;
	u64 pixel_rate = div_u64(priv->link_freq * 2 * priv->lanes, GMSL1MP_BPP);

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
	struct device_node *des_np;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;

	ret = gmsl1mp_parse_dt(priv, client, &des_np);
	if (ret)
		return ret;
	ret = gmsl1mp_des_attach(priv, client, des_np);
	of_node_put(des_np);
	if (ret)
		return ret;

	v4l2_i2c_subdev_init(&priv->sd, client, &gmsl1mp_subdev_ops);
	snprintf(priv->sd.name, sizeof(priv->sd.name), "gmsl1mp %c %d-%04x",
		 'a' + priv->link, i2c_adapter_id(client->adapter),
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
	gmsl1mp_des_detach(priv);
	return ret;
}

static void gmsl1mp_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gmsl1mp *priv = to_gmsl1mp(sd);

	v4l2_async_unregister_subdev(sd);
	device_remove_file(&client->dev, &dev_attr_status);
	if (priv->streaming)
		priv->ops->port_stream(priv->des, priv->link, false);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&priv->ctrls);
	gmsl1mp_des_detach(priv);
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

MODULE_DESCRIPTION("Waveshare GMSL-1MP-Camera-A (MAX96701), client of gmsl_max9296a");
MODULE_LICENSE("GPL");
