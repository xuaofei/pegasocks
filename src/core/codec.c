#include "core/codec.h"
#include "core/crypto.h"

#include <assert.h>

const char *ws_upgrade = "HTTP/1.1 101";
const char *ws_key = "dGhlIHNhbXBsZSBub25jZQ==";
const char *ws_accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
const char vmess_key_suffix[36] = "c48619fe-8f02-49e0-b9e9-edf763e17e21";

static void pgs_vmess_init_encryptor(pgs_outbound_ctx_v2ray_t *ctx)
{
	switch (ctx->cipher) {
	case AES_128_CFB:
		ctx->encryptor = pgs_cryptor_new(ctx->cipher, PGS_ENCRYPT,
						 (const uint8_t *)ctx->key,
						 (const uint8_t *)ctx->iv);
		break;
	case AEAD_AES_128_GCM:
		assert(ctx->iv_len == AEAD_AES_128_GCM_IV_LEN);
		assert(ctx->key_len == AEAD_AES_128_GCM_KEY_LEN);
		memcpy(ctx->data_enc_key, ctx->key, AEAD_AES_128_GCM_KEY_LEN);
		memcpy(ctx->data_enc_iv + 2, ctx->iv + 2, 10);
		ctx->encryptor =
			pgs_cryptor_new(ctx->cipher, PGS_ENCRYPT,
					(const uint8_t *)ctx->data_enc_key,
					(const uint8_t *)ctx->data_enc_iv);
		break;
	case AEAD_CHACHA20_POLY1305:
		assert(ctx->iv_len == AEAD_CHACHA20_POLY1305_IV_LEN);
		assert(ctx->key_len == AEAD_CHACHA20_POLY1305_KEY_LEN);
		md5((const uint8_t *)ctx->key, AES_128_CFB_IV_LEN,
		    ctx->data_enc_key);
		md5(ctx->data_enc_key, MD5_LEN, ctx->data_enc_key + MD5_LEN);
		memcpy(ctx->data_enc_iv + 2, ctx->iv + 2, 10);
		ctx->encryptor =
			pgs_cryptor_new(ctx->cipher, PGS_ENCRYPT,
					(const uint8_t *)ctx->data_enc_key,
					(const uint8_t *)ctx->data_enc_iv);
		break;
	case AEAD_AES_256_GCM:
		// not supported
	default:
		break;
	}
}

static void pgs_vmess_init_decryptor(pgs_outbound_ctx_v2ray_t *ctx)
{
	// riv rkey to decode header
	md5((const uint8_t *)ctx->iv, AES_128_CFB_IV_LEN, (uint8_t *)ctx->riv);
	md5((const uint8_t *)ctx->key, AES_128_CFB_KEY_LEN,
	    (uint8_t *)ctx->rkey);
	switch (ctx->cipher) {
	case AES_128_CFB:
		ctx->decryptor = pgs_cryptor_new(ctx->cipher, PGS_DECRYPT,
						 (const uint8_t *)ctx->rkey,
						 (const uint8_t *)ctx->riv);
		break;
	case AEAD_AES_128_GCM:
		assert(ctx->iv_len == AEAD_AES_128_GCM_IV_LEN);
		assert(ctx->key_len == AEAD_AES_128_GCM_KEY_LEN);
		memcpy(ctx->data_dec_key, ctx->rkey, AEAD_AES_128_GCM_KEY_LEN);
		ctx->dec_counter = 0;
		memcpy(ctx->data_dec_iv + 2, ctx->riv + 2, 10);
		ctx->decryptor =
			pgs_cryptor_new(ctx->cipher, PGS_DECRYPT,
					(const uint8_t *)ctx->data_dec_key,
					(const uint8_t *)ctx->data_dec_iv);
		break;
	case AEAD_CHACHA20_POLY1305:
		assert(ctx->iv_len == AEAD_CHACHA20_POLY1305_IV_LEN);
		assert(ctx->key_len == AEAD_CHACHA20_POLY1305_KEY_LEN);
		md5((const uint8_t *)ctx->rkey, AES_128_CFB_IV_LEN,
		    ctx->data_dec_key);
		md5(ctx->data_dec_key, MD5_LEN, ctx->data_dec_key + MD5_LEN);
		ctx->dec_counter = 0;
		memcpy(ctx->data_dec_iv + 2, ctx->riv + 2, 10);
		ctx->decryptor =
			pgs_cryptor_new(ctx->cipher, PGS_DECRYPT,
					(const uint8_t *)ctx->data_dec_key,
					(const uint8_t *)ctx->data_dec_iv);
		break;
	case AEAD_AES_256_GCM:
		// not supported
	default:
		break;
	}
}

