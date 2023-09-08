// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for the sinowealth SH1106 OLED controller, based on the SSD1307 driver
 *
 * Copyright 2012 Free Electrons
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pwm.h>
#include <linux/uaccess.h>
#include <linux/regulator/consumer.h>

#define SH1106FB_DATA			0x40
#define SH1106FB_COMMAND		0x00

#define SH1106FB_DISPLAY_OFF		0xAE
#define SH1106FB_DISPLAY_ON		0xAF

#define	SH1106FB_SET_CLOCK_FREQ		0xD5
#define SH1106FB_SET_MULTIPLEX_RATIO	0xA8
#define SH1106FB_SET_DISPLAY_OFFSET	0xD3
#define	SH1106FB_CHARGE_PUMP		0x8D
#define SH1106FB_SET_ADDRESS_MODE	0x20
#define SH1106FB_SET_ADDRESS_MODE_HORIZONTAL	0x00
#define SH1106FB_SET_ADDRESS_MODE_VERTICAL	0x01
#define SH1106FB_SET_ADDRESS_MODE_PAGE		0x02
#define SH1106FB_SEG_REMAP_ON		0xA1
#define SH1106FB_SET_COM_SCAN_DIRECTION	0xC8
#define	SH1106FB_SET_COM_PINS_CONFIG	0xDA
#define SH1106FB_CONTRAST		0x81
#define	SH1106FB_SET_PRECHARGE_PERIOD	0xD9
#define	SH1106FB_SET_VCOMH		0xDB

#define MAX_CONTRAST 255

#define REFRESHRATE 1

static u_int refreshrate = REFRESHRATE;
module_param(refreshrate, uint, 0);

struct sh1106fb_par;

struct sh1106fb_deviceinfo {
	u32 default_vcomh;
	u32 default_dclk_div;
	u32 default_dclk_frq;
	int need_pwm;
	int need_chargepump;
};

struct sh1106fb_par {
	unsigned area_color_enable : 1;
	unsigned com_invdir : 1;
	unsigned com_lrremap : 1;
	unsigned com_seq : 1;
	unsigned lookup_table_set : 1;
	unsigned low_power : 1;
	unsigned seg_remap : 1;
	u32 com_offset;
	u32 contrast;
	u32 dclk_div;
	u32 dclk_frq;
	const struct sh1106fb_deviceinfo *device_info;
	struct i2c_client *client;
	u32 height;
	struct fb_info *info;
	u8 lookup_table[4];
	u32 page_offset;
	u32 prechargep1;
	u32 prechargep2;
	struct pwm_device *pwm;
	u32 pwm_period;
	struct gpio_desc *reset;
	struct regulator *vbat_reg;
	u32 vcomh;
	u32 width;
};

struct sh1106fb_array {
	u8	type;
	u8	data[0];
};

static const struct fb_fix_screeninfo sh1106fb_fix = {
	.id		= "SH1106",
	.type		= FB_TYPE_PACKED_PIXELS,
	.visual		= FB_VISUAL_MONO10,
	.xpanstep	= 0,
	.ypanstep	= 0,
	.ywrapstep	= 0,
	.accel		= FB_ACCEL_NONE,
};

static const struct fb_var_screeninfo sh1106fb_var = {
	.bits_per_pixel	= 1,
	.red = { .length = 1 },
	.green = { .length = 1 },
	.blue = { .length = 1 },
};

static struct sh1106fb_array *sh1106fb_alloc_array(u32 len, u8 type)
{
	struct sh1106fb_array *array;

	array = kzalloc(sizeof(struct sh1106fb_array) + len, GFP_KERNEL);
	if (!array)
		return NULL;

	array->type = type;

	return array;
}

static int sh1106fb_write_array(struct i2c_client *client,
				 struct sh1106fb_array *array, u32 len)
{
	int ret;

	len += sizeof(struct sh1106fb_array);

	ret = i2c_master_send(client, (u8 *)array, len);
	if (ret != len) {
		dev_err(&client->dev, "Couldn't send I2C command.\n");
		return ret;
	}

	return 0;
}

