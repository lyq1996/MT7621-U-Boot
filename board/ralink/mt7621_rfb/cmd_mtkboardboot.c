// SPDX-License-Identifier:	GPL-2.0+
/*
 * Copyright (C) 2018 MediaTek Incorporation. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#include <common.h>
#include <asm/io.h>
#include <asm/addrspace.h>
#include <asm/types.h>
#include <asm/spl.h>
#include <linux/mtd/mtd.h>
#include <linux/sizes.h>
#include <dm.h>
#include <dm/device-internal.h>
#include <spi.h>
#include <spi_flash.h>
#include <jffs2/jffs2.h>

#include "../common/dual_image.h"

static struct spi_flash *get_sf_dev(void)
{
	unsigned int bus = CONFIG_SF_DEFAULT_BUS;
	unsigned int cs = CONFIG_SF_DEFAULT_CS;
	unsigned int speed = CONFIG_SF_DEFAULT_SPEED;
	unsigned int mode = CONFIG_SF_DEFAULT_MODE;
	struct udevice *new, *bus_dev;
	int ret;

	/* In DM mode defaults will be taken from DT */
	speed = 0, mode = 0;

	/* Remove the old device, otherwise probe will just be a nop */
	ret = spi_find_bus_and_cs(bus, cs, &bus_dev, &new);
	if (!ret)
		device_remove(new, DM_REMOVE_NORMAL);

	ret = spi_flash_probe_bus_cs(bus, cs, speed, mode, &new);
	if (ret) {
		printf("Failed to initialize SPI flash at %u:%u (error %d)\n",
			bus, cs, ret);
		return NULL;
	}

	return dev_get_uclass_priv(new);
}

static int mtkboardboot(void)
{
	char cmd[128];
	struct mtd_device *dev;
	struct part_info *part;
	image_header_t hdr;
	struct spi_flash *sf;
	uint32_t fw_off = CONFIG_DEFAULT_NOR_KERNEL_OFFSET;
	uint32_t load_addr, kernel_load_addr, size;
	u8 pnum;
	int ret;

#ifdef CONFIG_MTK_DUAL_IMAGE_SUPPORT
	dual_image_check();
#endif

	ret = mtdparts_init();
	if (!ret) {
		ret = find_dev_and_part("firmware", &dev, &pnum, &part);
		if (!ret) {
			if (dev->id->type == MTD_DEV_TYPE_NOR) {
				if (part->offset)
					fw_off = (uint32_t) part->offset;
			}
		}
	}

	if (fw_off < SZ_4M - sizeof (hdr)) {
		memcpy(&hdr, (void *) CONFIG_SPI_ADDR + fw_off, sizeof(hdr));

		switch (genimg_get_format((void *) &hdr)) {
		case IMAGE_FORMAT_LEGACY:
			size = image_get_image_size(&hdr);
			break;
#if defined(CONFIG_FIT)
		case IMAGE_FORMAT_FIT:
			size = fit_get_size((const void *) &hdr);
			break;
#endif
		default:
			printf("Error: no Image found at 0x%08x\n",
				CONFIG_SPI_ADDR + fw_off);
			return CMD_RET_FAILURE;
		}

		if (size + fw_off <= SZ_4M) {
			sprintf(cmd, "bootm 0x%08x",
				CONFIG_SPI_ADDR + fw_off);

			return run_command(cmd, 0);
		}
	}

	sf = get_sf_dev();
	if (!sf)
		return CMD_RET_FAILURE;

	ret = spi_flash_read(sf, fw_off, sizeof(hdr), &hdr);
	if (ret)
		return CMD_RET_FAILURE;

	switch (genimg_get_format((void *) &hdr)) {
	case IMAGE_FORMAT_LEGACY:
		size = image_get_image_size(&hdr);
		kernel_load_addr = image_get_load(&hdr);

		if (CKSEG0ADDR(kernel_load_addr) -
			CKSEG0ADDR(CONFIG_SYS_LOAD_ADDR) < size)
			load_addr = CKSEG0ADDR(kernel_load_addr) + SZ_16M;
		else
			load_addr = CONFIG_SYS_LOAD_ADDR;

		break;
#if defined(CONFIG_FIT)
	case IMAGE_FORMAT_FIT:
		size = fit_get_size((const void *) &hdr);
		load_addr = CONFIG_SYS_LOAD_ADDR;
		break;
#endif
	default:
		printf("Error: no Image found at flash offset 0x%08x\n",
			fw_off);
		return CMD_RET_FAILURE;
	}

	printf("Reading from flash 0x%x to mem 0x%08x, size 0x%x ... \n",
		fw_off, load_addr, size);
	ret = spi_flash_read(sf, fw_off, size, (void *) load_addr);
	if (ret)
		return CMD_RET_FAILURE;

	sprintf(cmd, "bootm 0x%08x", load_addr);

	return run_command(cmd, 0);
}

static void gpio_mode_config(void)
{
	unsigned int gpiomode = REG32(RALINK_REG_GPIOMODE) | RALINK_GPIOMODE_DFT;;
	REGWRITE32(RALINK_REG_GPIOMODE, gpiomode);
}

static int check_reset_button(int index)
{
	struct udevice *dev;
	char buf[80];
	int ret;
	int offset;

	for (ret = uclass_first_device(UCLASS_GPIO, &dev); dev; ret = uclass_next_device(&dev)) {
		const char *bank_name;
		int num_bits;

		bank_name = gpio_get_bank_info(dev, &num_bits);
		if (!num_bits) {	
			debug("GPIO device %s has no bits\n", bank_name);
			continue;
		}
		for (offset = 0; offset < num_bits; offset++) {
			ret = gpio_get_status_by_offset(dev, offset, buf, sizeof(buf));
			if (ret < 0)
				goto err;
		
			if(index == offset)
				return ret;
		}
	}
err:
	printf("Error %d\n", ret);
	return ret;
}

static int do_mtkboardboot(cmd_tbl_t *cmdtp, int flag, int argc,
	char *const argv[])
{
#ifdef CONFIG_WEBUI_FAILSAFE_ON_BUTTON
	gpio_mode_config();
	unsigned int gpio = CONFIG_WEBUI_FAILSAFE_BUTTON_GPIO;
	unsigned int reset_value = 0;
	// long press to enter failsafe mode
	for(int i=0; i<3; ++i) {
		// wait 1s
		mdelay(1000);
		reset_value = check_reset_button(gpio);
		if (reset_value == 1)
			break;
		
		if (i == 2 && reset_value == 0) {
			printf("Enter failsafe mode\n");
			run_command("httpd", 0);
			// return and continue do mtkboardboot()
		}
	}
#endif

	mtkboardboot();

#ifndef CONFIG_WEBUI_FAILSAFE_ON_AUTOBOOT_FAIL
	return CMD_RET_FAILURE;
#else
	return run_command("httpd", 0);
#endif
}

U_BOOT_CMD(mtkboardboot, 1, 0, do_mtkboardboot,
	"Boot MTK firmware", ""
);