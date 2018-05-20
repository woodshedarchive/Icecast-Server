/* Icecast
 *
 * This program is distributed under the GNU General Public License, version 2.
 * A copy of this license is included with this source.
 *
 * Copyright 2000-2004, Jack Moffitt <jack@xiph.org, 
 *                      Michael Smith <msmith@xiph.org>,
 *                      oddsock <oddsock@xiph.org>,
 *                      Karl Heyes <karl@xiph.org>
 *                      and others (see AUTHORS for details).
 * Copyright 2014,      Philipp "ph3-der-loewe" Schafft <lion@lion.leolix.org>,
 */

#ifndef __CONNECTION_H__
#define __CONNECTION_H__

#include <sys/types.h>
#include <time.h>

#include "compat.h"
#include "common/httpp/httpp.h"
#include "common/thread/thread.h"
#include "common/net/sock.h"
#include "tls.h"

struct _client_tag;
struct source_tag;
struct ice_config_tag;

typedef enum _tlsmode_tag {
    /* no TLS is used at all */
    ICECAST_TLSMODE_DISABLED = 0,
    /* TLS mode is to be detected */
    ICECAST_TLSMODE_AUTO,
    /* Like ICECAST_TLSMODE_AUTO but enforces TLS */
    ICECAST_TLSMODE_AUTO_NO_PLAIN,
    /* TLS via HTTP Upgrade:-header [RFC2817] */
    ICECAST_TLSMODE_RFC2817,
    /* TLS for transport layer like HTTPS [RFC2818] does */
    ICECAST_TLSMODE_RFC2818
} tlsmode_t;

typedef struct connection_tag connection_t;

#include "listensocket.h"


struct connection_tag {
    unsigned long id;

    time_t con_time;
    time_t discon_time;
    uint64_t sent_bytes;

    sock_t sock;
    listensocket_t *listensocket_real;
    listensocket_t *listensocket_effective;
    int error;

    tlsmode_t tlsmode;
    tls_t *tls;
    int (*send)(struct connection_tag *handle, const void *buf, size_t len);
    int (*read)(struct connection_tag *handle, void *buf, size_t len);

    char *ip;
};

void connection_initialize(void);
void connection_shutdown(void);
void connection_reread_config(struct ice_config_tag *config);
void connection_accept_loop(void);
void connection_setup_sockets(struct ice_config_tag *config);
void connection_close(connection_t *con);
connection_t *connection_create(sock_t sock, listensocket_t *listensocket_real, listensocket_t* listensocket_effective, char *ip);
int connection_complete_source(struct source_tag *source, int response);
void connection_queue(connection_t *con);
void connection_uses_tls(connection_t *con);

ssize_t connection_read_bytes(connection_t *con, void *buf, size_t len);

extern rwlock_t _source_shutdown_rwlock;

#endif  /* __CONNECTION_H__ */
