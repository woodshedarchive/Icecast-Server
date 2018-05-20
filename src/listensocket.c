/* Icecast
 *
 * This program is distributed under the GNU General Public License, version 2.
 * A copy of this license is included with this source.
 *
 * Copyright 2018,      Philipp "ph3-der-loewe" Schafft <lion@lion.leolix.org>,
 */

/**
 * Listen socket operations.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_POLL
#include <poll.h>
#else
#include <sys/select.h>
#endif

#include "common/thread/thread.h"
#include "listensocket.h"

#include "logging.h"
#define CATMODULE "listensocket"


struct listensocket_container_tag {
    size_t refc;
    mutex_t lock;
    listensocket_t **sock;
    int *sockref;
    size_t sock_len;
    void (*sockcount_cb)(size_t count, void *userdata);
    void *sockcount_userdata;
};
struct listensocket_tag {
    size_t refc;
    size_t sockrefc;
    mutex_t lock;
    rwlock_t listener_rwlock;
    listener_t *listener;
    listener_t *listener_update;
    sock_t sock;
};

static int listensocket_container_configure__unlocked(listensocket_container_t *self, const ice_config_t *config);
static int listensocket_container_setup__unlocked(listensocket_container_t *self);
static ssize_t listensocket_container_sockcount__unlocked(listensocket_container_t *self);
static listensocket_t * listensocket_new(const listener_t *listener);
static int              listensocket_apply_config(listensocket_t *self);
static int              listensocket_apply_config__unlocked(listensocket_t *self);
static int              listensocket_set_update(listensocket_t *self, const listener_t *listener);
#ifdef HAVE_POLL
static inline int listensocket__poll_fill(listensocket_t *self, struct pollfd *p);
#else
static inline int listensocket__select_set(listensocket_t *self, fd_set *set, int *max);
static inline int listensocket__select_isset(listensocket_t *self, fd_set *set);
#endif

static inline const char * __string_default(const char *str, const char *def)
{
    return str != NULL ? str : def;
}

static inline int __listener_cmp(const listener_t *a, const listener_t *b)
{
    if (a == b)
        return 1;

    if (a->port != b->port)
        return 0;

    if ((a->bind_address == NULL && b->bind_address != NULL) ||
        (a->bind_address != NULL && b->bind_address == NULL))
        return 0;


    if (a->bind_address != NULL && b->bind_address != NULL && strcmp(a->bind_address, b->bind_address) != 0)
        return 0;

    return 1;
}

static inline void __call_sockcount_cb(listensocket_container_t *self)
{
    if (self->sockcount_cb == NULL)
        return;

    self->sockcount_cb(listensocket_container_sockcount__unlocked(self), self->sockcount_userdata);
}

listensocket_container_t *  listensocket_container_new(void)
{
    listensocket_container_t *self = calloc(1, sizeof(*self));
    if (!self)
        return NULL;

    self->refc = 1;
    self->sock = NULL;
    self->sock_len = 0;
    self->sockcount_cb = NULL;
    self->sockcount_userdata = NULL;

    thread_mutex_create(&self->lock);

    return self;
}

int                         listensocket_container_ref(listensocket_container_t *self)
{
    if (!self)
        return -1;

    thread_mutex_lock(&self->lock);
    self->refc++;
    thread_mutex_unlock(&self->lock);
    return 0;
}

static void __listensocket_container_clear_sockets(listensocket_container_t *self)
{
    size_t i;

    if (self->sock == NULL)
        return;

    for (i = 0; i < self->sock_len; i++) {
        if (self->sock[i] != NULL) {
            if (self->sockref[i]) {
                listensocket_unrefsock(self->sock[i]);
            }
            listensocket_unref(self->sock[i]);
            self->sock[i] = NULL;
        }
    }

    self->sock_len = 0;
    free(self->sock);
    free(self->sockref);
    self->sock = NULL;
    self->sockref = NULL;

    __call_sockcount_cb(self);
}

int                         listensocket_container_unref(listensocket_container_t *self)
{
    if (!self)
        return -1;

    thread_mutex_lock(&self->lock);
    self->refc--;
    if (self->refc) {
        thread_mutex_unlock(&self->lock);
        return 0;
    }

    __listensocket_container_clear_sockets(self);

    thread_mutex_unlock(&self->lock);
    thread_mutex_destroy(&self->lock);
    free(self);
    return 0;
}

static inline void __find_matching_entry(listensocket_container_t *self, const listener_t *listener, listensocket_t ***found, int **ref)
{
    const listener_t *b;
    int test;
    size_t i;

    for (i = 0; i < self->sock_len; i++) {
        if (self->sock[i] != NULL) {
            if (self->sockref[i]) {
                b = listensocket_get_listener(self->sock[i]);
                test = __listener_cmp(listener, b);
                listensocket_release_listener(self->sock[i]);
                if (test == 1) {
                    *found = &(self->sock[i]);
                    *ref = &(self->sockref[i]);
                    return;
                }
            }
        }
    }

    *found = NULL;
    *ref = NULL;
}

int                         listensocket_container_configure(listensocket_container_t *self, const ice_config_t *config)
{
    int ret;

    if (!self)
        return -1;

    thread_mutex_lock(&self->lock);
    ret = listensocket_container_configure__unlocked(self, config);
    thread_mutex_unlock(&self->lock);

    return ret;
}

static int listensocket_container_configure__unlocked(listensocket_container_t *self, const ice_config_t *config)
{
    listensocket_t **n;
    listensocket_t **match;
    listener_t *cur;
    int *r;
    int *m;
    size_t i;

    if (!self || !config)
        return -1;

    if (!config->listen_sock_count) {
        __listensocket_container_clear_sockets(self);
        return 0;
    }

    n = calloc(config->listen_sock_count, sizeof(listensocket_t *));
    r = calloc(config->listen_sock_count, sizeof(int));
    if (!n || !r) {
        free(n);
        free(r);
        return -1;
    }

    cur = config->listen_sock;
    for (i = 0; i < config->listen_sock_count; i++) {
        if (cur) {
            __find_matching_entry(self, cur, &match, &m);
            if (match) {
                n[i] = *match;
                r[i] = 1;
                *match = NULL;
                *m = 0;
                listensocket_set_update(n[i], cur);
            } else {
                n[i] = listensocket_new(cur);
            }
        } else {
            n[i] = NULL;
        }
        if (n[i] == NULL) {
            for (; i; i--) {
                listensocket_unref(n[i - 1]);
            }
            return -1;
        }

        cur = cur->next;
    }

    __listensocket_container_clear_sockets(self);

    self->sock = n;
    self->sockref = r;
    self->sock_len = config->listen_sock_count;

    return 0;
}

int                         listensocket_container_configure_and_setup(listensocket_container_t *self, const ice_config_t *config)
{
    void (*cb)(size_t count, void *userdata);
    int ret;

    if (!self)
        return -1;

    thread_mutex_lock(&self->lock);
    cb = self->sockcount_cb;
    self->sockcount_cb = NULL;

    if (listensocket_container_configure__unlocked(self, config) == 0) {
        ret = listensocket_container_setup__unlocked(self);
    } else {
        ret = -1;
    }

    self->sockcount_cb = cb;
    __call_sockcount_cb(self);
    thread_mutex_unlock(&self->lock);

    return ret;
}

int                         listensocket_container_setup(listensocket_container_t *self)
{
    int ret;

    if (!self)
        return -1;

    thread_mutex_lock(&self->lock);
    ret = listensocket_container_setup__unlocked(self);
    thread_mutex_unlock(&self->lock);

    return ret;
}
static int listensocket_container_setup__unlocked(listensocket_container_t *self)
{
    listener_type_t type;
    size_t i;
    int ret = 0;

    for (i = 0; i < self->sock_len; i++) {
        listensocket_apply_config(self->sock[i]);

        type = listensocket_get_type(self->sock[i]);
        if (self->sockref[i] && type == LISTENER_TYPE_VIRTUAL) {
            if (listensocket_unrefsock(self->sock[i]) == 0) {
                self->sockref[i] = 0;
            }
        } else if (!self->sockref[i] && type != LISTENER_TYPE_VIRTUAL) {
            if (listensocket_refsock(self->sock[i]) == 0) {
                self->sockref[i] = 1;
            } else {
                ICECAST_LOG_DEBUG("Can not ref socket.");
                ret = 1;
            }
        }
    }

    __call_sockcount_cb(self);

    return ret;
}

static connection_t *       listensocket_container_accept__inner(listensocket_container_t *self, int timeout)
{
#ifdef HAVE_POLL
    struct pollfd ufds[self->sock_len];
    listensocket_t *socks[self->sock_len];
    size_t i, found, p;
    int ok;
    int ret;

    for (i = 0, found = 0; i < self->sock_len; i++) {
        ok = self->sockref[i];

        if (ok && listensocket__poll_fill(self->sock[i], &(ufds[found])) == -1) {
            ICECAST_LOG_WARN("Can not poll on closed socket.");
            ok = 0;
        }

        if (ok) {
            socks[found] = self->sock[i];
            found++;
        }
    }

    if (!found) {
        ICECAST_LOG_ERROR("No sockets found to poll on.");
        return NULL;
    }

    ret = poll(ufds, found, timeout);
    if (ret <= 0)
        return NULL;

    for (i = 0; i < found; i++) {
        if (ufds[i].revents & POLLIN) {
            return listensocket_accept(socks[i]);
        }

        if (!(ufds[i].revents & (POLLHUP|POLLERR|POLLNVAL)))
            continue;

        for (p = 0; p < self->sock_len; p++) {
            if (self->sock[p] == socks[i]) {
                if (self->sockref[p]) {
                    ICECAST_LOG_ERROR("Closing listen socket in error state.");
                    listensocket_unrefsock(socks[i]);
                    self->sockref[p] = 0;
                    __call_sockcount_cb(self);
                }
            }
        }
    }

    return NULL;
#else
    fd_set rfds;
    size_t i;
    struct timeval tv, *p=NULL;
    int ret;
    int max = -1;

    FD_ZERO(&rfds);

    if (timeout >= 0) {
        tv.tv_sec = timeout/1000;
        tv.tv_usec = (timeout % 1000) * 1000;
        p = &tv;
    }

    for (i = 0; i < self->sock_len; i++) {
        if (self->sockref[i]) {
            listensocket__select_set(self->sock[i], &rfds, &max);
        }
    }

    ret = select(max+1, &rfds, NULL, NULL, p);
    if (ret <= 0)
        return NULL;

    for (i = 0; i < self->sock_len; i++) {
        if (self->sockref[i]) {
            if (listensocket__select_isset(self->sock[i], &rfds)) {
                return listensocket_accept(self->sock[i]);
            }
        }
    }

    return NULL;
#endif
}
connection_t *              listensocket_container_accept(listensocket_container_t *self, int timeout)
{
    connection_t *ret;

    if (!self)
        return NULL;

    thread_mutex_lock(&self->lock);
    ret = listensocket_container_accept__inner(self, timeout);
    thread_mutex_unlock(&self->lock);
    return ret;
}

int                         listensocket_container_set_sockcount_cb(listensocket_container_t *self, void (*cb)(size_t count, void *userdata), void *userdata)
{
    if (!self)
        return -1;

    thread_mutex_lock(&self->lock);
    self->sockcount_cb = cb;
    self->sockcount_userdata = userdata;
    thread_mutex_unlock(&self->lock);

    return 0;
}

ssize_t                     listensocket_container_sockcount(listensocket_container_t *self)
{
    ssize_t ret;

    if (!self)
        return -1;

    thread_mutex_lock(&self->lock);
    ret = listensocket_container_sockcount__unlocked(self);
    thread_mutex_unlock(&self->lock);

    return ret;
}

static ssize_t listensocket_container_sockcount__unlocked(listensocket_container_t *self)
{
    ssize_t count = 0;
    size_t i;

    for (i = 0; i < self->sock_len; i++) {
        if (self->sockref[i]) {
            count++;
        }
    }

    return count;
}

/* ---------------------------------------------------------------------------- */

