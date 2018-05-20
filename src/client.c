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
 * Copyright 2011-2018, Philipp "ph3-der-loewe" Schafft <lion@lion.leolix.org>,
 */

/* client.c
 **
 ** client interface implementation
 **
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include "common/thread/thread.h"
#include "common/avl/avl.h"
#include "common/httpp/httpp.h"

#include "cfgfile.h"
#include "connection.h"
#include "refbuf.h"
#include "format.h"
#include "stats.h"
#include "fserve.h"
#include "errors.h"

#include "client.h"
#include "auth.h"
#include "logging.h"

#include "util.h"

/* for ADMIN_COMMAND_ERROR */
#include "admin.h"

#include "cors.h"

#ifdef _WIN32
#define snprintf _snprintf
#endif

#undef CATMODULE
#define CATMODULE "client"

static inline void client_send_500(client_t *client, const char *message);

/* create a client_t with the provided connection and parser details. Return
 * 0 on success, -1 if server limit has been reached.  In either case a
 * client_t is returned just in case a message needs to be returned. Should
 * be called with global lock held.
 */
int client_create(client_t **c_ptr, connection_t *con, http_parser_t *parser)
{
    ice_config_t    *config;
    client_t        *client = (client_t *) calloc(1, sizeof(client_t));
    int              ret    = -1;

    if (client == NULL)
        abort();

    config = config_get_config();

    global.clients++;
    if (config->client_limit < global.clients) {
        ICECAST_LOG_WARN("server client limit reached (%d/%d)", config->client_limit, global.clients);
    } else {
        ret = 0;
    }

    config_release_config ();

    stats_event_args (NULL, "clients", "%d", global.clients);
    client->con = con;
    client->parser = parser;
    client->protocol = ICECAST_PROTOCOL_HTTP;
    client->admin_command = ADMIN_COMMAND_ERROR;
    client->refbuf = refbuf_new (PER_CLIENT_REFBUF_SIZE);
    client->refbuf->len = 0; /* force reader code to ignore buffer contents */
    client->pos = 0;
    client->write_to_client = format_generic_write_to_client;
    *c_ptr = client;

    return ret;
}

static inline void client_reuseconnection(client_t *client) {
    connection_t *con;
    reuse_t reuse;

    if (!client)
        return;

    con = client->con;
    con = connection_create(con->sock, con->serversock, strdup(con->ip));
    reuse = client->reuse;
    client->con->sock = -1; /* TODO: do not use magic */

    /* handle to keep the TLS connection */
    if (client->con->tls) {
        /* AHhhggrr.. That pain....
         * stealing TLS state...
         */
        con->tls  = client->con->tls;
        con->read = client->con->read;
        con->send = client->con->send;
        client->con->tls  = NULL;
        client->con->read = NULL;
        client->con->send = NULL;
    }

    client->reuse = ICECAST_REUSE_CLOSE;

    client_destroy(client);

    if (reuse == ICECAST_REUSE_UPGRADETLS)
        connection_uses_tls(con);
    connection_queue(con);
}

void client_destroy(client_t *client)
{
    ICECAST_LOG_DEBUG("Called to destory client %p", client);
    if (client == NULL)
        return;

    if (client->reuse != ICECAST_REUSE_CLOSE) {
        client_reuseconnection(client);
        return;
    }

    /* release the buffer now, as the buffer could be on the source queue
     * and may of disappeared after auth completes */
    if (client->refbuf) {
        refbuf_release (client->refbuf);
        client->refbuf = NULL;
    }

    if (auth_release_client(client))
        return;

    /* write log entry if ip is set (some things don't set it, like outgoing
     * slave requests
     */
    if (client->respcode && client->parser)
        logging_access(client);
    if (client->con)
        connection_close(client->con);
    if (client->parser)
        httpp_destroy(client->parser);
    if (client->encoding)
        httpp_encoding_release(client->encoding);

    global_lock();
    global.clients--;
    stats_event_args(NULL, "clients", "%d", global.clients);
    global_unlock();

    /* we need to free client specific format data (if any) */
    if (client->free_client_data)
        client->free_client_data(client);

    free(client->username);
    free(client->password);
    free(client->role);
    acl_release(client->acl);

    free(client);
}

