/*
 * Copyright (c) 2017 Linaro Limited
 * Copyright (c) 2017-2019 Foundries.io
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME net_lwm2m_client_app
#define LOG_LEVEL LOG_LEVEL_DBG

#include <logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#include <drivers/hwinfo.h>
#include <zephyr.h>
#include <dfu/mcuboot.h>
#include <dfu/flash_img.h>
#include <drivers/flash.h>
#include <logging/log_ctrl.h>
#include <power/reboot.h>
#include <drivers/gpio.h>
#include <drivers/sensor.h>
#include <storage/flash_map.h>
#include <net/lwm2m.h>
#include <settings/settings.h>
#include "settings.h"

#define APP_BANNER "Run LWM2M client"

#if !defined(CONFIG_NET_CONFIG_PEER_IPV4_ADDR)
#define CONFIG_NET_CONFIG_PEER_IPV4_ADDR ""
#endif

#if !defined(CONFIG_NET_CONFIG_PEER_IPV6_ADDR)
#define CONFIG_NET_CONFIG_PEER_IPV6_ADDR ""
#endif

#if defined(CONFIG_NET_IPV6)
#define SERVER_ADDR CONFIG_NET_CONFIG_PEER_IPV6_ADDR
#elif defined(CONFIG_NET_IPV4)
#define SERVER_ADDR CONFIG_NET_CONFIG_PEER_IPV4_ADDR
#else
#error LwM2M requires either IPV6 or IPV4 support
#endif


#define WAIT_TIME	K_SECONDS(10)
#define CONNECT_TIME	K_SECONDS(10)

#define CLIENT_MANUFACTURER	"Zephyr"
#define CLIENT_MODEL_NUMBER	"OMA-LWM2M Sample Client"
#define CLIENT_SERIAL_NUMBER	"345000123"
#define CLIENT_FIRMWARE_VER	"1.0"
#define CLIENT_DEVICE_TYPE	"OMA-LWM2M Client"
#define CLIENT_HW_VER		"1.0.1"

#define LIGHT_NAME		"Test light"
#define TIMER_NAME		"Test timer"

#define ENDPOINT_LEN		32

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
#define LED_GPIO_PORT	DT_GPIO_LABEL(DT_ALIAS(led0), gpios)
#define LED_GPIO_PIN	DT_GPIO_PIN(DT_ALIAS(led0), gpios)
#define LED_GPIO_FLAGS	DT_GPIO_FLAGS(DT_ALIAS(led0), gpios)
#else
/* Not an error; the relevant IPSO object will simply not be created. */
#define LED_GPIO_PORT	""
#define LED_GPIO_PIN	0
#define LED_GPIO_FLAGS	0
#endif

static uint8_t bat_idx = LWM2M_DEVICE_PWR_SRC_TYPE_BAT_INT;
static int bat_mv = 3800;
static int bat_ma = 125;
static uint8_t usb_idx = LWM2M_DEVICE_PWR_SRC_TYPE_USB;
static int usb_mv = 5000;
static int usb_ma = 900;
static uint8_t bat_level = 95;
static uint8_t bat_status = LWM2M_DEVICE_BATTERY_STATUS_CHARGING;
static int mem_free = 15;
static int mem_total = 25;

static const struct device *led_dev;
static uint32_t led_state;

static struct lwm2m_ctx client;

static struct k_delayed_work reboot_work;

#define FLASH_BANK0_ID FLASH_AREA_ID(image_0)
#define FLASH_BANK1_ID FLASH_AREA_ID(image_1)
#define FLASH_BANK1_OFFSET FLASH_AREA_OFFSET(image_1)
#define FLASH_BANK1_SIZE FLASH_AREA_SIZE(image_1)
static const struct device *flash_dev;
static struct flash_img_context dfu_ctx;

#if defined(CONFIG_LWM2M_DTLS_SUPPORT)
#define TLS_TAG			1

/* "000102030405060708090a0b0c0d0e0f" */
static unsigned char client_psk[] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};