static listensocket_t * listensocket_new(const listener_t *listener) {
    listensocket_t *self;

    if (listener == NULL)
        return NULL;

    self = calloc(1, sizeof(*self));
    if (!self)
        return NULL;

    self->refc = 1;
    self->sock = SOCK_ERROR;

    thread_mutex_create(&self->lock);
    thread_rwlock_create(&self->listener_rwlock);

    self->listener = config_copy_listener_one(listener);
    if (self->listener == NULL) {
        listensocket_unref(self);
        return NULL;
    }

    return self;
}
int                         listensocket_ref(listensocket_t *self)
{
    if (!self)
        return -1;

    thread_mutex_lock(&self->lock);
    self->refc++;
    thread_mutex_unlock(&self->lock);
    return 0;
}

int                         listensocket_unref(listensocket_t *self)
{
    if (!self)
        return -1;

    thread_mutex_lock(&self->lock);
    self->refc--;
    if (self->refc) {
        thread_mutex_unlock(&self->lock);
        return 0;
    }

    if (self->sockrefc) {
        ICECAST_LOG_ERROR("BUG: self->sockrefc == 0 && self->sockrefc == %lli", (long long unsigned)self->sockrefc);
        self->sockrefc = 1;
        listensocket_unrefsock(self);
    }

    while ((self->listener_update = config_clear_listener(self->listener_update)));
    thread_rwlock_wlock(&self->listener_rwlock);
    while ((self->listener = config_clear_listener(self->listener)));
    thread_rwlock_unlock(&self->listener_rwlock);
    thread_rwlock_destroy(&self->listener_rwlock);
    thread_mutex_unlock(&self->lock);
    thread_mutex_destroy(&self->lock);
    free(self);
    return 0;
}

