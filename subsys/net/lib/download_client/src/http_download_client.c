/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdio.h>
#include <string.h>
#include <download_client.h>
#include <zephyr.h>
#include <net/socket.h>
#include <zephyr/types.h>
#include <misc/util.h>
#include <logging/log.h>

LOG_MODULE_REGISTER(download_client);

#define GET_TEMPLATE                                                           \
	"GET /%s HTTP/1.1\r\n"                                                 \
	"Host: %s\r\n"                                                         \
	"Connection: keep-alive\r\n"                                           \
	"Range: bytes=%u-%u\r\n"                                               \
	"\r\n"

static int resolve_and_connect(const char *const host,
			       const char *const port,
			       u32_t family, u32_t proto)
{
	int fd;

	if (host == NULL) {
		return -1;
	}

	struct addrinfo *addrinf = NULL;
	struct addrinfo hints = {
	    .ai_family = family,
	    .ai_socktype = SOCK_STREAM,
	    .ai_protocol = proto,
	};

	LOG_DBG("Requesting getaddrinfo() for %s", host);

	/* DNS resolve the port. */
	int rc = getaddrinfo(host, port, &hints, &addrinf);

	if (rc < 0 || (addrinf == NULL)) {
		LOG_ERR("getaddrinfo() failed, err %d", errno);
		return -1;
	}

	struct addrinfo *addr = addrinf;
	struct sockaddr *remoteaddr;

	int addrlen = (family == AF_INET6)
			  ? sizeof(struct sockaddr_in6)
			  : sizeof(struct sockaddr_in);

	/* Open a socket based on the local address. */
	fd = socket(family, SOCK_STREAM, proto);
	if (fd >= 0) {
		/* Look for IPv4 address of the broker. */
		while (addr != NULL) {
			remoteaddr = addr->ai_addr;

			LOG_DBG("Resolved address family %d\n",
				addr->ai_family);

			if (remoteaddr->sa_family == family) {
				((struct sockaddr_in *)remoteaddr)->sin_port =
					htons(80);

				LOG_HEXDUMP_DBG((const uint8_t *)remoteaddr,
					addr->ai_addrlen, "Resolved addr");

				/** TODO:
				 *  Need to set security setting for HTTPS.
				 */
				rc = connect(fd, remoteaddr, addrlen);
				if (rc == 0) {
					break;
				}
			}
			addr = addr->ai_next;
		}
	}

	freeaddrinfo(addrinf);

	if (rc < 0) {
		close(fd);
		fd = -1;
	}

	return fd;
}

static int http_request_send(const struct download_client *client, size_t len)
{
	int sent;
	size_t off = 0;

	while (len) {
		sent = send(client->fd, client->buf + off, len, 0);
		if (sent <= 0) {
			return -EIO;
		}

		off += sent;
		len -= sent;
	}

	return 0;
}

static int get_request_send(struct download_client *client)
{
	__ASSERT(client, "NULL object");
	__ASSERT(client->host, "NULL host");
	__ASSERT(client->file, "NULL resource");

	int err;
	int len;
	size_t off;

	/* Offset of last byte in range (Content-Range) */
	off = client->progress + CONFIG_NRF_DOWNLOAD_MAX_FRAGMENT_SIZE - 1;

	if (client->file_size != 0) {
		/* Don't request bytes past the end of file */
		off = MIN(off, client->file_size);
	}

	len = snprintf(client->buf, CONFIG_NRF_DOWNLOAD_MAX_RESPONSE_SIZE,
		       GET_TEMPLATE, client->file, client->host,
		       client->progress, off);

	if (len < 0 || len > CONFIG_NRF_DOWNLOAD_MAX_RESPONSE_SIZE) {
		LOG_ERR("Cannot create GET request, buffer too small");
		return -ENOMEM;
	}

	LOG_HEXDUMP_DBG(client->buf, len, "HTTP request");

	LOG_DBG("Sending HTTP request");

	err = http_request_send(client, len);
	if (err) {
		return err;
	}

	LOG_DBG("HTTP request sent");

	return 0;
}

static int header_parse(struct download_client *client)
{
	char *p;
	size_t hdr;

	p = strstr(client->buf, "\r\n\r\n");
	if (!p) {
		/* Awaiting full GET response */
		LOG_DBG("Awaiting full header in response");
		return 1;
	}

	/* Offset of the header in the buffer */
	hdr = p + strlen("\r\n\r\n") - client->buf;

	__ASSERT(hdr < sizeof(client->buf), "Buffer overflow");

	LOG_DBG("GET header size: %u", hdr);
	LOG_HEXDUMP_DBG(client->buf, hdr, "GET");

	/* If file size is not known, read it from the header */
	if (client->file_size == 0) {
		p = strstr(client->buf, "Content-Range: bytes");
		if (!p) {
			/* Cannot continue */
			LOG_ERR("Server did not send "
				"\"Content-Range\" in response");
			return -1;
		}
		p = strstr(p, "/");
		if (!p) {
			/* Cannot continue */
			LOG_ERR("Server did not send file size in response");
			return -1;
		}

		client->file_size = atoi(p + 1);

		LOG_INF("File size = %d", client->file_size);
	}

	if (client->offset != hdr) {
		/* The current buffer contains some payload bytes.
		 * Copy them at the beginning of the buffer
		 * then update the offset.
		 */
		LOG_WRN("Copying %u payload bytes", client->offset - hdr);
		memcpy(client->buf, client->buf + hdr,
		       client->offset - hdr);

		client->offset -= hdr;
	} else {
		/* Reset the offset.
		 * The payload is received in an empty buffer.
		 */
		client->offset = 0;
	}

	return 0;
}

