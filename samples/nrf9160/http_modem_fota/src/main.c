/*$$$LICENCE_NORDIC_STANDARD<2016>$$$*/
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <zephyr.h>

#include "bsd.h"
#include "lte_lc.h"
#include "nrf_socket.h"
#include "dfu_client.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(app);

#define APP_DFU_FRAGMENT_SIZE    1024


static int      m_modem_dfu_fd;          /**< Socket fd used for modem DFU. */
static bool     m_skip_upgrade = false;  /**< State variable indicating if a upgrade is needed for not. */

static int app_dfu_client_event_handler(struct dfu_client_object *const dfu, enum dfu_client_evt event,
                  u32_t status);


static struct dfu_client_object dfu = {
    .host = "s3.amazonaws.com",
    .target = DFU_CLIENT_TARGET_NETWORK_STACK,
    .callback = app_dfu_client_event_handler
};


/**@brief Recoverable BSD library error. */
void bsd_recoverable_error_handler(uint32_t err)
{
    LOG_ERR("bsd_recoverable_error_handler = %lu\n", err);

    // Read the fault register for details.
    volatile uint32_t fault_reason = *((volatile uint32_t *)0x4002A614);
    LOG_INF("Fault reason = %lu\n", fault_reason);
    if (fault_reason == 0x5500001ul) {
        LOG_INF("Firmware update okay, restart device.");
        bsd_shutdown();
        bsd_init();
    } else {
        while(1);
    }
}

/**@brief Irrecoverable BSD library error. */
void bsd_irrecoverable_error_handler(uint32_t err)
{
    LOG_ERR("bsd_irrecoverable_error_handler = %lu\n", err);

    // Read the fault register for details.
    volatile uint32_t fault_reason = *((volatile uint32_t *)0x4002A614);
    LOG_INF("Fault reason = %lu\n", fault_reason);
    if (fault_reason == 0x5500001ul) {
        LOG_INF("Firmware update okay, restart device.");
    } else {
        LOG_INF ("Something went wrong! Restart");
    }

    bsd_shutdown();
    bsd_init();
}


static void app_modem_firmware_revision_validate(nrf_dfu_fw_version_t * p_revision)
{
    // Validate firmware revision or use it to trigger updates.
}


/**@brief Initialize modem DFU. */
static void app_modem_dfu_init(void)
{
    // Request a socket for firmware upgrade.
    m_modem_dfu_fd = nrf_socket(NRF_AF_LOCAL, NRF_SOCK_STREAM, NRF_PROTO_DFU);
    __ASSERT(m_modem_dfu_fd != -1, "Failed to open Modem DFU socket.");

    // Bind the socket to Modem.


    uint32_t optlen;
    // Get the current revision of modem.
    nrf_dfu_fw_version_t revision;
    optlen = sizeof(nrf_dfu_fw_version_t);
    int retval = nrf_getsockopt(m_modem_dfu_fd,
                                NRF_SOL_DFU,
                                NRF_SO_DFU_FW_VERSION,
                                &revision,
                                &optlen);
    __ASSERT(retval != -1, "Firmware revision request failed.");
    app_modem_firmware_revision_validate(&revision);
    char * r = revision;
    printk("Revision = \n");
    printk("    %x%x%x%x%x%x-%x%x%x%x%x%x-%x%x%x%x%x%x\n", r[0],r[1],r[2],r[3],r[4],r[5], r[6],r[7],r[8],r[9],r[10],r[11],r[12],r[13],r[14],r[15],r[16],r[17]);
    printk("    %x%x%x%x%x%x-%x%x%x%x%x%x-%x%x%x%x%x%x\n", r[18],r[19],r[20],r[21],r[22],r[23], r[24],r[25],r[26],r[27],r[28],r[29],r[30],r[31],r[32],r[33],r[34],r[35]);

    // Get available resources to improve chances of a successful upgrade.
    nrf_dfu_fw_resource_t resource;
    optlen = sizeof(nrf_dfu_fw_resource_t);
    retval = nrf_getsockopt(m_modem_dfu_fd,
                            NRF_SOL_DFU,
                            NRF_SO_DFU_RESOURCE,
                            &resource,
                            &optlen);
    __ASSERT(retval != -1, "Resource request failed.");

    // Get offset of existing download in the event that the download was interrupted.
    nrf_dfu_fw_offset_t offset;
    optlen = sizeof(nrf_dfu_fw_offset_t);
    retval = nrf_getsockopt(m_modem_dfu_fd,
                            NRF_SOL_DFU,
                            NRF_SO_DFU_OFFSET,
                            &offset,
                            &optlen);
    __ASSERT(retval != -1, "Offset request failed.");

    if (offset != 0)
    {
        dfu.offset = offset;
    }

    retval = dfu_client_init(&dfu);
   __ASSERT(retval == 0, "dfu_init() failed, err %d", retval);
}