static const char client_psk_id[] = "Client_identity";
#endif /* CONFIG_LWM2M_DTLS_SUPPORT */

static struct k_sem quit_lock;

#if defined(CONFIG_LWM2M_FIRMWARE_UPDATE_OBJ_SUPPORT)
static uint8_t firmware_buf[CONFIG_LWM2M_COAP_BLOCK_SIZE];
static char firmware_version[32];
#endif

/* TODO: Move to a pre write hook that can handle ret codes once available */
static int led_on_off_cb(uint16_t obj_inst_id, uint16_t res_id, uint16_t res_inst_id,
			 uint8_t *data, uint16_t data_len,
			 bool last_block, size_t total_size)
{
	int ret = 0;
	uint32_t led_val;

	led_val = *(uint8_t *) data;
	if (led_val != led_state) {
		ret = gpio_pin_set(led_dev, LED_GPIO_PIN, (int) led_val);
		if (ret) {
			/*
			 * We need an extra hook in LWM2M to better handle
			 * failures before writing the data value and not in
			 * post_write_cb, as there is not much that can be
			 * done here.
			 */
			LOG_ERR("Fail to write to GPIO %d", LED_GPIO_PIN);
			return ret;
		}

		led_state = led_val;
		/* TODO: Move to be set by an internal post write function */
		lwm2m_engine_set_s32("3311/0/5852", 0);
	}

	return ret;
}

static int init_led_device(void)
{
	int ret;

	led_dev = device_get_binding(LED_GPIO_PORT);
	if (!led_dev) {
		return -ENODEV;
	}

	ret = gpio_pin_configure(led_dev, LED_GPIO_PIN, LED_GPIO_FLAGS |
							GPIO_OUTPUT_INACTIVE);
	if (ret) {
		return ret;
	}

	return 0;
}

static void *firmware_read_cb(uint16_t obj_inst_id, uint16_t res_id,
				uint16_t res_inst_id, size_t *data_len)
{
	*data_len = strlen(firmware_version);

	return firmware_version;
}

static void reboot(struct k_work *work)
{
	LOG_INF("Rebooting device");
#ifdef CONFIG_NET_L2_BT
	bt_network_disable();
#endif
	LOG_PANIC();
	sys_reboot(SYS_REBOOT_WARM);
}

static int device_reboot_cb(uint16_t obj_inst_id)
{
	LOG_INF("DEVICE: Reboot in progress");
	k_delayed_work_submit(&reboot_work, K_SECONDS(1));

	return 0;
}

static int device_factory_default_cb(uint16_t obj_inst_id)
{
	LOG_INF("DEVICE: FACTORY DEFAULT");
	/* Add an error for testing */
	lwm2m_device_add_err(LWM2M_DEVICE_ERROR_GPS_FAILURE);
	/* Change the USB current for testing */
	lwm2m_engine_set_s32("3/0/8/1", (usb_ma - 1));

	return 0;
}

#if defined(CONFIG_LWM2M_FIRMWARE_UPDATE_PULL_SUPPORT)
static int firmware_update_cb(uint16_t obj_inst_id)
{
	struct update_counter update_counter;
	int ret = 0;

	LOG_DBG("Executing firmware update");

	/* Bump update counter so it can be verified on the next reboot */
	ret = fota_update_counter_read(&update_counter);
	if (ret) {
		LOG_ERR("Failed read update counter");
		goto cleanup;
	}
	LOG_INF("Update Counter: current %d, update %d",
		update_counter.current, update_counter.update);
	ret = fota_update_counter_update(COUNTER_UPDATE,
					  update_counter.current + 1);
	if (ret) {
		LOG_ERR("Failed to update the update counter: %d", ret);
		goto cleanup;
	}

	boot_request_upgrade(false);

	k_delayed_work_submit(&reboot_work, K_SECONDS(1));

	return 0;

cleanup:
	return ret;
}
#endif