static int              listensocket_apply_config(listensocket_t *self)
{
    int ret;

    if (!self)
        return -1;

    thread_mutex_lock(&self->lock);
    ret = listensocket_apply_config__unlocked(self);
    thread_mutex_unlock(&self->lock);

    return ret;
}

static int              listensocket_apply_config__unlocked(listensocket_t *self)
{
    const listener_t *listener;

    if (!self)
        return -1;

    thread_rwlock_wlock(&self->listener_rwlock);
    if (self->listener_update) {
        if (__listener_cmp(self->listener, self->listener_update) != 1) {
            ICECAST_LOG_ERROR("Tried to apply incomplete configuration to listensocket: bind address missmatch: have %s:%i, got %s:%i",
                                __string_default(self->listener->bind_address, "<ANY>"),
                                self->listener->port,
                                __string_default(self->listener_update->bind_address, "<ANY>"),
                                self->listener_update->port
                             );
            thread_rwlock_unlock(&self->listener_rwlock);
            return -1;
        }

        listener = self->listener_update;
    } else {
        listener = self->listener;
    }

    if (self->sock != SOCK_ERROR) {
        if (listener->so_sndbuf)
            sock_set_send_buffer(self->sock, listener->so_sndbuf);

        sock_set_blocking(self->sock, 0);
    }

    if (self->listener_update) {
        while ((self->listener = config_clear_listener(self->listener)));
        self->listener = self->listener_update;
        self->listener_update = NULL;
    }

    thread_rwlock_unlock(&self->listener_rwlock);

    return 0;
}

