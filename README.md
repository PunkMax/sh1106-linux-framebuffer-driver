# Liunx下sh1106 OLED屏的显示驱动程序

这个OLED屏是使用sh1106芯片的显示屏，大小为1.3寸，128x64像素，i2c接口，单色屏幕（色彩深度为1）。

目前某宝上主流的1.3寸屏都是这个型号的IC。

但用这个屏的都是在单片机或者Arduino上，在linux上没有相关的驱动代码。

但有一款类似的IC芯片SSD1306，在linux中有驱动代码，位于/drivers/video/fbdev/ssd1307fb.c，于是就在这个驱动的基础上制作了适用于sh1106屏幕的驱动。

目前只把I2C型号的驱动完成了，但SPI型号的驱动没有移植成功。

将驱动代码放到 /drivers/video/fbdev/ 目录下，并在这个目录下的Kconfig和Makefile中加入sh1106的选项
Kconfig：
```
config FB_SH1106
	tristate "Sinowealth SH1106 framebuffer support"
	depends on FB && I2C
	depends on OF
	depends on GPIOLIB || COMPILE_TEST
	select FB_SYS_FOPS
	select FB_SYS_FILLRECT
	select FB_SYS_COPYAREA
	select FB_SYS_IMAGEBLIT
	select FB_DEFERRED_IO
	select PWM
	select FB_BACKLIGHT
	help
	  This driver implements support for the Sinowealth SH1106
	  OLED controller over I2C.
```

Makefile：
```
obj-$(CONFIG_FB_SH1106)		  += sh1106fb.o
```

在设备树文件中添加sh1106的设备节点
```
&i2c3 {
	clock-frequency = <100000>;
	pinctrl-names = "default";
	pinctrl-0 = <&pinctrl_i2c3>;
	status = "okay";

	sh1106: oled@3c {
		compatible = "sinowealth,sh1106fb-i2c";
		reg = <0x3c>;
		sinowealth,width = <128>;
		sinowealth,height = <64>;
		sinowealth,com-offset = <0>;
		sinowealth,com-invdir;
	};
};
```

这样就成功在系统中加载驱动了，驱动成功加载时会打印信息

sh1106fb 2-003c: fb0: SH1106 framebuffer device registered, using 1024 bytes of video memory

以及创建设备节点

/dev/fb0

驱动加载完成后会将屏幕初始化，屏幕会显示花屏，这是正常的。没有显示才是有问题的。

现在就可以通过/dev/fb0设备节点来使用这块屏幕了。

基于Liunx 5.4.70版本制作。

# sh1106_oled_linux_framebuffer_driver

This OLED screen is a display screen controlled by sh1106, with a size of 1.3 inches, 128x64 pixels, i2c interface, and a monochrome screen (bits per pixel is 1).
    
At present, the mainstream 1.3-inch screens for sale are all this ICs type.
    
But those who use this screen are on microcontrollers or Arduino, and there is no driver code on Linux.
    
But there is a similar IC chip SSD1306, with driver code in Linux, located at /drivers/video/fbdev/ssd1307fb.c, so based on this driver, a driver suitable for sh1106 screen was made.
    
At present, only the I2C model driver has been completed, but the SPI model driver has not been successfully transplanted.