static void *temperature_get_buf(uint16_t obj_inst_id, uint16_t res_id,
				 uint16_t res_inst_id, size_t *data_len)
{
	/* Last read temperature value, will use 25.5C if no sensor available */
	static struct float32_value v = { 25, 500000 };
	const struct device *dev = NULL;

#if defined(CONFIG_FXOS8700_TEMP)
	dev = device_get_binding(DT_LABEL(DT_INST(0, nxp_fxos8700)));
#endif

	if (dev != NULL) {
		if (sensor_sample_fetch(dev)) {
			LOG_ERR("temperature data update failed");
		}

		sensor_channel_get(dev, SENSOR_CHAN_DIE_TEMP,
				  (struct sensor_value *) &v);
		LOG_DBG("LWM2M temperature set to %d.%d", v.val1, v.val2);
	}

	/* echo the value back through the engine to update min/max values */
	lwm2m_engine_set_float32("3303/0/5700", &v);
	*data_len = sizeof(v);
	return &v;
}


#if defined(CONFIG_LWM2M_FIRMWARE_UPDATE_OBJ_SUPPORT)
static void *firmware_get_buf(uint16_t obj_inst_id, uint16_t res_id,
			      uint16_t res_inst_id, size_t *data_len)
{
	*data_len = sizeof(firmware_buf);
	return firmware_buf;
}

static int firmware_block_received_cb(uint16_t obj_inst_id,
				      uint16_t res_id, uint16_t res_inst_id,
				      uint8_t *data, uint16_t data_len,
				      bool last_block, size_t total_size)
{
#if defined(CONFIG_FOTA_ERASE_PROGRESSIVELY)
	static int last_offset = FLASH_BANK1_OFFSET;
#endif
	static uint8_t percent_downloaded;
	static uint32_t bytes_downloaded;
	uint8_t downloaded;
	int ret = 0;

	LOG_DBG("FIRMWARE: BLOCK RECEIVED: len:%u last_block:%d",
			data_len, last_block);

	if (total_size > FLASH_BANK1_SIZE) {
		LOG_ERR("Artifact file size too big (%d)", total_size);
		return -EINVAL;
	}

	if (!data_len) {
		LOG_ERR("Data len is zero, nothing to write.");
		return -EINVAL;
	}

	/* Erase bank 1 before starting the write process */
	if (bytes_downloaded == 0) {
		flash_img_init(&dfu_ctx);
#if defined(CONFIG_FOTA_ERASE_PROGRESSIVELY)
		LOG_INF("Download firmware started, erasing progressively.");
		/* reset image data */
		ret = boot_invalidate_slot1();
		if (ret != 0) {
			LOG_ERR("Failed to reset image data in bank 1");
			goto cleanup;
		}
#else
		LOG_INF("Download firmware started, erasing second bank");
		ret = boot_erase_img_bank(FLASH_BANK1_ID);
		if (ret != 0) {
			LOG_ERR("Failed to erase flash bank 1");
			goto cleanup;
		}
#endif
	}

	bytes_downloaded += data_len;

	/* display a % downloaded, if it's different */
	if (total_size) {
		downloaded = bytes_downloaded * 100 / total_size;
	} else {
		/* Total size is empty when there is only one block */
		downloaded = 100;
	}

	if (downloaded > percent_downloaded) {
		percent_downloaded = downloaded;
		LOG_INF("%d%%", percent_downloaded);
	}

#if defined(CONFIG_FOTA_ERASE_PROGRESSIVELY)
	/* Erase the sector that's going to be written to next */
	while (last_offset <
			FLASH_BANK1_OFFSET + dfu_ctx.bytes_written +
			DT_FLASH_ERASE_BLOCK_SIZE) {
		LOG_INF("Erasing sector at offset 0x%x", last_offset);
		flash_write_protection_set(flash_dev, false);
		ret = flash_erase(flash_dev, last_offset,
				DT_FLASH_ERASE_BLOCK_SIZE);
		flash_write_protection_set(flash_dev, true);
		last_offset += DT_FLASH_ERASE_BLOCK_SIZE;
		if (ret) {
			LOG_ERR("Error %d while erasing sector", ret);
			goto cleanup;
		}
	}
#endif

	ret = flash_img_buffered_write(&dfu_ctx, data, data_len, last_block);
	if (ret < 0) {
		LOG_ERR("Failed to write flash block");
		goto cleanup;
	}

	if (!last_block) {
		/* Keep going */
		return ret;
	}

	if (total_size && (bytes_downloaded != total_size)) {
		LOG_ERR("Early last block, downloaded %d, expecting %d",
				bytes_downloaded, total_size);
		ret = -EIO;
	}

cleanup:
#if defined(CONFIG_FOTA_ERASE_PROGRESSIVELY)
	last_offset = DT_FLASH_AREA_IMAGE_1_OFFSET;
#endif
	bytes_downloaded = 0;
	percent_downloaded = 0;

	return ret;
}
#endif