static void pgs_vmess_increase_cryptor_iv(pgs_outbound_ctx_v2ray_t *ctx,
					  pgs_cryptor_direction_t dir)
{
	if (ctx->cipher == AES_128_CFB)
		return;

	uint16_t *counter = NULL;
	pgs_cryptor_t *cryptor = NULL;
	uint8_t *iv = NULL;
	switch (dir) {
	case PGS_DECRYPT:
		counter = &ctx->dec_counter;
		cryptor = ctx->decryptor;
		iv = ctx->data_dec_iv;
		break;
	case PGS_ENCRYPT:
		counter = &ctx->enc_counter;
		cryptor = ctx->encryptor;
		iv = ctx->data_enc_iv;
		break;
	default:
		break;
	}

	if (counter != NULL && cryptor != NULL) {
		*counter += 1;
		iv[0] = *counter >> 8;
		iv[1] = *counter;

		pgs_cryptor_reset_iv(cryptor, iv);
	}
}

void pgs_ws_req(struct evbuffer *out, const char *hostname,
		const char *server_address, int server_port, const char *path)
{
	// out, hostname, server_address, server_port, path
	evbuffer_add_printf(out, "GET %s HTTP/1.1\r\n", path);
	evbuffer_add_printf(out, "Host:%s:%d\r\n", hostname, server_port);
	evbuffer_add_printf(out, "Upgrade:websocket\r\n");
	evbuffer_add_printf(out, "Connection:upgrade\r\n");
	evbuffer_add_printf(out, "Sec-WebSocket-Key:%s\r\n", ws_key);
	evbuffer_add_printf(out, "Sec-WebSocket-Version:13\r\n");
	evbuffer_add_printf(
		out, "Origin:https://%s:%d\r\n", server_address,
		server_port); //missing this key will lead to 403 response.
	evbuffer_add_printf(out, "\r\n");
}

bool pgs_ws_upgrade_check(const char *data)
{
	return strncmp(data, ws_upgrade, strlen(ws_upgrade)) != 0 ||
	       !strstr(data, ws_accept);
}

void pgs_ws_write(struct evbuffer *buf, uint8_t *msg, uint64_t len, int opcode)
{
	pgs_ws_write_head(buf, len, opcode);
	// x ^ 0 = x
	evbuffer_add(buf, msg, len);
}

void pgs_ws_write_head(struct evbuffer *buf, uint64_t len, int opcode)
{
	uint8_t a = 0;
	a |= 1 << 7; //fin
	a |= opcode;

	uint8_t b = 0;
	b |= 1 << 7; //mask

	uint16_t c = 0;
	uint64_t d = 0;

	//payload len
	if (len < 126) {
		b |= len;
	} else if (len < (1 << 16)) {
		b |= 126;
		c = htons(len);
	} else {
		b |= 127;
		d = htonll(len);
	}

	evbuffer_add(buf, &a, 1);
	evbuffer_add(buf, &b, 1);

	if (c)
		evbuffer_add(buf, &c, sizeof(c));
	else if (d)
		evbuffer_add(buf, &d, sizeof(d));

	// tls will protect data
	// mask data makes nonsense
	uint8_t mask_key[4] = { 0, 0, 0, 0 };
	evbuffer_add(buf, &mask_key, 4);
}

