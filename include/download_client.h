/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

/**@file download_client.h
 *
 * @defgroup dl_client Client to download an object.
 * @{
 * @brief Client to download an object.
 */
/* The download client provides APIs to:
 *  - connect to a remote server
 *  - download an object from the server
 *  - disconnect from the server
 *  - receive asynchronous event notification on the status of
 *    download
 *
 * Currently, only the HTTP protocol is supported for download.
 */

#ifndef DOWNLOAD_CLIENT_H__
#define DOWNLOAD_CLIENT_H__

#include <zephyr.h>
#include <zephyr/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Download events. */
enum download_client_evt_id {
	/** Indicates an error during download.
	 *  The application should disconnect and retry the operation
	 *  when receiving this event.
	 */
	DOWNLOAD_CLIENT_EVT_ERROR = 0x00,
	DOWNLOAD_CLIENT_EVT_FRAGMENT = 0x01,
	DOWNLOAD_CLIENT_EVT_CONN_CLOSED = 0x2,
	DOWNLOAD_CLIENT_EVT_DONE = 0x03,
};

struct download_client_evt {
	enum download_client_evt_id id;
	union  {
		int error;
		struct {
			const void *buf;
			size_t len;
		} fragment;
	};
};

/** @brief Download client asynchronous event handler.
 *
 * The application is notified of the status of the object download
 * through this event handler.
 * The application can use the return value to indicate if it handled
 * the event successfully or not.
 * This feedback is useful when, for example, a faulty fragment was
 * received or the application was unable to process a object fragment.
 *
 * @param[in] client	The client instance.
 * @param[in] event     The event.
 * @param[in] status    Event status (either 0 or an errno value).
 *
 * @retval 0 If the event was handled successfully.
 *           Other values indicate that the application failed to handle
 *           the event.
 */
typedef int (*download_client_event_handler_t)(
	const struct download_client_evt *event);

/** @brief Object download client instance that describes the state of download.
 */
struct download_client {
	/** Buffer used to receive responses from the server.
	 *  This buffer can be read by the application if necessary,
	 *  but must never be written to.
	 */
	char buf[CONFIG_NRF_DOWNLOAD_MAX_RESPONSE_SIZE];
	/** Transport file descriptor.
	 *  If negative, the transport is disconnected.
	 */
	int fd;

	K_THREAD_STACK_MEMBER(thread_stack, 2048);
	struct k_thread thread;
	k_tid_t tid;
	struct k_sem sem;

	size_t file_size;
	size_t progress;
	size_t offset;

	int status;
	bool has_header;

	/** Server that hosts the object. */
	char *host;
	/** Resource to be downloaded. */
	char *file;
	/** Event handler. Must not be NULL. */
	download_client_event_handler_t callback;

};

/** @brief Initialize the download client object for a given host and resource.
 *
 * The server to connect to for the object download is identified by
 * the @p host field of @p client. The @p callback field of @p client
 * must contain an event handler.
 *
 * @note If this method fails, do no call any other APIs for the instance.
 *
 * @param[in,out] client The client instance. Must not be NULL.
 *		      The target, host, resource, and callback fields must be
 *		      correctly initialized in the object instance.
 *		      The fd, status, fragment, fragment_size, download_size,
 *                    and object_size fields are out parameters.
 *
 * @retval 0  If the operation was successful.
 * @retval -1 Otherwise.
 */
int download_client_init(struct download_client *client,
			 download_client_event_handler_t callback);

/**@brief Establish a connection to the server.
 *
 * The server to connect to for the object download is identified by
 * the @p host field of @p client. The @p host field is expected to be a
 * that can be resolved to an IP address using DNS.
 *
 * @note This is a blocking call.
 *       Do not initiate a @ref download_client_start if this procedure fails.
 *
 * @param[in] client The client instance.
 *
 * @retval 0  If the operation was successful.
 * @retval -1 Otherwise.
 */
int download_client_connect(struct download_client *client, char *host);

/**@brief Start downloading the object.
 *
 * This is a blocking call used to trigger the download of an object
 * identified by the @p resource field of @p client. The download is
 * requested from the server in chunks of CONFIG_NRF_DOWNLOAD_MAX_FRAGMENT_SIZE.
 *
 * This API may be used to resume an interrupted download by setting the @p
 * download_size field of @p client to the last successfully downloaded fragment
 * of the object.
 *
 * If the API succeeds, use @ref download_client_process to advance the download
 * until a @ref DOWNLOAD_CLIENT_EVT_ERROR or a
 * @ref DOWNLOAD_CLIENT_EVT_DOWNLOAD_DONE event is received in the registered
 * callback.
 * If the API fails, disconnect using @ref download_client_disconnect, then
 * reconnect using @ref download_client_connect and restart the procedure.
 *
 * @param[in] client The client instance.
 *
 * @retval 0  If the operation was successful.
 * @retval -1 Otherwise.
 */
int download_client_start(struct download_client *client,
			  char *file, size_t offset);

void download_client_pause(struct download_client *);
void download_client_resume(struct download_client *);

/**@brief Disconnect from the server.
 *
 * This API terminates the connection to the server. If called before
 * the download is complete, it is possible to resume the interrupted transfer
 * by reconnecting to the server using the @ref download_client_connect API and
 * calling @ref download_client_start to continue the download.
 * If you want to resume after a power cycle, you must store the download size
 * persistently and supply this value in the next connection.
 *
 * @note You should disconnect from the server as soon as the download
 * is complete.
 *
 * @param[in] client The client instance.
 *
 */
int download_client_disconnect(struct download_client *client);

#ifdef __cplusplus
}
#endif

#endif /* DOWNLOAD_CLIENT_H__ */

/**@} */