static int timer_digital_state_cb(uint16_t obj_inst_id,
				  uint16_t res_id, uint16_t res_inst_id,
				  uint8_t *data, uint16_t data_len,
				  bool last_block, size_t total_size)
{
	bool *digital_state = (bool *)data;

	if (*digital_state) {
		LOG_INF("TIMER: ON");
	} else {
		LOG_INF("TIMER: OFF");
	}

	return 0;
}

static int lwm2m_setup(void)
{
	int ret;
	char *server_url;
	uint16_t server_url_len;
	uint8_t server_url_flags;

	/* setup SECURITY object */

	/* Server URL */
	ret = lwm2m_engine_get_res_data("0/0/0",
					(void **)&server_url, &server_url_len,
					&server_url_flags);
	if (ret < 0) {
		return ret;
	}

	snprintk(server_url, server_url_len, "coap%s//%s%s%s",
		 IS_ENABLED(CONFIG_LWM2M_DTLS_SUPPORT) ? "s:" : ":",
		 strchr(SERVER_ADDR, ':') ? "[" : "", SERVER_ADDR,
		 strchr(SERVER_ADDR, ':') ? "]" : "");

	/* Security Mode */
	lwm2m_engine_set_u8("0/0/2",
			    IS_ENABLED(CONFIG_LWM2M_DTLS_SUPPORT) ? 0 : 3);
#if defined(CONFIG_LWM2M_DTLS_SUPPORT)
	lwm2m_engine_set_string("0/0/3", (char *)client_psk_id);
	lwm2m_engine_set_opaque("0/0/5",
				(void *)client_psk, sizeof(client_psk));
#endif /* CONFIG_LWM2M_DTLS_SUPPORT */

#if defined(CONFIG_LWM2M_RD_CLIENT_SUPPORT_BOOTSTRAP)
	/* Mark 1st instance of security object as a bootstrap server */
	lwm2m_engine_set_u8("0/0/1", 1);

	/* Create 2nd instance of security object needed for bootstrap */
	lwm2m_engine_create_obj_inst("0/1");
#else
	/* Match Security object instance with a Server object instance with
	 * Short Server ID.
	 */
	lwm2m_engine_set_u16("0/0/10", 101);
	lwm2m_engine_set_u16("1/0/0", 101);
#endif

	/* setup SERVER object */

	/* setup DEVICE object */

	lwm2m_engine_set_res_data("3/0/0", CLIENT_MANUFACTURER,
				  sizeof(CLIENT_MANUFACTURER),
				  LWM2M_RES_DATA_FLAG_RO);
	lwm2m_engine_set_res_data("3/0/1", CLIENT_MODEL_NUMBER,
				  sizeof(CLIENT_MODEL_NUMBER),
				  LWM2M_RES_DATA_FLAG_RO);
	lwm2m_engine_set_res_data("3/0/2", CLIENT_SERIAL_NUMBER,
				  sizeof(CLIENT_SERIAL_NUMBER),
				  LWM2M_RES_DATA_FLAG_RO);
	lwm2m_engine_set_res_data("3/0/3", CLIENT_FIRMWARE_VER,
				  sizeof(CLIENT_FIRMWARE_VER),
				  LWM2M_RES_DATA_FLAG_RO);
	lwm2m_engine_register_read_callback("3/0/3", firmware_read_cb);
	lwm2m_engine_register_exec_callback("3/0/4", device_reboot_cb);
	lwm2m_engine_register_exec_callback("3/0/5", device_factory_default_cb);
	lwm2m_engine_set_res_data("3/0/9", &bat_level, sizeof(bat_level), 0);
	lwm2m_engine_set_res_data("3/0/10", &mem_free, sizeof(mem_free), 0);
	lwm2m_engine_set_res_data("3/0/17", CLIENT_DEVICE_TYPE,
				  sizeof(CLIENT_DEVICE_TYPE),
				  LWM2M_RES_DATA_FLAG_RO);
	lwm2m_engine_set_res_data("3/0/18", CLIENT_HW_VER,
				  sizeof(CLIENT_HW_VER),
				  LWM2M_RES_DATA_FLAG_RO);
	lwm2m_engine_set_res_data("3/0/20", &bat_status, sizeof(bat_status), 0);
	lwm2m_engine_set_res_data("3/0/21", &mem_total, sizeof(mem_total), 0);

	/* add power source resource instances */
	lwm2m_engine_create_res_inst("3/0/6/0");
	lwm2m_engine_set_res_data("3/0/6/0", &bat_idx, sizeof(bat_idx), 0);

	lwm2m_engine_create_res_inst("3/0/7/0");
	lwm2m_engine_set_res_data("3/0/7/0", &bat_mv, sizeof(bat_mv), 0);
	lwm2m_engine_create_res_inst("3/0/8/0");
	lwm2m_engine_set_res_data("3/0/8/0", &bat_ma, sizeof(bat_ma), 0);
	lwm2m_engine_create_res_inst("3/0/6/1");
	lwm2m_engine_set_res_data("3/0/6/1", &usb_idx, sizeof(usb_idx), 0);
	lwm2m_engine_create_res_inst("3/0/7/1");
	lwm2m_engine_set_res_data("3/0/7/1", &usb_mv, sizeof(usb_mv), 0);
	lwm2m_engine_create_res_inst("3/0/8/1");
	lwm2m_engine_set_res_data("3/0/8/1", &usb_ma, sizeof(usb_ma), 0);

	/* setup FIRMWARE object */

#if defined(CONFIG_LWM2M_FIRMWARE_UPDATE_OBJ_SUPPORT)
	/* setup data buffer for block-wise transfer */
	lwm2m_engine_register_pre_write_callback("5/0/0", firmware_get_buf);
	lwm2m_firmware_set_write_cb(firmware_block_received_cb);
#endif
#if defined(CONFIG_LWM2M_FIRMWARE_UPDATE_PULL_SUPPORT)
	lwm2m_firmware_set_update_cb(firmware_update_cb);
#endif

	/* setup TEMP SENSOR object */
	lwm2m_engine_create_obj_inst("3303/0");
	lwm2m_engine_register_read_callback("3303/0/5700", temperature_get_buf);

	/* IPSO: Light Control object */
	if (init_led_device() == 0) {
		lwm2m_engine_create_obj_inst("3311/0");
		lwm2m_engine_register_post_write_callback("3311/0/5850",
				led_on_off_cb);
		lwm2m_engine_set_res_data("3311/0/5750",
					  LIGHT_NAME, sizeof(LIGHT_NAME),
					  LWM2M_RES_DATA_FLAG_RO);
	}

	/* IPSO: Timer object */
	lwm2m_engine_create_obj_inst("3340/0");
	lwm2m_engine_register_post_write_callback("3340/0/5543",
			timer_digital_state_cb);
	lwm2m_engine_set_res_data("3340/0/5750", TIMER_NAME, sizeof(TIMER_NAME),
				  LWM2M_RES_DATA_FLAG_RO);

	/* Reboot work, used when executing update */
	k_delayed_work_init(&reboot_work, reboot);

	return 0;
}