static inline int sh1106fb_write_cmd(struct i2c_client *client, u8 cmd)
{
	struct sh1106fb_array *array;
	int ret;

	array = sh1106fb_alloc_array(1, SH1106FB_COMMAND);
	if (!array)
		return -ENOMEM;

	array->data[0] = cmd;

	ret = sh1106fb_write_array(client, array, 1);
	kfree(array);

	return ret;
}

static void sh1106fb_update_display(struct sh1106fb_par *par)
{
	struct sh1106fb_array *array;
	u8 *vmem = par->info->screen_buffer;
	unsigned int line_length = par->info->fix.line_length;
	unsigned int pages = DIV_ROUND_UP(par->height, 8);
	int i, j, k;

	/*
	 * The screen is divided in pages, each having a height of 8
	 * pixels, and the width of the screen. When sending a byte of
	 * data to the controller, it gives the 8 bits for the current
	 * column. I.e, the first byte are the 8 bits of the first
	 * column, then the 8 bits for the second column, etc.
	 *
	 *
	 * Representation of the screen, assuming it is 5 bits
	 * wide. Each letter-number combination is a bit that controls
	 * one pixel.
	 *
	 * A0 A1 A2 A3 A4
	 * B0 B1 B2 B3 B4
	 * C0 C1 C2 C3 C4
	 * D0 D1 D2 D3 D4
	 * E0 E1 E2 E3 E4
	 * F0 F1 F2 F3 F4
	 * G0 G1 G2 G3 G4
	 * H0 H1 H2 H3 H4
	 *
	 * If you want to update this screen, you need to send 5 bytes:
	 *  (1) A0 B0 C0 D0 E0 F0 G0 H0
	 *  (2) A1 B1 C1 D1 E1 F1 G1 H1
	 *  (3) A2 B2 C2 D2 E2 F2 G2 H2
	 *  (4) A3 B3 C3 D3 E3 F3 G3 H3
	 *  (5) A4 B4 C4 D4 E4 F4 G4 H4
	 */

	for (i = 0; i < pages; i++) {
		array = sh1106fb_alloc_array(par->width, SH1106FB_DATA);
		if (!array)
			return;
		sh1106fb_write_cmd(par->client, 0xb0+i);
		sh1106fb_write_cmd(par->client, 0x10);
		sh1106fb_write_cmd(par->client, 0x02);
		for (j = 0; j < par->width; j++) {
			int m = 8;
			u32 array_idx = j;
			array->data[array_idx] = 0;
			/* Last page may be partial */
			if (i + 1 == pages && par->height % 8)
				m = par->height % 8;
			for (k = 0; k < m; k++) {
				u8 byte = vmem[(8 * i + k) * line_length +
					       j / 8];
				u8 bit = (byte >> (j % 8)) & 1;
				array->data[array_idx] |= bit << k;
			}
		}
		sh1106fb_write_array(par->client, array, par->width);
		kfree(array);
	}
}


static ssize_t sh1106fb_write(struct fb_info *info, const char __user *buf,
		size_t count, loff_t *ppos)
{
	struct sh1106fb_par *par = info->par;
	unsigned long total_size;
	unsigned long p = *ppos;
	void *dst;

	total_size = info->fix.smem_len;

	if (p > total_size)
		return -EINVAL;

	if (count + p > total_size)
		count = total_size - p;

	if (!count)
		return -EINVAL;

	dst = info->screen_buffer + p;

	if (copy_from_user(dst, buf, count))
		return -EFAULT;

	sh1106fb_update_display(par);

	*ppos += count;

	return count;
}

static int sh1106fb_blank(int blank_mode, struct fb_info *info)
{
	struct sh1106fb_par *par = info->par;

	if (blank_mode != FB_BLANK_UNBLANK)
		return sh1106fb_write_cmd(par->client, SH1106FB_DISPLAY_OFF);
	else
		return sh1106fb_write_cmd(par->client, SH1106FB_DISPLAY_ON);
}

static void sh1106fb_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
	struct sh1106fb_par *par = info->par;
	sys_fillrect(info, rect);
	sh1106fb_update_display(par);
}

static void sh1106fb_copyarea(struct fb_info *info, const struct fb_copyarea *area)
{
	struct sh1106fb_par *par = info->par;
	sys_copyarea(info, area);
	sh1106fb_update_display(par);
}

