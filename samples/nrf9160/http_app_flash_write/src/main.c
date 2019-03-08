/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */
#include <stdint.h>
#include <zephyr.h>
#include <flash.h>
#include <bsd.h>
#include <nrf_socket.h>
#include <dfu_client.h>
#include <logging/log.h>

LOG_MODULE_REGISTER(main);

static struct device		*m_flash_dev;

static int app_dfu_client_event_handler(struct dfu_client_object *const dfu,
			enum dfu_client_evt event, u32_t status);


static struct dfu_client_object dfu = {
	.host = CONFIG_HOST,
	.resource = CONFIG_RESOURCE,
	.callback = app_dfu_client_event_handler
};


/**@brief Recoverable BSD library error. */
void bsd_recoverable_error_handler(uint32_t err)
{
	LOG_ERR("%s = %lu\n", __func__, err);

	/* Read the fault register for details. */
	volatile uint32_t fault_reason = *((volatile uint32_t *)0x4002A614);

	__ASSERT(false, "%s, reason %lu, reset application\n",
					__func__, fault_reason);
}

/**@brief Irrecoverable BSD library error. */
void bsd_irrecoverable_error_handler(uint32_t err)
{
	LOG_ERR("%s = %lu\n", __func__, err);

	/* Read the fault register for details. */
	volatile uint32_t fault_reason = *((volatile uint32_t *)0x4002A614);

	__ASSERT(false, "%s, reason %lu, reset application\n",
					 __func__, fault_reason);
}


/**@brief Initialize application. */
static void app_init(void)
{
	m_flash_dev = device_get_binding(DT_FLASH_DEV_NAME);

	__ASSERT(m_flash_dev != 0, "Nordic nRF flash driver was not found!\n");

	int retval = dfu_client_init(&dfu);

	__ASSERT(retval == 0, "dfu_client_init() failed, err %d", retval);
}


/**@brief Start transfer of the file. */
static void app_transfer_start(void)
{
	int retval = dfu_client_connect(&dfu);

	__ASSERT(retval == 0, "dfu_client_connect() failed, err %d", retval);

	retval = dfu_client_download(&dfu);
	__ASSERT(retval != -1, "dfu_client_download() failed, err %d", retval);
}


static int app_dfu_client_event_handler(struct dfu_client_object *const dfu,
					enum dfu_client_evt event, u32_t error)
{
	int err;

	switch (event) {
	case DFU_CLIENT_EVT_DOWNLOAD_FRAG: {
		static u32_t address = CONFIG_FLASH_OFFSET;

		/* Erase page(s) here: */

		err = flash_write(m_flash_dev, address, dfu->fragment,
							dfu->fragment_size);
		if (err != 0) {
			__ASSERT(false,
					"Flash write error %d at address %08x\n",
					err, address);
		}
		address += dfu->fragment_size;
		break;
	}

	case DFU_CLIENT_EVT_DOWNLOAD_DONE:
		LOG_INF("DFU DFU_CLIENT_EVT_DOWNLOAD_DONE");
		break;

	case DFU_CLIENT_EVT_ERROR: {
		LOG_ERR("DFU error");
		dfu_client_disconnect(dfu);
		__ASSERT(false, "Something went wrong, please restart "
						"the application\n");
		break;
	}
	default:
		break;
	}
	return 0;
}



int main(void)
{
	app_init();

	app_transfer_start();

	while (dfu.status != DFU_CLIENT_STATUS_DOWNLOAD_COMPLETE) {
		dfu_client_process(&dfu);
	}

	dfu_client_disconnect(&dfu);
	LOG_INF("Download complete. Nothing more to do.");
	while (true) {
	}

	return 0;
}