bool pgs_ws_parse_head(uint8_t *data, uint64_t data_len, pgs_ws_resp_t *meta)
{
	meta->fin = !!(*data & 0x80);
	meta->opcode = *data & 0x0F;
	meta->mask = !!(*(data + 1) & 0x80);
	meta->payload_len = *(data + 1) & 0x7F;
	meta->header_len = 2 + (meta->mask ? 4 : 0);

	if (meta->payload_len < 126) {
		if (meta->header_len > data_len)
			return false;

	} else if (meta->payload_len == 126) {
		meta->header_len += 2;
		if (meta->header_len > data_len)
			return false;

		meta->payload_len = ntohs(*(uint16_t *)(data + 2));

	} else if (meta->payload_len == 127) {
		meta->header_len += 8;
		if (meta->header_len > data_len)
			return false;

		meta->payload_len = ntohll(*(uint64_t *)(data + 2));
	}

	if (meta->header_len + meta->payload_len > data_len)
		return false;

	const unsigned char *mask_key = data + meta->header_len - 4;

	for (int i = 0; meta->mask && i < meta->payload_len; i++)
		data[meta->header_len + i] ^= mask_key[i % 4];

	return true;
}

uint64_t pgs_vmess_write_head(pgs_session_t *session,
			      pgs_outbound_ctx_v2ray_t *ctx)
{
	const uint8_t *uuid = session->outbound->config->password;
	int is_udp = (session->inbound != NULL &&
		      session->inbound->state == INBOUND_UDP_RELAY);
	const uint8_t *udp_rbuf = NULL;
	if (is_udp) {
		udp_rbuf = session->inbound->udp_rbuf;
	}

	uint8_t *buf = ctx->remote_wbuf;
	const uint8_t *socks5_cmd = ctx->cmd;
	uint64_t socks5_cmd_len = ctx->cmdlen;

	// auth part
	time_t now = time(NULL);
	unsigned long ts = htonll(now);
	uint8_t header_auth[MD5_LEN];
	uint64_t header_auth_len = 0;
	hmac_md5(uuid, 16, (const uint8_t *)&ts, 8, header_auth,
		 &header_auth_len);
	assert(header_auth_len == MD5_LEN);
	memcpy(buf, header_auth, header_auth_len);

	// command part
	int n = socks5_cmd_len - 4 - 2;
	if (is_udp) {
		n = pgs_get_addr_len(udp_rbuf + 3);
	}
	int p = 0;
	uint64_t header_cmd_len =
		1 + 16 + 16 + 1 + 1 + 1 + 1 + 1 + 2 + 1 + n + p + 4;
	uint8_t header_cmd_raw[header_cmd_len];
	uint8_t header_cmd_encoded[header_cmd_len];
	memzero(header_cmd_raw, header_cmd_len);
	memzero(header_cmd_encoded, header_cmd_len);

	int offset = 0;
	// ver
	header_cmd_raw[0] = 1;
	offset += 1;
	// data iv
	rand_bytes(header_cmd_raw + offset, AES_128_CFB_IV_LEN);
	memcpy(ctx->iv, header_cmd_raw + offset, AES_128_CFB_IV_LEN);
	offset += AES_128_CFB_IV_LEN;
	// data key
	rand_bytes(header_cmd_raw + offset, AES_128_CFB_KEY_LEN);
	memcpy(ctx->key, header_cmd_raw + offset, AES_128_CFB_KEY_LEN);
	offset += AES_128_CFB_KEY_LEN;

	// init data encryptor
	if (!ctx->encryptor)
		pgs_vmess_init_encryptor(ctx);
	assert(ctx->encryptor != NULL);

	if (!ctx->decryptor)
		pgs_vmess_init_decryptor(ctx);
	assert(ctx->decryptor != NULL);

	// v
	rand_bytes(header_cmd_raw + offset, 1);
	ctx->v = header_cmd_raw[offset];
	offset += 1;
	// standard format data
	header_cmd_raw[offset] = 0x01;
	offset += 1;
	// secure
	header_cmd_raw[offset] = ctx->cipher;
	offset += 1;
	// X
	header_cmd_raw[offset] = 0x00;
	offset += 1;

	if (is_udp) {
		header_cmd_raw[offset] = 0x02;
	} else {
		header_cmd_raw[offset] = 0x01;
	}
	offset += 1;

	if (is_udp) {
		// port
		header_cmd_raw[offset] = udp_rbuf[4 + n];
		header_cmd_raw[offset + 1] = udp_rbuf[4 + n + 1];
		offset += 2;
		// atype
		if (udp_rbuf[3] == 0x01) {
			header_cmd_raw[offset] = 0x01;
		} else {
			header_cmd_raw[offset] = udp_rbuf[3] - 1;
		}
		offset += 1;
		// addr
		memcpy(header_cmd_raw + offset, udp_rbuf + 4, n);
		offset += n;
	} else {
		// port
		header_cmd_raw[offset] = socks5_cmd[socks5_cmd_len - 2];
		header_cmd_raw[offset + 1] = socks5_cmd[socks5_cmd_len - 1];
		offset += 2;
		// atype
		if (socks5_cmd[3] == 0x01) {
			header_cmd_raw[offset] = 0x01;
		} else {
			header_cmd_raw[offset] = socks5_cmd[3] - 1;
		}
		offset += 1;
		// addr
		memcpy(header_cmd_raw + offset, socks5_cmd + 4, n);
		offset += n;
	}

	assert(offset + 4 == header_cmd_len);

	unsigned int f = fnv1a(header_cmd_raw, header_cmd_len - 4);

	header_cmd_raw[offset] = f >> 24;
	header_cmd_raw[offset + 1] = f >> 16;
	header_cmd_raw[offset + 2] = f >> 8;
	header_cmd_raw[offset + 3] = f;

	uint8_t k_md5_input[16 + 36];
	memcpy(k_md5_input, uuid, 16);
	memcpy(k_md5_input + 16, vmess_key_suffix, 36);
	uint8_t cmd_k[AES_128_CFB_KEY_LEN];
	md5(k_md5_input, 16 + 36, cmd_k);

	uint8_t iv_md5_input[32];
	now = time(NULL);
	ts = htonll(now);
	memcpy(iv_md5_input, (const unsigned char *)&ts, 8);
	memcpy(iv_md5_input + 8, (const unsigned char *)&ts, 8);
	memcpy(iv_md5_input + 16, (const unsigned char *)&ts, 8);
	memcpy(iv_md5_input + 24, (const unsigned char *)&ts, 8);
	uint8_t cmd_iv[AES_128_CFB_IV_LEN];
	md5(iv_md5_input, 32, cmd_iv);

	aes_128_cfb_encrypt(header_cmd_raw, header_cmd_len, cmd_k, cmd_iv,
			    header_cmd_encoded);
	memcpy(buf + header_auth_len, header_cmd_encoded, header_cmd_len);

	return header_auth_len + header_cmd_len;
}