static void sh1106fb_imageblit(struct fb_info *info, const struct fb_image *image)
{
	struct sh1106fb_par *par = info->par;
	sys_imageblit(info, image);
	sh1106fb_update_display(par);
}

static struct fb_ops sh1106fb_ops = {
	.owner		= THIS_MODULE,
	.fb_read	= fb_sys_read,
	.fb_write	= sh1106fb_write,
	.fb_blank	= sh1106fb_blank,
	.fb_fillrect	= sh1106fb_fillrect,
	.fb_copyarea	= sh1106fb_copyarea,
	.fb_imageblit	= sh1106fb_imageblit,
};

static void sh1106fb_deferred_io(struct fb_info *info,
				struct list_head *pagelist)
{
	sh1106fb_update_display(info->par);
}

static int sh1106fb_init(struct sh1106fb_par *par)
{
	sh1106fb_write_cmd(par->client, SH1106FB_DISPLAY_OFF);

	sh1106fb_write_cmd(par->client, SH1106FB_SET_CLOCK_FREQ);
	sh1106fb_write_cmd(par->client, 0x80);

	sh1106fb_write_cmd(par->client, SH1106FB_SET_MULTIPLEX_RATIO);
	sh1106fb_write_cmd(par->client, 0X3F);

	sh1106fb_write_cmd(par->client, SH1106FB_SET_DISPLAY_OFFSET);
	sh1106fb_write_cmd(par->client, 0X00);

	sh1106fb_write_cmd(par->client, 0x40);//set page start address

	sh1106fb_write_cmd(par->client, SH1106FB_CHARGE_PUMP);
	sh1106fb_write_cmd(par->client, 0x14);

	sh1106fb_write_cmd(par->client, SH1106FB_SET_ADDRESS_MODE);
	sh1106fb_write_cmd(par->client, SH1106FB_SET_ADDRESS_MODE_PAGE);
	sh1106fb_write_cmd(par->client, SH1106FB_SEG_REMAP_ON);

	sh1106fb_write_cmd(par->client, SH1106FB_SET_COM_SCAN_DIRECTION);

	sh1106fb_write_cmd(par->client, SH1106FB_SET_COM_PINS_CONFIG);
	sh1106fb_write_cmd(par->client, 0x12);

	sh1106fb_write_cmd(par->client, SH1106FB_CONTRAST);//range 1~255
	sh1106fb_write_cmd(par->client, 0x7F);

	sh1106fb_write_cmd(par->client, SH1106FB_SET_PRECHARGE_PERIOD);
	sh1106fb_write_cmd(par->client, 0xF1);

	sh1106fb_write_cmd(par->client, SH1106FB_SET_VCOMH);
	sh1106fb_write_cmd(par->client, 0x30);

	sh1106fb_write_cmd(par->client, 0xA4);//global display on

	sh1106fb_write_cmd(par->client, 0xA6);//set display mode normall

	sh1106fb_write_cmd(par->client, SH1106FB_DISPLAY_ON);

	return 0;
}
static int sh1106fb_update_bl(struct backlight_device *bdev)
{
	struct sh1106fb_par *par = bl_get_data(bdev);
	int ret;
	int brightness = bdev->props.brightness;

	par->contrast = brightness;

	ret = sh1106fb_write_cmd(par->client, SH1106FB_CONTRAST);
	if (ret < 0)
		return ret;
	ret = sh1106fb_write_cmd(par->client, par->contrast);
	if (ret < 0)
		return ret;
	return 0;
}

static int sh1106fb_get_brightness(struct backlight_device *bdev)
{
	struct sh1106fb_par *par = bl_get_data(bdev);

	return par->contrast;
}

static int sh1106fb_check_fb(struct backlight_device *bdev,
				   struct fb_info *info)
{
	return (info->bl_dev == bdev);
}

static const struct backlight_ops sh1106fb_bl_ops = {
	.options	= BL_CORE_SUSPENDRESUME,
	.update_status	= sh1106fb_update_bl,
	.get_brightness	= sh1106fb_get_brightness,
	.check_fb	= sh1106fb_check_fb,
};