static void rd_client_event(struct lwm2m_ctx *client,
			    enum lwm2m_rd_client_event client_event)
{
	switch (client_event) {

	case LWM2M_RD_CLIENT_EVENT_NONE:
		/* do nothing */
		break;

	case LWM2M_RD_CLIENT_EVENT_BOOTSTRAP_REG_FAILURE:
		LOG_DBG("Bootstrap registration failure!");
		break;

	case LWM2M_RD_CLIENT_EVENT_BOOTSTRAP_REG_COMPLETE:
		LOG_DBG("Bootstrap registration complete");
		break;

	case LWM2M_RD_CLIENT_EVENT_BOOTSTRAP_TRANSFER_COMPLETE:
		LOG_DBG("Bootstrap transfer complete");
		break;

	case LWM2M_RD_CLIENT_EVENT_REGISTRATION_FAILURE:
		LOG_DBG("Registration failure!");
		break;

	case LWM2M_RD_CLIENT_EVENT_REGISTRATION_COMPLETE:
		LOG_DBG("Registration complete");
		break;

	case LWM2M_RD_CLIENT_EVENT_REG_UPDATE_FAILURE:
		LOG_DBG("Registration update failure!");
		break;

	case LWM2M_RD_CLIENT_EVENT_REG_UPDATE_COMPLETE:
		LOG_DBG("Registration update complete");
		break;

	case LWM2M_RD_CLIENT_EVENT_DEREGISTER_FAILURE:
		LOG_DBG("Deregister failure!");
		break;

	case LWM2M_RD_CLIENT_EVENT_DISCONNECT:
		LOG_DBG("Disconnected");
		break;

	case LWM2M_RD_CLIENT_EVENT_QUEUE_MODE_RX_OFF:
		LOG_DBG("Queue mode RX window closed");
		break;

	case LWM2M_RD_CLIENT_EVENT_NETWORK_ERROR:
		LOG_ERR("LwM2M engine reported a network erorr.");
		lwm2m_rd_client_stop(client, rd_client_event);
		break;
	}
}

