/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <zephyr.h>
#include <flash.h>
#include "bsd.h"
#include "nrf_socket.h"
#include "dfu_client.h"
#include "binfile.h"

#include <logging/log.h>

LOG_MODULE_REGISTER(main);

#define FLASH_OFFSET			0x62000
#define FLASH_PAGE_SIZE			4096

static struct device 	*m_flash_dev;

static int app_dfu_client_event_handler(struct dfu_client_object *const dfu,
			enum dfu_client_evt event, u32_t status);


static struct dfu_client_object dfu = {
	.host = "s3.amazonaws.com",
	.resource = "/nordic-firmware-files/f81197c5-0353-4ac2-a961-3b2ce867329d",
	.callback = app_dfu_client_event_handler
};


/**@brief Recoverable BSD library error. */
void bsd_recoverable_error_handler(uint32_t err)
{
	LOG_ERR("bsd_recoverable_error_handler = %lu\n", err);

	// Read the fault register for details.
	volatile uint32_t fault_reason = *((volatile uint32_t *)0x4002A614);
	__ASSERT(false, "bsd_recoverable_error_handler, reason %lu, reset application\n", fault_reason);
}

/**@brief Irrecoverable BSD library error. */
void bsd_irrecoverable_error_handler(uint32_t err)
{
	LOG_ERR("bsd_irrecoverable_error_handler = %lu\n", err);

	// Read the fault register for details.
	volatile uint32_t fault_reason = *((volatile uint32_t *)0x4002A614);
	__ASSERT(false, "bsd_recoverable_error_handler, reason %lu, reset application\n", fault_reason);
}


/**@brief Initialize application DFU. */
static void app_dfu_init(void)
{
	m_flash_dev = device_get_binding(DT_FLASH_DEV_NAME);

	__ASSERT(m_flash_dev != 0, "Nordic nRF flash driver was not found!\n");

	int retval = dfu_client_init(&dfu);
	__ASSERT(retval == 0, "dfu_init() failed, err %d", retval);
}


/**@brief Start transfer of the new firmware patch. */
static void app_dfu_transfer_start(void)
{
    int retval = dfu_client_connect(&dfu);
    __ASSERT(retval == 0, "dfu_client_connect() failed, err %d", retval);

    retval = dfu_client_download(&dfu);
    __ASSERT(retval != -1, "dfu_client_download() failed, err %d", retval);
}

void check_fragment(u32_t address, char *fragment, int fragment_size)
{
    address -= 0x62000;
    for (int i=0;i<fragment_size; i++)
    {
        if (binfile[address+i] != fragment[i])
        {
            LOG_INF("Mismatch at %08x, got %02x, expected %02x", address, fragment[i], binfile[address+i]);
            return;
        }
    }
    LOG_INF("All %d matched at %08x!", fragment_size, address);
}


static int app_dfu_client_event_handler(struct dfu_client_object *const dfu,
					enum dfu_client_evt event, u32_t error)
{
    int err;

    switch (event) {
    case DFU_CLIENT_EVT_DOWNLOAD_INIT: {
        LOG_INF("Download started");
        break;
    }
    case DFU_CLIENT_EVT_DOWNLOAD_FRAG: {
        static u32_t address = FLASH_OFFSET;
		/* Erase page(s) here: */
        check_fragment(address, dfu->fragment, dfu->fragment_size);
		// err = flash_write(m_flash_dev, address, dfu->fragment, dfu->fragment_size);
		// if (err != 0) {
		// 	__ASSERT(false, "Flash write error %d at address %08x\n", err, address);
		// }
        address += dfu->fragment_size;
        break;
    }

    case DFU_CLIENT_EVT_ERROR: {
        LOG_ERR("DFU error");
        dfu_client_disconnect(dfu);
        dfu_client_abort(dfu);
        __ASSERT(false, "Something went wrong, please restart the application\n");
        break;
    }
    default:
        break;
    }

    return 0;
}



int main(void)
{
    app_dfu_init();

    app_dfu_transfer_start();

    while (dfu.status != DFU_CLIENT_STATUS_DOWNLOAD_COMPLETE) {
        k_sleep(2000);
		dfu_client_process(&dfu);
	}

    dfu_client_disconnect(&dfu);
    LOG_INF("Download complete. Nothing more to do.");
    while(true) {

    }

    return 0;
}