static struct sh1106fb_deviceinfo sh1106fb_sh1106_deviceinfo = {
	.default_vcomh = 0x30,
	.default_dclk_div = 1,
	.default_dclk_frq = 8,
	.need_chargepump = 1,
};

static const struct of_device_id sh1106fb_of_match[] = {
	{
		.compatible = "sinowealth,sh1106fb-i2c",
		.data = (void *)&sh1106fb_sh1106_deviceinfo,
	},
	{},
};
MODULE_DEVICE_TABLE(of, sh1106fb_of_match);

static int sh1106fb_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct backlight_device *bl;
	char bl_name[12];
	struct fb_info *info;
	struct device_node *node = client->dev.of_node;
	struct fb_deferred_io *sh1106fb_defio;
	u32 vmem_size;
	struct sh1106fb_par *par;
	void *vmem;
	int ret;

	if (!node) {
		dev_err(&client->dev, "No device tree data found!\n");
		return -EINVAL;
	}

	info = framebuffer_alloc(sizeof(struct sh1106fb_par), &client->dev);
	if (!info)
		return -ENOMEM;

	par = info->par;
	par->info = info;
	par->client = client;

	par->device_info = of_device_get_match_data(&client->dev);

	par->reset = devm_gpiod_get_optional(&client->dev, "reset",
					     GPIOD_OUT_LOW);
	if (IS_ERR(par->reset)) {
		dev_err(&client->dev, "failed to get reset gpio: %ld\n",
			PTR_ERR(par->reset));
		ret = PTR_ERR(par->reset);
		goto fb_alloc_error;
	}

	par->vbat_reg = devm_regulator_get_optional(&client->dev, "vbat");
	if (IS_ERR(par->vbat_reg)) {
		ret = PTR_ERR(par->vbat_reg);
		if (ret == -ENODEV) {
			par->vbat_reg = NULL;
		} else {
			dev_err(&client->dev, "failed to get VBAT regulator: %d\n",
				ret);
			goto fb_alloc_error;
		}
	}

	if (of_property_read_u32(node, "sinowealth,width", &par->width))
		par->width = 96;

	if (of_property_read_u32(node, "sinowealth,height", &par->height))
		par->height = 16;

	if (of_property_read_u32(node, "sinowealth,page-offset", &par->page_offset))
		par->page_offset = 1;

	if (of_property_read_u32(node, "sinowealth,com-offset", &par->com_offset))
		par->com_offset = 0;

	if (of_property_read_u32(node, "sinowealth,prechargep1", &par->prechargep1))
		par->prechargep1 = 2;

	if (of_property_read_u32(node, "sinowealth,prechargep2", &par->prechargep2))
		par->prechargep2 = 2;

	if (!of_property_read_u8_array(node, "sinowealth,lookup-table",
				       par->lookup_table,
				       ARRAY_SIZE(par->lookup_table)))
		par->lookup_table_set = 1;

	par->seg_remap = !of_property_read_bool(node, "sinowealth,segment-no-remap");
	par->com_seq = of_property_read_bool(node, "sinowealth,com-seq");
	par->com_lrremap = of_property_read_bool(node, "sinowealth,com-lrremap");
	par->com_invdir = of_property_read_bool(node, "sinowealth,com-invdir");
	par->area_color_enable =
		of_property_read_bool(node, "sinowealth,area-color-enable");
	par->low_power = of_property_read_bool(node, "sinowealth,low-power");

	par->contrast = 127;
	par->vcomh = par->device_info->default_vcomh;

	/* Setup display timing */
	if (of_property_read_u32(node, "sinowealth,dclk-div", &par->dclk_div))
		par->dclk_div = par->device_info->default_dclk_div;
	if (of_property_read_u32(node, "sinowealth,dclk-frq", &par->dclk_frq))
		par->dclk_frq = par->device_info->default_dclk_frq;

	vmem_size = DIV_ROUND_UP(par->width, 8) * par->height;

	vmem = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
					get_order(vmem_size));
	if (!vmem) {
		dev_err(&client->dev, "Couldn't allocate graphical memory.\n");
		ret = -ENOMEM;
		goto fb_alloc_error;
	}

	sh1106fb_defio = devm_kzalloc(&client->dev, sizeof(*sh1106fb_defio),
				       GFP_KERNEL);
	if (!sh1106fb_defio) {
		dev_err(&client->dev, "Couldn't allocate deferred io.\n");
		ret = -ENOMEM;
		goto fb_alloc_error;
	}

	sh1106fb_defio->delay = HZ / refreshrate;
	sh1106fb_defio->deferred_io = sh1106fb_deferred_io;

	info->fbops = &sh1106fb_ops;
	info->fix = sh1106fb_fix;
	info->fix.line_length = DIV_ROUND_UP(par->width, 8);
	info->fbdefio = sh1106fb_defio;

	info->var = sh1106fb_var;
	info->var.xres = par->width;
	info->var.xres_virtual = par->width;
	info->var.yres = par->height;
	info->var.yres_virtual = par->height;

	info->screen_buffer = vmem;
	info->fix.smem_start = __pa(vmem);
	info->fix.smem_len = vmem_size;

	fb_deferred_io_init(info);

	i2c_set_clientdata(client, info);

	if (par->reset) {
		/* Reset the screen */
		gpiod_set_value_cansleep(par->reset, 1);
		udelay(4);
		gpiod_set_value_cansleep(par->reset, 0);
		udelay(4);
	}

	if (par->vbat_reg) {
		ret = regulator_enable(par->vbat_reg);
		if (ret) {
			dev_err(&client->dev, "failed to enable VBAT: %d\n",
				ret);
			goto reset_oled_error;
		}
	}

	ret = sh1106fb_init(par);
	if (ret)
		goto regulator_enable_error;

	ret = register_framebuffer(info);
	if (ret) {
		dev_err(&client->dev, "Couldn't register the framebuffer\n");
		goto panel_init_error;
	}

	snprintf(bl_name, sizeof(bl_name), "sh1106fb%d", info->node);
	bl = backlight_device_register(bl_name, &client->dev, par,
				       &sh1106fb_bl_ops, NULL);
	if (IS_ERR(bl)) {
		ret = PTR_ERR(bl);
		dev_err(&client->dev, "unable to register backlight device: %d\n",
			ret);
		goto bl_init_error;
	}

	bl->props.brightness = par->contrast;
	bl->props.max_brightness = MAX_CONTRAST;
	info->bl_dev = bl;

	dev_info(&client->dev, "fb%d: %s framebuffer device registered, using %d bytes of video memory\n", info->node, info->fix.id, vmem_size);

	return 0;