/* helper function for reading data from a client */
static ssize_t __client_read_bytes_real(client_t *client, void *buf, size_t len)
{
    /* we have data to read from a refbuf first */
    if (client->refbuf->len < len)
        len = client->refbuf->len;
    memcpy (buf, client->refbuf->data, len);
    if (len < client->refbuf->len) {
        char *ptr = client->refbuf->data;
        memmove (ptr, ptr+len, client->refbuf->len - len);
    }
    client->refbuf->len -= len;
    return len;
}

int client_read_bytes(client_t *client, void *buf, unsigned len)
{
    ssize_t (*reader)(void*, void*, size_t) = (ssize_t(*)(void*,void*,size_t))__client_read_bytes_real;
    void *userdata = client;
    int bytes;

    if (!(client->refbuf && client->refbuf->len)) {
        reader = (ssize_t(*)(void*,void*,size_t))connection_read_bytes;
        userdata = client->con;
    }

    if (client->encoding) {
        bytes = httpp_encoding_read(client->encoding, buf, len, reader, userdata);
    } else {
        bytes = reader(userdata, buf, len);
    }

    if (bytes == -1 && client->con->error)
        ICECAST_LOG_DEBUG("reading from connection has failed");

    return bytes;
}

static inline void _client_send_error(client_t *client, int plain, const icecast_error_t *error)
{
    ssize_t ret;
    refbuf_t *data;

    if (error->http_status == 500) {
         client_send_500(client, error->message);
         return;
    }

    data = refbuf_new(PER_CLIENT_REFBUF_SIZE);
    if (!data) {
         client_send_500(client, error->message);
         return;
    }

    client->reuse = ICECAST_REUSE_KEEPALIVE;

    ret = util_http_build_header(client->refbuf->data, PER_CLIENT_REFBUF_SIZE, 0,
                                 0, error->http_status, NULL,
                                 plain ? "text/plain" : "text/html", "utf-8",
                                 NULL, NULL, client);

    if (ret == -1 || ret >= PER_CLIENT_REFBUF_SIZE) {
        ICECAST_LOG_ERROR("Dropping client as we can not build response headers.");
        client_send_500(client, "Header generation failed.");
        return;
    }

    if (plain) {
        snprintf(data->data, data->len, "Error %i\r\n---------\r\n\r\nMessage: %s\r\n\r\nError code: %s\r\n",
                 error->http_status, error->message, error->uuid
                );
    } else {
        snprintf(data->data, data->len,
                 "<html><head><title>Error %i</title></head><body><h1>Error %i</h1><hr><p><b>%s</b></p><p>Error code: %s</p></body></html>\r\n",
                 error->http_status, error->http_status, error->message, error->uuid);
    }
    data->len = strlen(data->data);

    snprintf(client->refbuf->data + ret, PER_CLIENT_REFBUF_SIZE - ret,
             "Content-Length: %llu\r\n\r\n",
             (long long unsigned int)data->len);

    client->respcode = error->http_status;
    client->refbuf->len = strlen (client->refbuf->data);
    client->refbuf->next = data;

    fserve_add_client (client, NULL);
}

void client_send_error_by_id(client_t *client, int id)
{
    const icecast_error_t *error = error_get_by_id(id);
    const char *pref;
    int plain;

    if (!error) {
         client_send_500(client, "Unknown error ID");
         return;
    }

    pref = util_http_select_best(httpp_getvar(client->parser, "accept"), "text/plain", "text/html", (const char*)NULL);

    if (strcmp(pref, "text/plain") == 0) {
        plain = 1;
    } else if (strcmp(pref, "text/html") == 0) {
        plain = 0;
    } else {
        plain = 1;
    }

    _client_send_error(client, plain, error);
}

