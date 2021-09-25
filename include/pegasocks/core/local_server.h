#ifndef _PGS_LOCAL_SERVER_H
#define _PGS_LOCAL_SERVER_H

#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/dns.h>

#include <stdint.h>

#include "optional/acl.h"
#include "log.h"
#include "config.h"
#include "server_manager.h"
#include "ssl.h"

typedef struct pgs_local_server_s {
	uint32_t tid;
	int server_fd;
	struct event_base *base;
	struct evdns_base *dns_base;
	struct evconnlistener *listener;
	pgs_logger_t *logger;
	// shared from main thread, read only
	pgs_config_t *config;
	pgs_server_manager_t *sm;
	pgs_acl_t *acl;
	pgs_ssl_ctx_t *ssl_ctx;
} pgs_local_server_t;

typedef struct pgs_local_server_ctx_s {
	int fd;
	pgs_mpsc_t *mpsc;
	pgs_config_t *config;
	pgs_server_manager_t *sm;
	pgs_acl_t *acl;
	pgs_ssl_ctx_t *ssl_ctx;
} pgs_local_server_ctx_t;

pgs_local_server_t *pgs_local_server_new(pgs_local_server_ctx_t *ctx);
void pgs_local_server_run(pgs_local_server_t *local);
void pgs_local_server_destroy(pgs_local_server_t *local);

void *start_local_server(void *data);

#endif
