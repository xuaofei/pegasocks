#ifndef _PGS_CODEC_H
#define _PGS_CODEC_H

#include <arpa/inet.h>
#include <event2/buffer.h>
#include <stdint.h>

#include "defs.h"
#include "session.h"

#ifndef htonll
#define htonll(x)                                                              \
	((1 == htonl(1)) ?                                                     \
		       (x) :                                                         \
		       ((uint64_t)htonl((x)&0xFFFFFFFF) << 32) | htonl((x) >> 32))
#endif

#ifndef ntohll
#define ntohll(x) htonll(x)
#endif

#define pgs_ws_write_head_text(b, l) pgs_ws_write_head(b, l, 0x01)
#define pgs_ws_write_head_bin(b, l) pgs_ws_write_head(b, l, 0x02)
#define pgs_ws_write_text(b, msg, l) pgs_ws_write(b, msg, l, 0x01)
#define pgs_ws_write_bin(b, msg, l) pgs_ws_write(b, msg, l, 0x02)

typedef void *(*pgs_session_write_fn)(pgs_session_t *, uint8_t *, uint64_t);

/* for ws response header */
typedef struct pgs_ws_resp_s {
	int fin;
	int opcode;
	int mask;
	uint64_t payload_len;
	size_t header_len;
} pgs_ws_resp_t;

/* websocket */
void pgs_ws_req(struct evbuffer *out, const char *hostname,
		const char *server_address, int server_port, const char *path);
bool pgs_ws_upgrade_check(const char *data);
void pgs_ws_write_head(struct evbuffer *buf, uint64_t len, int opcode);
void pgs_ws_write(struct evbuffer *buf, uint8_t *msg, uint64_t len, int opcode);
bool pgs_ws_parse_head(uint8_t *data, uint64_t data_len, pgs_ws_resp_t *meta);

/* vmess */

typedef struct pgs_vmess_resp_s {
	uint8_t v;
	uint8_t opt;
	uint8_t cmd;
	uint8_t m;
} pgs_vmess_resp_t;

// to remote
uint64_t pgs_vmess_write_head(pgs_session_t *session,
			      pgs_outbound_ctx_v2ray_t *ctx);
uint64_t pgs_vmess_write_body(pgs_session_t *session, const uint8_t *data,
			      uint64_t data_len, uint64_t head_len,
			      pgs_session_write_fn flush);
uint64_t pgs_vmess_write_remote(pgs_session_t *session, const uint8_t *data,
				uint64_t data_len, pgs_session_write_fn flush);

// to local
bool pgs_vmess_parse(pgs_session_t *session, const uint8_t *data,
		     uint64_t data_len, pgs_session_write_fn flush);
bool pgs_vmess_parse_cfb(pgs_session_t *session, const uint8_t *data,
			 uint64_t data_len, pgs_session_write_fn flush);
bool pgs_vmess_parse_gcm(pgs_session_t *session, const uint8_t *data,
			 uint64_t data_len, pgs_session_write_fn flush);

static inline int pgs_get_addr_len(const uint8_t *data)
{
	switch (data[0] /*atype*/) {
	case 0x01:
		// IPv4
		return 4;
	case 0x03:
		return 1 + data[1];
	case 0x04:
		// IPv6
		return 16;
	default:
		break;
	}
	return 0;
}

// helper flush functions
static void trojan_write_remote(pgs_session_t *session, uint8_t *msg,
				uint64_t len)
{
	struct bufferevent *outbev = session->outbound->bev;
	struct evbuffer *outboundw = bufferevent_get_output(outbev);
	pgs_outbound_ctx_trojan_t *trojan_s_ctx = session->outbound->ctx;
	uint64_t head_len = trojan_s_ctx->head_len;
	if (head_len > 0) {
		evbuffer_add(outboundw, trojan_s_ctx->head, head_len);
		trojan_s_ctx->head_len = 0;
	}
	evbuffer_add(outboundw, msg, len);

	pgs_session_debug(session, "local -> remote: %d", len + head_len);
}