bl_init_error:
	unregister_framebuffer(info);
panel_init_error:
	if (par->device_info->need_pwm) {
		pwm_disable(par->pwm);
		pwm_put(par->pwm);
	}
regulator_enable_error:
	if (par->vbat_reg)
		regulator_disable(par->vbat_reg);
reset_oled_error:
	fb_deferred_io_cleanup(info);
fb_alloc_error:
	framebuffer_release(info);
	return ret;
}

static int sh1106fb_remove(struct i2c_client *client)
{
	struct fb_info *info = i2c_get_clientdata(client);
	struct sh1106fb_par *par = info->par;

	sh1106fb_write_cmd(par->client, SH1106FB_DISPLAY_OFF);

	backlight_device_unregister(info->bl_dev);

	unregister_framebuffer(info);
	if (par->device_info->need_pwm) {
		pwm_disable(par->pwm);
		pwm_put(par->pwm);
	}
	fb_deferred_io_cleanup(info);
	__free_pages(__va(info->fix.smem_start), get_order(info->fix.smem_len));
	framebuffer_release(info);

	return 0;
}

static const struct i2c_device_id sh1106fb_i2c_id[] = {
	{ "sh1106fb", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sh1106fb_i2c_id);

static struct i2c_driver sh1106fb_driver = {
	.probe = sh1106fb_probe,
	.remove = sh1106fb_remove,
	.id_table = sh1106fb_i2c_id,
	.driver = {
		.name = "sh1106fb",
		.of_match_table = sh1106fb_of_match,
	},
};

module_i2c_driver(sh1106fb_driver);

MODULE_DESCRIPTION("FB driver for the Sinowealth SH1106 OLED controller");
MODULE_AUTHOR("Wei Liu");
MODULE_LICENSE("GPL");