/**@brief Start transfer of the new firmware patch. */
static void app_modem_dfu_transfer_start(void)
{
    if (m_skip_upgrade == true)
    {
        return;
    }

    int retval = dfu_client_connect(&dfu);
    __ASSERT(retval == 0, "dfu_client_connect() failed, err %d", retval);

    retval = dfu_client_download(&dfu);
    __ASSERT(retval == 0, "dfu_client_download() failed, err %d", retval);
}


/**
 * @brief Apply the firmware transferred firmware.
 *
 * @note In all normal operations, including the LTE link are disrupted on
 *       application of new firmware. The communication with the modem should be
 *       reinitialized using the bsd_init. The LTE link establishment procedures
 *       must re-initiated as well. This reinitialization is needed regardless of
 *       success or failure of the procedure. The success or failure of the procedure
 *       indicates if firmware upgrade was successful or not. The success indicates that
 *       new firmware was applied and failure indicates that firmware upgrade was not
 *       successful and the older revision of firmware will be used.
 */
static void app_modem_dfu_transfer_apply(void)
{
    if (m_skip_upgrade == true) {
        uint32_t retval = nrf_setsockopt(m_modem_dfu_fd,
                                     NRF_SOL_DFU,
                                     NRF_SO_DFU_REVERT,
                                     NULL,
                                     0);
        __ASSERT(retval != -1, "Failed to revert to the old firmware.\n");
        printk("Requested reverting to the old firmware.\n");

        dfu.offset = 0;

        retval = nrf_setsockopt(m_modem_dfu_fd,
                             NRF_SOL_DFU,
                             NRF_SO_DFU_OFFSET,
                             &dfu.offset,
                             sizeof(dfu.offset));
        __ASSERT(retval != -1, "Failed to reset offset.\n");
        printk("Reset offset.\n");

        // Delete the backup
        retval = nrf_setsockopt(m_modem_dfu_fd,
                             NRF_SOL_DFU,
                             NRF_SO_DFU_BACKUP_DELETE,
                             NULL,
                             0);
        __ASSERT(retval != -1, "Failed to reset offset.\n");
        printk("Reset offset.\n");

    } else {
        uint32_t retval = nrf_setsockopt(m_modem_dfu_fd,
                                         NRF_SOL_DFU,
                                         NRF_SO_DFU_APPLY,
                                         NULL,
                                         0);
        __ASSERT(retval != -1, "Failed to apply the new firmware.\n");
        printk("Requested apply of new firmware.\n");
    }


    nrf_close(m_modem_dfu_fd);

    // Reinitialize.
    printk("Reinitializing bsd.\n");
    bsd_init();

}


static int app_dfu_client_event_handler(struct dfu_client_object *const dfu,
                  enum dfu_client_evt event,
                  u32_t error)
{
    int err;

    switch (event) {
    case DFU_CLIENT_EVT_DOWNLOAD_INIT: {
        LOG_INF("Download started");
        break;
    }
    case DFU_CLIENT_EVT_DOWNLOAD_FRAG: {
         int sent = 0;

         sent = nrf_send(m_modem_dfu_fd,
                            dfu->fragment,
                            dfu->fragment_size,
                            0);

         if (sent == -1) {
            return -1;
         }

        break;
    }
    case DFU_CLIENT_EVT_DOWNLOAD_DONE: {
        LOG_INF("Download completed");
        if (!error) {
            dfu_client_disconnect(dfu);
            err = dfu_client_apply(dfu);
            if (err) {
                /* Firmware upgrade failed. */
            }
            app_modem_dfu_transfer_apply();
        }
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
    app_modem_dfu_init();

    lte_lc_init_and_connect();

    app_modem_dfu_transfer_start();

    while (true) {
        k_sleep(1000);
        dfu_client_process(&dfu);
    }

    return 0;
}