static void log_img_ver(void)
{
	struct mcuboot_img_header header;
	struct mcuboot_img_sem_ver *ver;
	int ret;

	ret = boot_read_bank_header(FLASH_BANK0_ID, &header, sizeof(header));
	if (ret) {
		LOG_ERR("can't read header: %d", ret);
		return;
	} else if (header.mcuboot_version != 1) {
		LOG_ERR("unsupported MCUboot version %u",
			header.mcuboot_version);
		return;
	}

	ver = &header.h.v1.sem_ver;
	snprintf(firmware_version, sizeof(firmware_version),
		 "%u.%u.%u build #%u", ver->major, ver->minor,
		 ver->revision, ver->build_num);
	LOG_INF("image version %s", log_strdup(firmware_version));
}

static int lwm2m_image_init(void)
{
	int ret = 0;
	struct update_counter counter;
	bool image_ok;

	/*
	 * Initialize the DFU context.
	 */
	flash_dev = device_get_binding(DT_CHOSEN_ZEPHYR_FLASH_CONTROLLER_LABEL);
	if (!flash_dev) {
		LOG_ERR("missing flash device %s",
				DT_CHOSEN_ZEPHYR_FLASH_CONTROLLER_LABEL);
		return -ENODEV;
	}

	log_img_ver();

	/* Update boot status and update counter */
	ret = fota_update_counter_read(&counter);
	if (ret) {
		LOG_ERR("Failed read update counter");
		return ret;
	}
	LOG_INF("Update Counter: current %d, update %d",
		counter.current, counter.update);
	image_ok = boot_is_img_confirmed();
	LOG_INF("Image is%s confirmed OK", image_ok ? "" : " not");
	if (!image_ok) {
		ret = boot_write_img_confirmed();
		if (ret) {
			LOG_ERR("Couldn't confirm this image: %d", ret);
			return ret;
		}
		LOG_INF("Marked image as OK");
#if defined(CONFIG_FOTA_ERASE_PROGRESSIVELY)
		/* instead of erasing slot 1, reset image data */
		ret = boot_invalidate_slot1();
		if (ret) {
			LOG_ERR("Flash image 1 reset: error %d", ret);
			return ret;
		}

		LOG_DBG("Erased flash bank 1 at offset %x",
			FLASH_BANK1_OFFSET);
#else
		ret = boot_erase_img_bank(FLASH_BANK1_ID);
		if (ret) {
			LOG_ERR("Flash bank erase at offset %x: error %d",
				FLASH_BANK1_OFFSET, ret);
			return ret;
		}

		LOG_DBG("Erased flash bank 1 at offset %x",
			FLASH_BANK1_OFFSET);
#endif

		if (counter.update != -1) {
			ret = fota_update_counter_update(COUNTER_CURRENT,
						counter.update);
			if (ret) {
				LOG_ERR("Failed to update the update "
					"counter: %d", ret);
				return ret;
			}
			ret = fota_update_counter_read(&counter);
			if (ret) {
				LOG_ERR("Failed to read update counter: %d",
					ret);
				return ret;
			}
			LOG_INF("Update Counter updated");
		}
	}

	/* Check if a firmware update status needs to be reported */
	if (counter.update != -1 &&
			counter.current == counter.update) {
		/* Successful update */
		LOG_INF("Firmware updated successfully");
		lwm2m_engine_set_u8("5/0/5", RESULT_SUCCESS);
	} else if (counter.update > counter.current) {
		/* Failed update */
		LOG_INF("Firmware failed to be updated");
		lwm2m_engine_set_u8("5/0/5", RESULT_UPDATE_FAILED);
	}

	return ret;
}
void main(void)
{
	uint32_t flags = IS_ENABLED(CONFIG_LWM2M_RD_CLIENT_SUPPORT_BOOTSTRAP) ?
				LWM2M_RD_CLIENT_FLAG_BOOTSTRAP : 0;
	int ret;

	LOG_INF(APP_BANNER);

	k_sem_init(&quit_lock, 0, UINT_MAX);

	LOG_INF("Initializing FOTA settings\n");
	ret = fota_settings_init();
	if (ret) {
		LOG_ERR("Failed to init fota settings (%d)", ret);
		return;
	}
	LOG_INF("Initializing FOTA settings done");

	settings_load();

	LOG_INF("Initializing LWM2M Image");
	ret = lwm2m_image_init();
	if (ret < 0) {
		LOG_ERR("Failed to setup image properties (%d)", ret);
		return;
	}
	LOG_INF("lwm2m_image_init done");

	ret = lwm2m_setup();
	if (ret < 0) {
		LOG_ERR("Cannot setup LWM2M fields (%d)", ret);
		return;
	}
	LOG_DBG("lwm2m_setup done");

	(void)memset(&client, 0x0, sizeof(client));
#if defined(CONFIG_LWM2M_DTLS_SUPPORT)
	client.tls_tag = TLS_TAG;
#endif

#if defined(CONFIG_HWINFO)
	uint8_t dev_id[16];
	char dev_str[33];
	ssize_t length;
	int i;

	(void)memset(dev_id, 0x0, sizeof(dev_id));

	/* Obtain the device id */
	length = hwinfo_get_device_id(dev_id, sizeof(dev_id));

	/* If this fails for some reason, use all zeros instead */
	if (length <= 0) {
		length = sizeof(dev_id);
	}

	/* Render the obtained serial number in hexadecimal representation */
	for (i = 0 ; i < length ; i++) {
		sprintf(&dev_str[i*2], "%02x", dev_id[i]);
	}

	lwm2m_rd_client_start(&client, dev_str, flags, rd_client_event);
#else
	/* client.sec_obj_inst is 0 as a starting point */
	lwm2m_rd_client_start(&client, CONFIG_BOARD, flags, rd_client_event);
#endif

	k_sem_take(&quit_lock, K_FOREVER);
}
