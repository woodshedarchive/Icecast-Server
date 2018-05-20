/* Icecast
 *
 * This program is distributed under the GNU General Public License, version 2.
 * A copy of this license is included with this source.
 *
 * Copyright 2018,      Philipp "ph3-der-loewe" Schafft <lion@lion.leolix.org>,
 */

#ifndef __LISTENSOCKET_H__
#define __LISTENSOCKET_H__

typedef struct listensocket_container_tag listensocket_container_t;
typedef struct listensocket_tag listensocket_t;

#include "connection.h"
#include "cfgfile.h"

listensocket_container_t *  listensocket_container_new(void);
int                         listensocket_container_ref(listensocket_container_t *self);
int                         listensocket_container_unref(listensocket_container_t *self);
int                         listensocket_container_configure(listensocket_container_t *self, const ice_config_t *config);
int                         listensocket_container_configure_and_setup(listensocket_container_t *self, const ice_config_t *config);
int                         listensocket_container_setup(listensocket_container_t *self);
connection_t *              listensocket_container_accept(listensocket_container_t *self, int timeout);
int                         listensocket_container_set_sockcount_cb(listensocket_container_t *self, void (*cb)(size_t count, void *userdata), void *userdata);
ssize_t                     listensocket_container_sockcount(listensocket_container_t *self);

int                         listensocket_ref(listensocket_t *self);
int                         listensocket_unref(listensocket_t *self);
int                         listensocket_refsock(listensocket_t *self);
int                         listensocket_unrefsock(listensocket_t *self);
connection_t *              listensocket_accept(listensocket_t *self);
const listener_t *          listensocket_get_listener(listensocket_t *self);
int                         listensocket_release_listener(listensocket_t *self);
listener_type_t             listensocket_get_type(listensocket_t *self);

#endif