static int              listensocket_set_update(listensocket_t *self, const listener_t *listener)
{
    listener_t *n;

    if (!self || !listener)
        return -1;

    n = config_copy_listener_one(listener);
    if (n == NULL)
        return -1;

    thread_mutex_lock(&self->lock);
    while ((self->listener_update = config_clear_listener(self->listener_update)));
    self->listener_update = n;
    thread_mutex_unlock(&self->lock);
    return 0;
}

int                         listensocket_refsock(listensocket_t *self)
{
    if (!self)
        return -1;

    thread_mutex_lock(&self->lock);
    if (self->sockrefc) {
        self->sockrefc++;
        thread_mutex_unlock(&self->lock);
        return 0;
    }

    thread_rwlock_rlock(&self->listener_rwlock);
    self->sock = sock_get_server_socket(self->listener->port, self->listener->bind_address);
    thread_rwlock_unlock(&self->listener_rwlock);
    if (self->sock == SOCK_ERROR) {
        thread_mutex_unlock(&self->lock);
        return -1;
    }

    if (sock_listen(self->sock, ICECAST_LISTEN_QUEUE) == 0) {
        sock_close(self->sock);
        self->sock = SOCK_ERROR;
        thread_rwlock_rlock(&self->listener_rwlock);
        ICECAST_LOG_ERROR("Can not listen on socket: %s port %i", __string_default(self->listener->bind_address, "<ANY>"), self->listener->port);
        thread_rwlock_unlock(&self->listener_rwlock);
        thread_mutex_unlock(&self->lock);
        return -1;
    }

    if (listensocket_apply_config__unlocked(self) == -1) {
        thread_mutex_unlock(&self->lock);
        return -1;
    }

    self->sockrefc++;
    thread_mutex_unlock(&self->lock);

    return 0;
}