uint64_t pgs_vmess_write_body(pgs_session_t *session, const uint8_t *data,
			      uint64_t data_len, uint64_t head_len,
			      pgs_session_write_fn flush)
{
	pgs_outbound_ctx_v2ray_t *ctx = session->outbound->ctx;
	uint8_t *localr = ctx->local_rbuf;
	uint8_t *buf = ctx->remote_wbuf + head_len;
	uint64_t sent = 0;
	uint64_t offset = 0;
	uint64_t remains = data_len;
	uint64_t frame_data_len = data_len;

	while (remains > 0) {
		buf = ctx->remote_wbuf + head_len;
		switch (ctx->cipher) {
		case AES_128_CFB: {
			if (remains + 6 > BUFSIZE_16K - head_len) {
				frame_data_len = BUFSIZE_16K - head_len - 6;
			} else {
				frame_data_len = remains;
			}
			// L
			localr[0] = (frame_data_len + 4) >> 8;
			localr[1] = (frame_data_len + 4);

			unsigned int f =
				fnv1a((void *)data + offset, frame_data_len);
			localr[2] = f >> 24;
			localr[3] = f >> 16;
			localr[4] = f >> 8;
			localr[5] = f;

			memcpy(localr + 6, data + offset, frame_data_len);

			size_t ciphertext_len = 0;
			pgs_cryptor_encrypt(ctx->encryptor, localr,
					    frame_data_len + 6, NULL, buf,
					    &ciphertext_len);
			sent += (frame_data_len + 6);
			flush(session, ctx->remote_wbuf,
			      head_len + frame_data_len + 6);
			break;
		}
		case AEAD_AES_128_GCM: {
			if (remains + 18 > BUFSIZE_16K - head_len) {
				// more than one frame
				frame_data_len = BUFSIZE_16K - head_len - 18;
			} else {
				frame_data_len = remains;
			}
			// L
			buf[0] = (frame_data_len + 16) >> 8;
			buf[1] = (frame_data_len + 16);

			size_t ciphertext_len = 0;
			bool ok = pgs_cryptor_encrypt(ctx->encryptor,
						      data + offset,
						      frame_data_len,
						      buf + 2 + frame_data_len,
						      buf + 2, &ciphertext_len);
			pgs_vmess_increase_cryptor_iv(ctx, PGS_ENCRYPT);

			assert(ciphertext_len == frame_data_len);
			sent += (frame_data_len + 18);
			flush(session, ctx->remote_wbuf,
			      head_len + frame_data_len + 18);
			break;
		}
		default:
			// not support yet
			break;
		}

		offset += frame_data_len;
		remains -= frame_data_len;
		if (head_len > 0)
			head_len = 0;
	}

	return sent;
}