static void trojan_write_local(pgs_session_t *session, uint8_t *msg,
			       uint64_t len)
{
	uint8_t *udp_packet = NULL;
	if (session->inbound->state == INBOUND_PROXY) {
		struct bufferevent *inbev = session->inbound->bev;
		struct evbuffer *inboundw = bufferevent_get_output(inbev);
		evbuffer_add(inboundw, msg, len);
		pgs_session_debug(session, "remote -> local: %d", len);
	} else if (session->inbound->state == INBOUND_UDP_RELAY &&
		   session->inbound->udp_fd != -1) {
		// decode trojan udp packet
		uint8_t atype = msg[0];
		uint16_t addr_len = 1 + 2; // atype + port
		addr_len += pgs_get_addr_len(msg);
		uint16_t payload_len = msg[addr_len] << 8 | msg[addr_len + 1];
		if (len < (addr_len + 2 + 2 + payload_len) ||
		    msg[addr_len + 2] != '\r' || msg[addr_len + 3] != '\n') {
			pgs_session_error(
				session,
				"payload too large or invalid response");
			goto error;
		}
		// pack socks5 udp reply
		uint16_t udp_packet_len = 2 + 1 + addr_len + payload_len;
		udp_packet = malloc(udp_packet_len);
		if (udp_packet == NULL) {
			pgs_session_error(session, "out of memory");
			goto error;
		}
		udp_packet[0] = 0x00;
		udp_packet[1] = 0x00;
		udp_packet[2] = 0x00;
		memcpy(udp_packet + 3, msg, addr_len);
		memcpy(udp_packet + 3 + addr_len, msg + addr_len + 4,
		       payload_len);
		int n = sendto(
			session->inbound->udp_fd, udp_packet, udp_packet_len, 0,
			(struct sockaddr *)&session->inbound->udp_client_addr,
			session->inbound->udp_client_addr_size);
		pgs_session_debug(session, "write %d bytes to local udp sock",
				  n);
		free(udp_packet);
	}
	return;

error:
	if (udp_packet != NULL) {
		free(udp_packet);
		udp_packet = NULL;
	}
	pgs_session_free(session);
}

static void vmess_flush_remote(pgs_session_t *session, uint8_t *data,
			       uint64_t len)
{
	struct bufferevent *outbev = session->outbound->bev;
	struct evbuffer *outboundw = bufferevent_get_output(outbev);
	const pgs_server_config_t *config = session->outbound->config;
	const pgs_config_extra_v2ray_t *vconfig = config->extra;
	if (vconfig->websocket.enabled) {
		pgs_ws_write_bin(outboundw, data, len);
	} else {
		evbuffer_add(outboundw, data, len);
	}
}

static void vmess_flush_local(pgs_session_t *session, uint8_t *data,
			      uint64_t len)
{
	struct bufferevent *inbev = session->inbound->bev;
	struct evbuffer *inboundw = bufferevent_get_output(inbev);
	uint8_t *udp_packet = NULL;
	if (session->inbound->state == INBOUND_PROXY) {
		// TCP
		evbuffer_add(inboundw, data, len);
	} else if (session->inbound->state == INBOUND_UDP_RELAY &&
		   session->inbound->udp_fd != -1) {
		// pack to socks5 packet
		pgs_outbound_ctx_v2ray_t *ctx = session->outbound->ctx;
		uint64_t udp_packet_len = 2 + 1 + ctx->target_addr_len + len;
		udp_packet = malloc(udp_packet_len);
		if (udp_packet == NULL) {
			pgs_session_error(session, "out of memory");
			return;
		}
		udp_packet[0] = 0x00;
		udp_packet[1] = 0x00;
		udp_packet[2] = 0x00;
		memcpy(udp_packet + 3, ctx->target_addr, ctx->target_addr_len);
		memcpy(udp_packet + 3 + ctx->target_addr_len, data, len);
		int n = sendto(
			session->inbound->udp_fd, udp_packet, udp_packet_len, 0,
			(struct sockaddr *)&session->inbound->udp_client_addr,
			session->inbound->udp_client_addr_size);
		pgs_session_debug(session, "write %d bytes to local udp sock",
				  n);
		free(udp_packet);
	}
}

#endif