int                         listensocket_unrefsock(listensocket_t *self)
{
    if (!self)
        return -1;

    thread_mutex_lock(&self->lock);
    self->sockrefc--;
    if (self->sockrefc) {
        thread_mutex_unlock(&self->lock);
        return 0;
    }

    if (self->sock == SOCK_ERROR) {
        thread_mutex_unlock(&self->lock);
        return 0;
    }

    sock_close(self->sock);
    self->sock = SOCK_ERROR;
    thread_mutex_unlock(&self->lock);

    return 0;
}

connection_t *              listensocket_accept(listensocket_t *self)
{
    connection_t *con;
    sock_t sock;
    char *ip;

    if (!self)
        return NULL;

    ip = calloc(MAX_ADDR_LEN, 1);
    if (!ip)
        return NULL;

    thread_mutex_lock(&self->lock);
    sock = sock_accept(self->sock, ip, MAX_ADDR_LEN);
    thread_mutex_unlock(&self->lock);
    if (sock == SOCK_ERROR) {
        free(ip);
        return NULL;
    }

    if (strncmp(ip, "::ffff:", 7) == 0) {
        memmove(ip, ip+7, strlen(ip+7)+1);
    }

    con = connection_create(sock, self, self, ip);
    if (con == NULL) {
        sock_close(sock);
        free(ip);
        return NULL;
    }

    return con;
}

const listener_t *          listensocket_get_listener(listensocket_t *self)
{
    const listener_t *ret;

    if (!self)
        return NULL;

    thread_mutex_lock(&self->lock);
    thread_rwlock_rlock(&self->listener_rwlock);
    ret = self->listener;
    thread_mutex_unlock(&self->lock);

    return ret;
}

int                         listensocket_release_listener(listensocket_t *self)
{
    if (!self)
        return -1;

    /* This is safe with no self->lock holding as unref requires a wlock.
     * A wlock can not be acquired when someone still holds the rlock.
     * In fact this must be done in unlocked state as otherwise we could end up in a
     * dead lock with some 3rd party holding the self->lock for an unrelated operation
     * waiting for a wlock to be come available.
     * -- ph3-der-loewe, 2018-05-11
     */
    thread_rwlock_unlock(&self->listener_rwlock);

    return 0;
}

listener_type_t             listensocket_get_type(listensocket_t *self)
{
    listener_type_t ret;

    if (!self)
        return LISTENER_TYPE_ERROR;

    thread_mutex_lock(&self->lock);
    ret = self->listener->type;
    thread_mutex_unlock(&self->lock);

    return ret;
}

#ifdef HAVE_POLL
static inline int listensocket__poll_fill(listensocket_t *self, struct pollfd *p)
{
    if (!self)
        return -1;

    thread_mutex_lock(&self->lock);
    if (self->sock == SOCK_ERROR) {
        thread_mutex_unlock(&self->lock);
        return -1;
    }

    memset(p, 0, sizeof(*p));
    p->fd = self->sock;
    p->events = POLLIN;
    p->revents = 0;

    thread_mutex_unlock(&self->lock);

    return 0;
}
#else
static inline int listensocket__select_set(listensocket_t *self, fd_set *set, int *max)
{
    if (!self)
        return -1;

    thread_mutex_lock(&self->lock);
    if (self->sock == SOCK_ERROR) {
        thread_mutex_unlock(&self->lock);
        return -1;
    }

    if (*max < self->sock)
        *max = self->sock;

    FD_SET(self->sock, set);
    thread_mutex_unlock(&self->lock);

    return 0;
}
static inline int listensocket__select_isset(listensocket_t *self, fd_set *set)
{
    int ret;

    if (!self)
        return -1;

    thread_mutex_lock(&self->lock);
    if (self->sock == SOCK_ERROR) {
        thread_mutex_unlock(&self->lock);
        return -1;
    }
    ret = FD_ISSET(self->sock, set);
    thread_mutex_unlock(&self->lock);
    return ret;

}
#endif