uint64_t pgs_vmess_write_remote(pgs_session_t *session, const uint8_t *data,
				uint64_t data_len, pgs_session_write_fn flush)
{
	pgs_outbound_ctx_v2ray_t *ctx = session->outbound->ctx;
	uint64_t head_len = 0;
	if (!ctx->header_sent) {
		// will setup crytors and remote_wbuf
		head_len = pgs_vmess_write_head(session, ctx);
		ctx->header_sent = true;
	}

	uint64_t body_len =
		pgs_vmess_write_body(session, data, data_len, head_len, flush);
	return body_len + head_len;
}

bool pgs_vmess_parse(pgs_session_t *session, const uint8_t *data,
		     uint64_t data_len, pgs_session_write_fn flush)
{
	pgs_outbound_ctx_v2ray_t *ctx = session->outbound->ctx;
	switch (ctx->cipher) {
	case AES_128_CFB:
		return pgs_vmess_parse_cfb(session, data, data_len, flush);
	case AEAD_AES_128_GCM:
		return pgs_vmess_parse_gcm(session, data, data_len, flush);
	default:
		// not implement yet
		break;
	}
	return false;
}

/* symmetric cipher will eat all the data put in */
bool pgs_vmess_parse_cfb(pgs_session_t *session, const uint8_t *data,
			 uint64_t data_len, pgs_session_write_fn flush)
{
	pgs_outbound_ctx_v2ray_t *ctx = session->outbound->ctx;
	pgs_vmess_resp_t meta = { 0 };
	uint8_t *rrbuf = ctx->remote_rbuf;
	uint8_t *lwbuf = ctx->local_wbuf;
	pgs_cryptor_t *decryptor = ctx->decryptor;

	size_t decrypt_len = 0;
	if (!ctx->header_recved) {
		if (data_len < 4)
			return false;
		if (!pgs_cryptor_decrypt(decryptor, data, 4, NULL, rrbuf,
					 &decrypt_len))
			return false;
		meta.v = rrbuf[0];
		meta.opt = rrbuf[1];
		meta.cmd = rrbuf[2];
		meta.m = rrbuf[3];
		if (meta.v != ctx->v)
			return false;
		if (meta.m != 0) // support no cmd
			return false;
		ctx->header_recved = true;
		ctx->resp_len = 0;
		return pgs_vmess_parse_cfb(session, data + 4, data_len - 4,
					   flush);
	}

	if (ctx->resp_len == 0) {
		if (data_len == 0) // may called by itself, wait for more data
			return true;
		if (data_len < 2) // illegal data
			return false;
		if (!pgs_cryptor_decrypt(decryptor, data, 2, NULL, rrbuf,
					 &decrypt_len))
			return false;

		int l = rrbuf[0] << 8 | rrbuf[1];

		if (l == 0 || l == 4) // end
			return true;
		if (l < 4)
			return false;
		ctx->resp_len = l - 4;
		ctx->resp_hash = 0;
		// skip fnv1a hash
		return pgs_vmess_parse_cfb(session, data + 2, data_len - 2,
					   flush);
	}

	if (ctx->resp_hash == 0) {
		if (data_len < 4) // need more data
			return false;
		if (!pgs_cryptor_decrypt(decryptor, data, 4, NULL, rrbuf,
					 &decrypt_len))
			return false;
		ctx->resp_hash = (uint32_t)rrbuf[0] << 24 | rrbuf[1] << 16 |
				 rrbuf[2] << 8 | rrbuf[3];
		return pgs_vmess_parse_cfb(session, data + 4, data_len - 4,
					   flush);
	}

	if (data_len <= 0) // need more data
		return true;

	uint64_t data_to_decrypt =
		ctx->resp_len < data_len ? ctx->resp_len : data_len;
	if (!pgs_cryptor_decrypt(decryptor, data, data_to_decrypt, NULL, lwbuf,
				 &decrypt_len))
		return false;

	flush(session, lwbuf, data_to_decrypt);
	ctx->resp_len -= data_to_decrypt;

	return pgs_vmess_parse_cfb(session, data + data_to_decrypt,
				   data_len - data_to_decrypt, flush);
}