void client_send_101(client_t *client, reuse_t reuse)
{
    ssize_t ret;

    if (!client)
        return;

    if (reuse != ICECAST_REUSE_UPGRADETLS) {
        client_send_500(client, "Bad reuse parameter");
        return;
    }

    client->reuse = reuse;

    ret = util_http_build_header(client->refbuf->data, PER_CLIENT_REFBUF_SIZE, 0,
                                 0, 101, NULL,
                                 "text/plain", "utf-8",
                                 NULL, NULL, client);

    snprintf(client->refbuf->data + ret, PER_CLIENT_REFBUF_SIZE - ret,
             "Content-Length: 0\r\nUpgrade: TLS/1.0, HTTP/1.0\r\n\r\n");

    client->respcode = 101;
    client->refbuf->len = strlen(client->refbuf->data);

    fserve_add_client(client, NULL);
}

/* Sends an empty 204 response (for OPTIONS) */
void client_send_204(client_t *client)
{
    ssize_t  ret;
    char    *message;

    message = calloc(PER_CLIENT_REFBUF_SIZE, sizeof(char));

    if (!message) {
        client_send_500(client, "Unable to allocate memory for response");
        return;
    }
    ret = util_http_build_header(message,                 // Response buffer
                                 PER_CLIENT_REFBUF_SIZE,  // Buffer size
                                 0,                       // Offset
                                 0,                       // Prevent cache
                                 204,                     // Status code
                                 "No Content",            // Status message
                                 NULL,                    // Content-Type
                                 NULL,                    // Charset
                                 NULL,                    // Data
                                 NULL,                    // Source
                                 client);

    if (ret == -1 || ret >= PER_CLIENT_REFBUF_SIZE) {
        free(message);
        ICECAST_LOG_ERROR("Dropping client as we can not build response headers.");
        client_send_500(client, "Header generation failed.");
        return;
    }
    
    client_send_bytes(client, message, strlen(message));
    client_destroy(client);
    free(message);
}

void client_send_426(client_t *client, reuse_t reuse)
{
    ssize_t ret;

    if (!client)
        return;

    if (reuse != ICECAST_REUSE_UPGRADETLS) {
        client_send_500(client, "Bad reuse parameter");
        return;
    }

    client->reuse = reuse;

    ret = util_http_build_header(client->refbuf->data, PER_CLIENT_REFBUF_SIZE, 0,
                                 0, 426, NULL,
                                 "text/plain", "utf-8",
                                 NULL, NULL, client);

    snprintf(client->refbuf->data + ret, PER_CLIENT_REFBUF_SIZE - ret,
             "Content-Length: 0\r\nUpgrade: TLS/1.0, HTTP/1.0\r\n\r\n");

    client->respcode = 426;
    client->refbuf->len = strlen(client->refbuf->data);

    client->reuse = ICECAST_REUSE_KEEPALIVE;

    fserve_add_client(client, NULL);
}

/* this function is designed to work even if client is in bad state */
static inline void client_send_500(client_t *client, const char *message)
{
    const char header[] = "HTTP/1.0 500 Internal Server Error\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n"
                          "500 - Internal Server Error\n---------------------------\n";
    const ssize_t header_len = sizeof(header) - 1;
    ssize_t ret;

    ret = client_send_bytes(client, header, header_len);

    /* only send message if we have one AND if header could have transmitted completly */
    if (message && ret == header_len)
        client_send_bytes(client, message, strlen(message));

    client_destroy(client);
}

/* helper function for sending the data to a client */
int client_send_bytes(client_t *client, const void *buf, unsigned len)
{
    int ret = client->con->send(client->con, buf, len);

    if (client->con->error)
        ICECAST_LOG_DEBUG("Client connection died");

    return ret;
}

void client_set_queue(client_t *client, refbuf_t *refbuf)
{
    refbuf_t *to_release = client->refbuf;

    client->refbuf = refbuf;
    if (refbuf)
        refbuf_addref(client->refbuf);
    client->pos = 0;
    if (to_release)
        refbuf_release(to_release);
}