static int fragment_send(const struct download_client *client)
{
	__ASSERT(client->offset <= CONFIG_NRF_DOWNLOAD_MAX_FRAGMENT_SIZE,
		 "Fragment overflow!");

	__ASSERT(client->offset <= CONFIG_NRF_DOWNLOAD_MAX_RESPONSE_SIZE,
		 "Buffer overflow!");

	const struct download_client_evt evt = {
		.id = DOWNLOAD_CLIENT_EVT_FRAGMENT,
		.fragment = {
			.buf = client->buf,
			.len = client->offset,
		}
	};

	return client->callback(&evt);
}

void download_thread(void *pclient, void *a2, void *a3)
{
	int rc;
	size_t len;
	struct download_client *dl;

	dl = pclient;

	while (true) {
		__ASSERT(dl->offset < sizeof(dl->buf), "Buffer overflow");

		LOG_DBG("Receiving bytes..");
		len = recv(dl->fd, dl->buf + dl->offset,
			   sizeof(dl->buf) - dl->offset, 0);

		if (len == -1 && errno != EAGAIN) {
			LOG_ERR("Error reading from socket: recv() errno %d",
				errno);
			const struct download_client_evt evt = {
				.id = DOWNLOAD_CLIENT_EVT_ERROR,
				.error = -ENOTCONN
			};
			dl->callback(&evt);
			// thread dead?
			break;
		}

		if (len == 0) {
			LOG_WRN("Peer closed connection!");
			const struct download_client_evt evt = {
				.id = DOWNLOAD_CLIENT_EVT_ERROR,
				.error = -ECONNRESET
			};
			dl->callback(&evt);
			k_thread_suspend(dl->tid);
			continue;
		}

		LOG_DBG("Read %d bytes from socket", len);

		/* Accumulate buffer offset */
		dl->offset += len;

		if (!dl->has_header) {
			rc = header_parse(dl);
			if (rc > 0) {
				/* Wait for payload */
				continue;
			}
			if (rc < 0) {
				/* thread dead? */
				break;
			}

			dl->has_header = true;
		}

		/* Accumulate overall file progress.
		 *
		 * If the last recv() call read an HTTP header,
		 * the offset has been moved to the end of the header in
		 * header_parse(). Thus, we accumulate the offset
		 * to the progress.
		 *
		 * If the last recv() call received only a HTTP message body,
		 * then we accumulate 'len'.
		 *
		 */
		dl->progress += MIN(dl->offset, len);

		LOG_INF("Downloaded bytes: %u/%u", dl->progress,
			dl->file_size);

		LOG_HEXDUMP_DBG(dl->buf, dl->offset, "Fragment");

		if (dl->progress == dl->file_size) {
			LOG_INF("Download complete");
			/* Send out last fragment */
			fragment_send(dl);
			const struct download_client_evt evt = {
				.id = DOWNLOAD_CLIENT_EVT_DONE,
			};
			dl->callback(&evt);
			k_thread_suspend(dl->tid);
			continue;
		}

		if (dl->offset < CONFIG_NRF_DOWNLOAD_MAX_FRAGMENT_SIZE) {
			LOG_DBG("Awaiting full response (%u)", dl->offset);
			continue;
		}

		/* Send fragment to application */
		fragment_send(dl);

		/* Request next fragment */
		dl->offset = 0;
		dl->has_header = false;

		get_request_send(dl);
	}

	return;
}

int download_client_init(struct download_client *const client,
			 download_client_event_handler_t callback)
{
	if (client == NULL || callback == NULL) {
		return -EINVAL;
	}

	client->fd = -1;
	client->tid = NULL;
	client->callback = callback;

	return 0;
}

int download_client_connect(struct download_client * const client, char *host)
{
	if (client == NULL || host == NULL) {
		return -EINVAL;
	}

	if (client->fd != -1) {
		/* Already connected */
		return 0;
	}

	client->host = host;

	/* TODO: Parse the host for name, port and protocol. */
	client->fd =
		resolve_and_connect(client->host, NULL, AF_INET, IPPROTO_TCP);
	if (client->fd < 0) {
		LOG_ERR("resolve_and_connect() failed, err %d", errno);
		return -EINVAL;
	}

	LOG_INF("Socket ready");

	return 0;
}

int download_client_disconnect(struct download_client * const client)
{
	int err;

	if (client == NULL || client->fd < 0) {
		return -EINVAL;
	}

	err = close(client->fd);
	if (err) {
		return err;
	}

	client->fd = -1;

	return 0;
}

int download_client_start(struct download_client *client, char *file,
			  size_t offset)
{
	if (client == NULL || client->fd < 0) {
		return -EINVAL;
	}

	client->file = file;
	client->file_size = 0;
	client->progress = offset;

	client->offset = 0;
	client->has_header = false;

	LOG_INF("Downloading: %s [%u]", client->file, client->progress);

	if (client->progress % CONFIG_NRF_DOWNLOAD_MAX_FRAGMENT_SIZE) {
		LOG_DBG("Attempting to continue from non-2^ offset..");
	}

	get_request_send(client);

	if (client->tid == NULL) {
		client->tid = k_thread_create(
			&client->thread, client->thread_stack, 2048,
			download_thread, client, NULL, NULL,
			K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
	} else {
		k_thread_resume(client->tid);
	}

	return 0;
}

void download_client_pause(struct download_client *client)
{
	k_thread_suspend(client->tid);
}

void download_client_resume(struct download_client *client)
{
	k_thread_resume(client->tid);
}