/* AEAD cipher */
bool pgs_vmess_parse_gcm(pgs_session_t *session, const uint8_t *data,
			 uint64_t data_len, pgs_session_write_fn flush)
{
	pgs_outbound_ctx_v2ray_t *ctx = session->outbound->ctx;
	pgs_vmess_resp_t meta = { 0 };
	uint8_t *rrbuf = ctx->remote_rbuf;
	uint8_t *lwbuf = ctx->local_wbuf;
	pgs_cryptor_t *decryptor = ctx->decryptor;

	if (!ctx->header_recved) {
		if (data_len < 4)
			return false;
		if (!aes_128_cfb_decrypt(data, 4, (const uint8_t *)ctx->rkey,
					 (const uint8_t *)ctx->riv, rrbuf))
			return false;
		meta.v = rrbuf[0];
		meta.opt = rrbuf[1];
		meta.cmd = rrbuf[2];
		meta.m = rrbuf[3];
		if (meta.v != ctx->v)
			return false;
		if (meta.m != 0) // support no cmd
			return false;
		ctx->header_recved = true;
		ctx->resp_len = 0;
		return pgs_vmess_parse_gcm(session, data + 4, data_len - 4,
					   flush);
	}

	if (ctx->resp_len == 0) {
		if (data_len == 0) // may called by itself, wait for more data
			return true;
		if (data_len < 2) // illegal data
			return false;
		int l = data[0] << 8 | data[1];

		if (l == 0 || l == 16) // end
			return true;
		if (l < 16)
			return false;
		ctx->resp_len = l - 16;
		ctx->resp_hash = -1;
		// skip fnv1a hash
		return pgs_vmess_parse_gcm(session, data + 2, data_len - 2,
					   flush);
	}

	if (ctx->remote_rbuf_pos + data_len < ctx->resp_len + 16) {
		// need more data, have to cache this
		memcpy(rrbuf + ctx->remote_rbuf_pos, data, data_len);
		ctx->remote_rbuf_pos += data_len;
		return true;
	}

	size_t decrypt_len = 0;
	if (ctx->remote_rbuf_pos == 0) {
		// enough data for decoding and no cache
		uint64_t data_to_decrypt = ctx->resp_len;
		bool ok = pgs_cryptor_decrypt(decryptor, data, ctx->resp_len,
					      data + ctx->resp_len, lwbuf,
					      &decrypt_len);
		pgs_vmess_increase_cryptor_iv(ctx, PGS_DECRYPT);
		if (!ok)
			return false;

		flush(session, lwbuf, data_to_decrypt);
		ctx->resp_len -= data_to_decrypt;

		return pgs_vmess_parse_gcm(session, data + data_to_decrypt + 16,
					   data_len - data_to_decrypt - 16,
					   flush);
	} else {
		// have some cache in last chunk
		// read more and do the rest
		uint64_t data_to_read =
			ctx->resp_len + 16 - ctx->remote_rbuf_pos;
		memcpy(rrbuf + ctx->remote_rbuf_pos, data, data_to_read);

		bool ok = pgs_cryptor_decrypt(decryptor, rrbuf, ctx->resp_len,
					      rrbuf + ctx->resp_len, lwbuf,
					      &decrypt_len);
		pgs_vmess_increase_cryptor_iv(ctx, PGS_DECRYPT);
		if (!ok)
			return false;
		flush(session, lwbuf, ctx->resp_len);
		ctx->resp_len = 0;
		ctx->remote_rbuf_pos = 0;

		return pgs_vmess_parse_gcm(session, data + data_to_read,
					   data_len - data_to_read, flush);
	}
}