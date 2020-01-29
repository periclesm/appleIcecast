/* -*- c-basic-offset: 8; -*- */
/* shout.c: Implementation of public libshout interface shout.h
 *
 *  Copyright (C) 2002-2004 the Icecast team <team@icecast.org>,
 *  Copyright (C) 2012-2019 Philipp "ph3-der-loewe" Schafft <lion@lion.leolix.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the Free
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * $Id$
 */

#ifdef HAVE_CONFIG_H
#   include <config.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_STRINGS_H
#   include <strings.h>
#endif
#include <errno.h>

#include "shout.h"

#include "sock.h"
#include "timing.h"
#include "httpp.h"

#include "shout_private.h"
#include "util.h"

#ifdef _MSC_VER
#   ifndef va_copy
#       define va_copy(ap1, ap2) memcpy(&ap1, &ap2, sizeof(va_list))
#   endif
#   define vsnprintf      _vsnprintf
#   define inline         _inline
#endif

/* -- local prototypes -- */
static int shout_cb_connection_callback(shout_connection_t *con, shout_event_t event, void *userdata, va_list ap);
static int try_connect(shout_t *self);

/* -- static data -- */
static int _initialized = 0;

/* -- public functions -- */

void shout_init(void)
{
    if (_initialized)
        return;

    sock_initialize();
    _initialized = 1;
}

void shout_shutdown(void)
{
    if (!_initialized)
        return;

    sock_shutdown();
    _initialized = 0;
}

shout_t *shout_new(void)
{
    shout_t *self;

    /* in case users haven't done this explicitly. Should we error
     * if not initialized instead? */
    shout_init();

    if (!(self = (shout_t*)calloc(1, sizeof(shout_t)))) {
        return NULL;
    }

    if (shout_set_host(self, LIBSHOUT_DEFAULT_HOST) != SHOUTERR_SUCCESS) {
        shout_free(self);
        return NULL;
    }

    if (shout_set_user(self, LIBSHOUT_DEFAULT_USER) != SHOUTERR_SUCCESS) {
        shout_free(self);
        return NULL;
    }

    if (shout_set_agent(self, LIBSHOUT_DEFAULT_USERAGENT) != SHOUTERR_SUCCESS) {
        shout_free(self);
        return NULL;
    }

    if (!(self->audio_info = _shout_util_dict_new())) {
        shout_free(self);
        return NULL;
    }

    if (!(self->meta = _shout_util_dict_new())) {
        shout_free(self);
        return NULL;
    }

    if (shout_set_meta(self, "name", "no name") != SHOUTERR_SUCCESS) {
        shout_free(self);
        return NULL;
    }

#ifdef HAVE_OPENSSL
    if (shout_set_allowed_ciphers(self, LIBSHOUT_DEFAULT_ALLOWED_CIPHERS) != SHOUTERR_SUCCESS) {
        shout_free(self);
        return NULL;
    }

    self->tls_mode      = SHOUT_TLS_AUTO;
#endif

    self->port      = LIBSHOUT_DEFAULT_PORT;
    self->format    = LIBSHOUT_DEFAULT_FORMAT;
    self->usage     = LIBSHOUT_DEFAULT_USAGE;
    self->protocol  = LIBSHOUT_DEFAULT_PROTOCOL;

    return self;
}

void shout_free(shout_t *self)
{
    if (!self)
        return;

    if (!self->connection)
        return;

    if (self->host)
        free(self->host);
    if (self->password)
        free(self->password);
    if (self->mount)
        free(self->mount);
    if (self->user)
        free(self->user);
    if (self->useragent)
        free(self->useragent);
    if (self->audio_info)
        _shout_util_dict_free (self->audio_info);
    if (self->meta)
        _shout_util_dict_free (self->meta);

#ifdef HAVE_OPENSSL
    if (self->ca_directory)
        free(self->ca_directory);
    if (self->ca_file)
        free(self->ca_file);
    if (self->allowed_ciphers)
        free(self->allowed_ciphers);
    if (self->client_certificate)
        free(self->client_certificate);
#endif

    free(self);
}

int shout_open(shout_t *self)
{
    /* sanity check */
    if (!self)
        return SHOUTERR_INSANE;
    if (self->connection)
        return SHOUTERR_CONNECTED;
    if (!self->host || !self->password || !self->port)
        return self->error = SHOUTERR_INSANE;
    if (self->format == SHOUT_FORMAT_OGG &&  (self->protocol != SHOUT_PROTOCOL_HTTP && self->protocol != SHOUT_PROTOCOL_ROARAUDIO))
        return self->error = SHOUTERR_UNSUPPORTED;

    return self->error = try_connect(self);
}


int shout_close(shout_t *self)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (!self->connection)
        return self->error = SHOUTERR_UNCONNECTED;

    if (self->connection && self->connection->current_message_state == SHOUT_MSGSTATE_SENDING1 && self->close)
        self->close(self);

    shout_connection_unref(self->connection);
    self->connection = NULL;
    self->starttime = 0;
    self->senttime = 0;

    return self->error = SHOUTERR_SUCCESS;
}

int shout_send(shout_t *self, const unsigned char *data, size_t len)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (!self->connection || self->connection->current_message_state != SHOUT_MSGSTATE_SENDING1)
        return self->error = SHOUTERR_UNCONNECTED;

    if (self->starttime <= 0)
        self->starttime = timing_get_time();

    if (!len)
        return shout_connection_iter(self->connection, self);

    return self->send(self, data, len);
}

ssize_t shout_send_raw(shout_t *self, const unsigned char *data, size_t len)
{
    ssize_t ret;

    if (!self)
        return SHOUTERR_INSANE;

    if (!self->connection || self->connection->current_message_state != SHOUT_MSGSTATE_SENDING1)
        return SHOUTERR_UNCONNECTED;

    ret = shout_connection_send(self->connection, self, data, len);
    if (ret < 0)
       shout_connection_transfer_error(self->connection, self);
    return ret;
}

ssize_t shout_queuelen(shout_t *self)
{
    if (!self)
        return -1;

    return shout_connection_get_sendq(self->connection, self);
}


void shout_sync(shout_t *self)
{
    int64_t sleep;

    if (!self)
        return;

    if (self->senttime == 0)
        return;

    sleep = self->senttime / 1000 - (timing_get_time() - self->starttime);
    if (sleep > 0)
        timing_sleep((uint64_t)sleep);

}

int shout_delay(shout_t *self)
{

    if (!self)
        return 0;

    if (self->senttime == 0)
        return 0;

    return self->senttime / 1000 - (timing_get_time() - self->starttime);
}

shout_metadata_t *shout_metadata_new(void)
{
    return _shout_util_dict_new();
}

void shout_metadata_free(shout_metadata_t *self)
{
    if (!self)
        return;

    _shout_util_dict_free(self);
}

int shout_metadata_add(shout_metadata_t *self, const char *name, const char *value)
{
    if (!self || !name)
        return SHOUTERR_INSANE;

    return _shout_util_dict_set(self, name, value);
}

int shout_set_metadata(shout_t *self, shout_metadata_t *metadata)
{
    shout_connection_t *connection;
    shout_http_plan_t plan;
    size_t param_len;
    char *param = NULL;
    char *encvalue;
    char *encpassword;
    char *encmount;
    const char *param_template;
    int ret;
    int error;

    if (!self || !metadata)
        return SHOUTERR_INSANE;

    encvalue = _shout_util_dict_urlencode(metadata, '&');
    if (!encvalue)
        return self->error = SHOUTERR_MALLOC;

    memset(&plan, 0, sizeof(plan));

    plan.is_source = 0;

    switch (self->protocol) {
        case SHOUT_PROTOCOL_ICY:
            if (!(encpassword = _shout_util_url_encode(self->password))) {
                free(encvalue);
                return self->error = SHOUTERR_MALLOC;
            }

            param_template = "mode=updinfo&pass=%s&%s";
            param_len = strlen(param_template) + strlen(encvalue) + 1 + strlen(encpassword);
            param = malloc(param_len);
            if (!param) {
                free(encpassword);
                free(encvalue);
                return self->error = SHOUTERR_MALLOC;
            }
            snprintf(param, param_len, param_template, encpassword, encvalue);
            free(encpassword);

            plan.param = param;
            plan.fake_ua = 1;
            plan.auth = 0;
            plan.method = "GET";
            plan.resource = "/admin.cgi";
        break;
        case SHOUT_PROTOCOL_HTTP:
//            if (!(encmount = _shout_util_url_encode(self->mount))) {
//                free(encvalue);
//                return self->error = SHOUTERR_MALLOC;
//            }
//
//            param_template = "mode=updinfo&mount=%s&%s";
//            param_len = strlen(param_template) + strlen(encvalue) + 1 + strlen(encmount);
//            param = malloc(param_len);
//            if (!param) {
//                free(encmount);
//                free(encvalue);
//                return self->error = SHOUTERR_MALLOC;
//            }
//            snprintf(param, param_len, param_template, encmount, encvalue);
//            free(encmount);
//
//            plan.param = param;
//            plan.auth = 1;
//            plan.resource = "/admin/metadata";
            shout_set_http_metadata(self, metadata);
        break;
        case SHOUT_PROTOCOL_XAUDIOCAST:
            if (!(encmount = _shout_util_url_encode(self->mount))) {
                free(encvalue);
                return self->error = SHOUTERR_MALLOC;
            }
            if (!(encpassword = _shout_util_url_encode(self->password))) {
                free(encmount);
                free(encvalue);
                return self->error = SHOUTERR_MALLOC;
            }

            param_template = "mode=updinfo&pass=%s&mount=%s&%s";
            param_len = strlen(param_template) + strlen(encvalue) + 1 + strlen(encpassword) + strlen(self->mount);
            param = malloc(param_len);
            if (!param) {
                free(encpassword);
                free(encmount);
                free(encvalue);
                return self->error = SHOUTERR_MALLOC;
            }
            snprintf(param, param_len, param_template, encpassword, encmount, encvalue);
            free(encpassword);
            free(encmount);

            plan.param = param;
            plan.auth = 0;
            plan.method = "GET";
            plan.resource = "/admin.cgi";
        break;
        default:
            free(encvalue);
            return self->error = SHOUTERR_UNSUPPORTED;
        break;
    }

    free(encvalue);

    connection = shout_connection_new(self, shout_http_impl, &plan);
    if (!connection) {
        free(param);
        return self->error = SHOUTERR_MALLOC;
    }

    shout_connection_set_callback(self->connection, shout_cb_connection_callback, self);

#ifdef HAVE_OPENSSL
    shout_connection_select_tlsmode(connection, self->tls_mode);
#endif
    shout_connection_set_nonblocking(connection, 0);

    connection->target_message_state = SHOUT_MSGSTATE_PARSED_FINAL;

    shout_connection_connect(connection, self);

    ret = shout_connection_iter(connection, self);
    error = shout_connection_get_error(connection);

    shout_connection_unref(connection);

    free(param);

    if (ret == 0) {
        return SHOUTERR_SUCCESS;
    } else {
        return error;
    }
}

int shout_set_http_metadata(shout_t *self, shout_metadata_t *metadata)
{
    int         error;
    sock_t      socket = -1;
    int         rv;
    char       *encvalue = NULL;
    const char *request_template;
    char       *request = NULL;
    size_t      request_len;
    char       *auth = NULL;
    char       *mount = NULL;
#ifdef HAVE_OPENSSL
    shout_tls_t *tls = NULL;
#endif

    if (!self || !metadata)
        return SHOUTERR_INSANE;

    if (!(encvalue = _shout_util_dict_urlencode(metadata, '&')))
        goto error_malloc;

    if (!(mount = _shout_util_url_encode(self->mount)))
        goto error_malloc;

    auth = shout_http_basic_authorization(self);
    
    request_template = "GET /admin/metadata?mode=updinfo&mount=%s&%s HTTP/1.0\r\nUser-Agent: %s\r\n%s\r\n";
    request_len = strlen(request_template) + strlen(mount) + strlen(encvalue) + strlen(shout_get_agent(self)) + 1;
    if (auth)
        request_len += strlen(auth);
    if (!(request = malloc(request_len)))
        goto error_malloc;
    snprintf(request, request_len, request_template, mount, encvalue, shout_get_agent(self), auth ? auth : "");

    free(encvalue);
    encvalue = NULL;

    free(mount);
    mount = NULL;

    if (auth)
        free(auth);
    auth = NULL;

    if ((socket = sock_connect(self->host, self->port)) <= 0)
        return SHOUTERR_NOCONNECT;

#ifdef HAVE_OPENSSL
    switch (self->tls_mode_used) {
        case SHOUT_TLS_DISABLED:
            /* nothing to do */
            break;

        case SHOUT_TLS_RFC2817:
            /* Use TLS via HTTP Upgrade:-header [RFC2817]. */
            do {
                /* use a subscope to avoid more function level variables */
                char    upgrade[512];
                size_t  len;

                /* send upgrade request */
                snprintf(upgrade, sizeof(upgrade),
                         "GET / HTTP/1.1\r\nConnection: Upgrade\r\nUpgrade: TLS/1.0\r\nHost: %s:%i\r\n\r\n",
                         self->host, self->port);

                upgrade[sizeof(upgrade) - 1] = 0;
                len = strlen(upgrade);
                if (len == (sizeof(upgrade) - 1))
                    goto error_malloc;

                rv = sock_write_bytes(socket, upgrade, len);
                if (len != (size_t)rv)
                    goto error_socket;

                /* read status line */
                if (!sock_read_line(socket, upgrade, sizeof(upgrade)))
                    goto error_socket;
                if (strncmp(upgrade, "HTTP/1.1 101 ", 13) != 0)
                    goto error_socket;

                /* read headers */
                len = 0;
                do {
                    if (!sock_read_line(socket, upgrade, sizeof(upgrade)))
                        goto error_socket;
                    if (upgrade[0] == 0)
                        break;
                    if (!strncasecmp(upgrade, "Content-Length: ", 16) == 0)
                        len = atoi(upgrade + 16);
                } while (1);

                /* read body */
                while (len) {
                    rv = sock_read_bytes(socket, upgrade, len > sizeof(upgrade) ? sizeof(upgrade) : len);
                    if (rv < 1)
                        goto error_socket;
                    len -= rv;
                }
            } while (0);
            /* fall thru */

        case SHOUT_TLS_RFC2818:
            /* Use TLS for transport layer like HTTPS [RFC2818] does. */
            tls = shout_tls_new(self, socket);
            if (!tls)
                goto error_malloc;
            error = shout_tls_try_connect(tls);
            if (error != SHOUTERR_SUCCESS)
                goto error;
            break;

        default:
            /* Bad mode or auto detection not completed. */
            error = SHOUTERR_INSANE;
            goto error;
            break;
    }
#endif

#ifdef HAVE_OPENSSL
    if (tls) {
        rv = shout_tls_write(tls, request, strlen(request));
    } else {
        rv = sock_write(socket, "%s", request);
    }
#else
    rv = sock_write(socket, "%s", request);
#endif

    if (!rv)
        goto error_socket;

    error = SHOUTERR_SUCCESS;
    goto error;

error_socket:
    error = SHOUTERR_SOCKET;
    goto error;
error_malloc:
    error = SHOUTERR_MALLOC;
    goto error;
error:
#ifdef HAVE_OPENSSL
    if (tls)
        shout_tls_close(tls);
#endif
    if (socket != -1)
        sock_close(socket);
    if (encvalue)
        free(encvalue);
    if (request)
        free(request);
    if (auth)
        free(auth);
    if (mount)
        free(mount);
    return error;
}


/* getters/setters */
const char *shout_version(int *major, int *minor, int *patch)
{
//    if (major)
//        *major = LIBSHOUT_MAJOR;
//    if (minor)
//        *minor = LIBSHOUT_MINOR;
//    if (patch)
//        *patch = LIBSHOUT_MICRO;
//
//    return VERSION;
    
    return "2.4.3";
}

int shout_get_errno(shout_t *self)
{
    return self->error;
}

const char *shout_get_error(shout_t *self)
{
    if (!self)
        return "Invalid shout_t";

    switch (self->error) {
    case SHOUTERR_SUCCESS:
        return "No error";
    case SHOUTERR_INSANE:
        return "Nonsensical arguments";
    case SHOUTERR_NOCONNECT:
        return "Couldn't connect";
    case SHOUTERR_NOLOGIN:
        return "Login failed";
    case SHOUTERR_SOCKET:
        return "Socket error";
    case SHOUTERR_MALLOC:
        return "Out of memory";
    case SHOUTERR_CONNECTED:
        return "Cannot set parameter while connected";
    case SHOUTERR_UNCONNECTED:
        return "Not connected";
    case SHOUTERR_BUSY:
        return "Socket is busy";
    case SHOUTERR_UNSUPPORTED:
        return "This libshout doesn't support the requested option";
    case SHOUTERR_NOTLS:
        return "TLS requested but not supported by peer";
    case SHOUTERR_TLSBADCERT:
        return "TLS connection can not be established because of bad certificate";
    case SHOUTERR_RETRY:
        return "Please retry current operation.";
    default:
        return "Unknown error";
    }
}

/* Returns:
 *   SHOUTERR_CONNECTED if the connection is open,
 *   SHOUTERR_UNCONNECTED if it has not yet been opened,
 *   or an error from try_connect, including SHOUTERR_BUSY
 */
int shout_get_connected(shout_t *self)
{
    int rc;

    if (!self)
        return SHOUTERR_INSANE;

    if (self->connection && self->connection->current_message_state == SHOUT_MSGSTATE_SENDING1)
        return SHOUTERR_CONNECTED;
    if (self->connection && self->connection->current_message_state != SHOUT_MSGSTATE_SENDING1) {
        if ((rc = try_connect(self)) == SHOUTERR_SUCCESS)
            return SHOUTERR_CONNECTED;
        return rc;
    }

    return SHOUTERR_UNCONNECTED;
}

int shout_set_host(shout_t *self, const char *host)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (self->connection)
        return self->error = SHOUTERR_CONNECTED;

    if (self->host)
        free(self->host);

    if ( !(self->host = _shout_util_strdup(host)) )
        return self->error = SHOUTERR_MALLOC;

    return self->error = SHOUTERR_SUCCESS;
}

const char *shout_get_host(shout_t *self)
{
    if (!self)
        return NULL;

    return self->host;
}

int shout_set_port(shout_t *self, unsigned short port)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (self->connection)
        return self->error = SHOUTERR_CONNECTED;

    self->port = port;

    return self->error = SHOUTERR_SUCCESS;
}

unsigned short shout_get_port(shout_t *self)
{
    if (!self)
        return 0;

    return self->port;
}

int shout_set_password(shout_t *self, const char *password)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (self->connection)
        return self->error = SHOUTERR_CONNECTED;

    if (self->password)
        free(self->password);

    if ( !(self->password = _shout_util_strdup(password)) )
        return self->error = SHOUTERR_MALLOC;

    return self->error = SHOUTERR_SUCCESS;
}

const char* shout_get_password(shout_t *self)
{
    if (!self)
        return NULL;

    return self->password;
}

int shout_set_mount(shout_t *self, const char *mount)
{
    size_t len;

    if (!self || !mount)
        return SHOUTERR_INSANE;

    if (self->connection)
        return self->error = SHOUTERR_CONNECTED;

    if (self->mount)
        free(self->mount);

    len = strlen(mount) + 1;
    if (mount[0] != '/')
        len++;

    if ( !(self->mount = malloc(len)) )
        return self->error = SHOUTERR_MALLOC;

    snprintf(self->mount, len, "%s%s", mount[0] == '/' ? "" : "/", mount);

    return self->error = SHOUTERR_SUCCESS;
}

const char *shout_get_mount(shout_t *self)
{
    if (!self)
        return NULL;

    return self->mount;
}

int shout_set_name(shout_t *self, const char *name)
{
    return shout_set_meta(self, "name", name);
}

const char *shout_get_name(shout_t *self)
{
    return shout_get_meta(self, "name");
}

int shout_set_url(shout_t *self, const char *url)
{
    return shout_set_meta(self, "url", url);
}

const char *shout_get_url(shout_t *self)
{
    return shout_get_meta(self, "url");
}

int shout_set_genre(shout_t *self, const char *genre)
{
    return shout_set_meta(self, "genre", genre);
}

const char *shout_get_genre(shout_t *self)
{
    return shout_get_meta(self, "genre");
}

int shout_set_agent(shout_t *self, const char *agent)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (self->connection)
        return self->error = SHOUTERR_CONNECTED;

    if (self->useragent)
        free(self->useragent);

    if ( !(self->useragent = _shout_util_strdup(agent)) )
        return self->error = SHOUTERR_MALLOC;

    return self->error = SHOUTERR_SUCCESS;
}

const char *shout_get_agent(shout_t *self)
{
    if (!self)
        return NULL;

    return self->useragent;
}


int shout_set_user(shout_t *self, const char *username)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (self->connection)
        return self->error = SHOUTERR_CONNECTED;

    if (self->user)
        free(self->user);

    if ( !(self->user = _shout_util_strdup(username)) )
        return self->error = SHOUTERR_MALLOC;

    return self->error = SHOUTERR_SUCCESS;
}

const char *shout_get_user(shout_t *self)
{
    if (!self)
        return NULL;

    return self->user;
}

int shout_set_description(shout_t *self, const char *description)
{
    return shout_set_meta(self, "description", description);
}

const char *shout_get_description(shout_t *self)
{
    return shout_get_meta(self, "description");
}

int shout_set_dumpfile(shout_t *self, const char *dumpfile)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (self->connection)
        return SHOUTERR_CONNECTED;

    if (self->dumpfile)
        free(self->dumpfile);

    if ( !(self->dumpfile = _shout_util_strdup(dumpfile)) )
        return self->error = SHOUTERR_MALLOC;

    return self->error = SHOUTERR_SUCCESS;
}

const char *shout_get_dumpfile(shout_t *self)
{
    if (!self)
        return NULL;

    return self->dumpfile;
}

int shout_set_audio_info(shout_t *self, const char *name, const char *value)
{
    if (!self)
        return SHOUTERR_INSANE;

    return self->error = _shout_util_dict_set(self->audio_info, name, value);
}

const char *shout_get_audio_info(shout_t *self, const char *name)
{
    if (!self)
        return NULL;

    return _shout_util_dict_get(self->audio_info, name);
}

int shout_set_meta(shout_t *self, const char *name, const char *value)
{
    size_t i;
    char c;

    if (!self || !name)
        return SHOUTERR_INSANE;

    if (self->connection)
        return self->error = SHOUTERR_CONNECTED;

    for (i = 0; (c = name[i]); i++) {
        if ((c < 'a' || c > 'z') && (c < '0' || c > '9'))
            return self->error = SHOUTERR_INSANE;
    }

    for (i = 0; (c = value[i]); i++) {
        if (c == '\r' || c == '\n')
            return self->error = SHOUTERR_INSANE;
    }

    return self->error = _shout_util_dict_set(self->meta, name, value);
}

const char *shout_get_meta(shout_t *self, const char *name)
{
    if (!self)
        return NULL;

    return _shout_util_dict_get(self->meta, name);
}

int shout_set_public(shout_t *self, unsigned int public)
{
    if (!self || (public != 0 && public != 1))
        return SHOUTERR_INSANE;

    if (self->connection)
        return self->error = SHOUTERR_CONNECTED;

    self->public = public;

    return self->error = SHOUTERR_SUCCESS;
}

unsigned int shout_get_public(shout_t *self)
{
    if (!self)
        return 0;

    return self->public;
}

int shout_set_format(shout_t *self, unsigned int format)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (self->connection)
        return self->error = SHOUTERR_CONNECTED;

    switch (format) {
        case SHOUT_FORMAT_OGG:
            return shout_set_content_format(self, SHOUT_FORMAT_OGG, SHOUT_USAGE_UNKNOWN, NULL);
        break;
        case SHOUT_FORMAT_MP3:
            return shout_set_content_format(self, SHOUT_FORMAT_MP3, SHOUT_USAGE_AUDIO, NULL);
        break;
        case SHOUT_FORMAT_AAC:
            return shout_set_content_format(self, SHOUT_FORMAT_AAC, SHOUT_USAGE_AUDIO, NULL);
        break;
        case SHOUT_FORMAT_WEBM:
            return shout_set_content_format(self, SHOUT_FORMAT_WEBM, SHOUT_USAGE_AUDIO|SHOUT_USAGE_VISUAL, NULL);
        break;
        case SHOUT_FORMAT_WEBMAUDIO:
            return shout_set_content_format(self, SHOUT_FORMAT_WEBM, SHOUT_USAGE_AUDIO, NULL);
        break;
    }

    return self->error = SHOUTERR_UNSUPPORTED;
}

unsigned int shout_get_format(shout_t* self)
{
    if (!self)
        return 0;

    if (self->format == SHOUT_FORMAT_WEBM && self->usage == SHOUT_USAGE_AUDIO) {
        return SHOUT_FORMAT_WEBMAUDIO;
    }

    return self->format;
}

static inline unsigned int remove_bits(unsigned int value, unsigned int to_remove)
{
    value |= to_remove;
    value -= to_remove;

    return value;
}

static inline int is_audio(unsigned int usage)
{
    if (!(usage & SHOUT_USAGE_AUDIO))
        return 0;

    if (remove_bits(usage, SHOUT_USAGE_AUDIO|SHOUT_USAGE_SUBTITLE))
        return 0;

    return 1;
}

static inline int is_video(unsigned int usage)
{
    if (!(usage & SHOUT_USAGE_VISUAL))
        return 0;

    if (remove_bits(usage, SHOUT_USAGE_VISUAL|SHOUT_USAGE_AUDIO|SHOUT_USAGE_SUBTITLE|SHOUT_USAGE_3D|SHOUT_USAGE_4D))
        return 0;

    return 1;
}

static const char *shout_get_mimetype(unsigned int format, unsigned int usage, const char *codecs)
{
    if (codecs)
        return NULL;

    switch (format) {
        case SHOUT_FORMAT_OGG:
            if (is_audio(usage)) {
                return "audio/ogg";
            } else if (is_video(usage)) {
                return "video/ogg";
            } else {
                return "application/ogg";
            }
        break;

        case SHOUT_FORMAT_MP3:
            /* MP3 *ONLY* support Audio. So all other values are outright invalid */
            if (usage == SHOUT_USAGE_AUDIO) {
                return "audio/mpeg";
            }
        break;
            
        case SHOUT_FORMAT_AAC:
            /* AAC *ONLY* support Audio. So all other values are outright invalid */
            if (usage == SHOUT_USAGE_AUDIO) {
                return "audio/aac";
            }
        break;
            
        case SHOUT_FORMAT_WEBM:
            if (is_audio(usage)) {
                return "audio/webm";
            } else if (is_video(usage)) {
                return "video/webm";
            }
        break;
            
        case SHOUT_FORMAT_MATROSKA:
            if (is_audio(usage)) {
                return "audio/x-matroska";
            } else if (is_video(usage) && (usage & SHOUT_USAGE_3D)) {
                return "video/x-matroska-3d";
            } else if (is_video(usage)) {
                return "video/x-matroska";
            }
        break;
    }

    return NULL;
}

const char *shout_get_mimetype_from_self(shout_t *self)
{
    return shout_get_mimetype(self->format, self->usage, NULL);
}

int shout_set_content_format(shout_t *self, unsigned int format, unsigned int usage, const char *codecs)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (self->connection)
        return self->error = SHOUTERR_CONNECTED;

    if (codecs) {
        return self->error = SHOUTERR_UNSUPPORTED;
    }

    if (!shout_get_mimetype(format, usage, codecs)) {
        return self->error = SHOUTERR_UNSUPPORTED;
    }

    self->format = format;
    self->usage  = usage;

    return self->error = SHOUTERR_SUCCESS;
}

int shout_get_content_format(shout_t *self, unsigned int *format, unsigned int *usage, const char **codecs)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (format)
        *format = self->format;

    if (usage)
        *usage = self->usage;

    if (codecs)
        *codecs = NULL;

    return self->error = SHOUTERR_SUCCESS;
}

int shout_set_protocol(shout_t *self, unsigned int protocol)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (self->connection)
        return self->error = SHOUTERR_CONNECTED;

    if (protocol != SHOUT_PROTOCOL_HTTP &&
        protocol != SHOUT_PROTOCOL_XAUDIOCAST &&
        protocol != SHOUT_PROTOCOL_ICY &&
        protocol != SHOUT_PROTOCOL_ROARAUDIO) {
        return self->error = SHOUTERR_UNSUPPORTED;
    }

    self->protocol = protocol;

    return self->error = SHOUTERR_SUCCESS;
}

unsigned int shout_get_protocol(shout_t *self)
{
    if (!self)
        return 0;

    return self->protocol;
}

int shout_set_nonblocking(shout_t *self, unsigned int nonblocking)
{
    if (!self || (nonblocking != 0 && nonblocking != 1))
        return SHOUTERR_INSANE;

    if (self->connection)
        return self->error = SHOUTERR_CONNECTED;

    self->nonblocking = nonblocking;

    return SHOUTERR_SUCCESS;
}

unsigned int shout_get_nonblocking(shout_t *self)
{
    if (!self)
        return 0;

    return self->nonblocking;
}

/* TLS functions */
#ifdef HAVE_OPENSSL
int shout_set_tls(shout_t *self, int mode)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (mode != SHOUT_TLS_DISABLED &&
        mode != SHOUT_TLS_AUTO &&
        mode != SHOUT_TLS_AUTO_NO_PLAIN &&
        mode != SHOUT_TLS_RFC2818 &&
        mode != SHOUT_TLS_RFC2817)
        return self->error = SHOUTERR_UNSUPPORTED;

    self->tls_mode = mode;
    return SHOUTERR_SUCCESS;
}
int shout_get_tls(shout_t *self)
{
    if (!self)
        return SHOUTERR_INSANE;

    return self->tls_mode;
}

int shout_set_ca_directory(shout_t *self, const char *directory)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (self->connection)
        return self->error = SHOUTERR_CONNECTED;

    if (self->ca_directory)
        free(self->ca_directory);

    if (!(self->ca_directory = _shout_util_strdup(directory)))
        return self->error = SHOUTERR_MALLOC;

    return self->error = SHOUTERR_SUCCESS;
}

const char *shout_get_ca_directory(shout_t *self)
{
    if (!self)
        return NULL;

    return self->ca_directory;
}

int shout_set_ca_file(shout_t *self, const char *file)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (self->connection)
        return self->error = SHOUTERR_CONNECTED;

    if (self->ca_file)
        free(self->ca_file);

    if (!(self->ca_file = _shout_util_strdup(file)))
        return self->error = SHOUTERR_MALLOC;

    return self->error = SHOUTERR_SUCCESS;
}

const char *shout_get_ca_file(shout_t *self)
{
    if (!self)
        return NULL;

    return self->ca_file;
}

int shout_set_allowed_ciphers(shout_t *self, const char *ciphers)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (self->connection)
        return self->error = SHOUTERR_CONNECTED;

    if (self->allowed_ciphers)
        free(self->allowed_ciphers);

    if (!(self->allowed_ciphers = _shout_util_strdup(ciphers)))
        return self->error = SHOUTERR_MALLOC;

    return self->error = SHOUTERR_SUCCESS;
}

const char *shout_get_allowed_ciphers(shout_t *self)
{
    if (!self)
        return NULL;

    return self->allowed_ciphers;
}

int shout_set_client_certificate(shout_t *self, const char *certificate)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (self->connection)
        return self->error = SHOUTERR_CONNECTED;

    if (self->client_certificate)
        free(self->client_certificate);

    if (!(self->client_certificate = _shout_util_strdup(certificate)))
        return self->error = SHOUTERR_MALLOC;

    return self->error = SHOUTERR_SUCCESS;
}

const char *shout_get_client_certificate(shout_t *self)
{
    if (!self)
        return NULL;

    return self->client_certificate;
}
#else
int shout_set_tls(shout_t *self, int mode)
{
    if (!self)
        return SHOUTERR_INSANE;

    if (mode == SHOUT_TLS_DISABLED)
        return SHOUTERR_SUCCESS;

    return self->error = SHOUTERR_UNSUPPORTED;
}

int shout_get_tls(shout_t *self)
{
    return SHOUT_TLS_DISABLED;
}

int shout_set_ca_directory(shout_t *self, const char *directory)
{
    if (!self)
        return SHOUTERR_INSANE;

    return self->error = SHOUTERR_UNSUPPORTED;
}

const char *shout_get_ca_directory(shout_t *self)
{
    return NULL;
}

int shout_set_ca_file(shout_t *self, const char *file)
{
    if (!self)
        return SHOUTERR_INSANE;

    return self->error = SHOUTERR_UNSUPPORTED;
}

const char *shout_get_ca_file(shout_t *self)
{
    return NULL;
}

int shout_set_allowed_ciphers(shout_t *self, const char *ciphers)
{
    if (!self)
        return SHOUTERR_INSANE;

    return self->error = SHOUTERR_UNSUPPORTED;
}
const char *shout_get_allowed_ciphers(shout_t *self)
{
    return NULL;
}

int shout_set_client_certificate(shout_t *self, const char *certificate)
{
    if (!self)
        return SHOUTERR_INSANE;
    return self->error = SHOUTERR_UNSUPPORTED;
}

const char *shout_get_client_certificate(shout_t *self)
{
    return NULL;
}
#endif

int shout_control(shout_t *self, shout_control_t control, ...)
{
    int ret = SHOUTERR_INSANE;
    va_list ap;

    if (!self)
        return SHOUTERR_INSANE;

    va_start(ap, control);

    switch (control) {
#ifdef HAVE_OPENSSL
        case SHOUT_CONTROL_GET_SERVER_CERTIFICATE_AS_PEM:
        case SHOUT_CONTROL_GET_SERVER_CERTIFICATE_CHAIN_AS_PEM:
            if (self->connection->tls) {
                void **vpp = va_arg(ap, void **);
                if (vpp) {
                    ret = shout_connection_control(self->connection, control, vpp);
                } else {
                    ret = SHOUTERR_INSANE;
                }
            } else {
                ret = SHOUTERR_BUSY;
            }
        break;
#else
        case SHOUT_CONTROL_GET_SERVER_CERTIFICATE_AS_PEM:
        case SHOUT_CONTROL_GET_SERVER_CERTIFICATE_CHAIN_AS_PEM:
            ret = SHOUTERR_UNSUPPORTED;
        break;
#endif
        case SHOUT_CONTROL__MIN:
        case SHOUT_CONTROL__MAX:
            ret = SHOUTERR_INSANE;
        break;
    }

    va_end(ap);

    return self->error = ret;
}
int shout_set_callback(shout_t *self, shout_callback_t callback, void *userdata)
{
    if (!self)
        return SHOUTERR_INSANE;

    self->callback = callback;
    self->callback_userdata = userdata;

    return self->error = SHOUTERR_SUCCESS;
}

/* -- static function definitions -- */
static int shout_call_callback(shout_t *self, shout_event_t event, ...)
{
    va_list ap;
    int ret;

    if (!self->callback)
        return SHOUT_CALLBACK_PASS;

    va_start(ap, event);
    ret = self->callback(self, event, self->callback_userdata, ap);
    va_end(ap);

    return ret;
}
static int shout_cb_connection_callback(shout_connection_t *con, shout_event_t event, void *userdata, va_list ap)
{
    shout_t *self = userdata;

    /* Avoid going up if not needed */
    if (!self->callback)
        return SHOUT_CALLBACK_PASS;

    switch (event) {
        case SHOUT_EVENT_TLS_CHECK_PEER_CERTIFICATE:
            return shout_call_callback(self, event, con);
        break;
        case SHOUT_EVENT__MIN:
        case SHOUT_EVENT__MAX:
            return SHOUTERR_INSANE;
        break;
    }

    return SHOUT_CALLBACK_PASS;
}

static int try_connect(shout_t *self)
{
    int ret;

    if (!self->connection) {
        const shout_protocol_impl_t *impl = NULL;

        switch (shout_get_protocol(self)) {
            case SHOUT_PROTOCOL_HTTP:
                impl = shout_http_impl;
                memset(&(self->source_plan.http), 0, sizeof(self->source_plan.http));
                self->source_plan.http.is_source = 1;
                self->source_plan.http.auth = 1;
                self->source_plan.http.resource = self->mount;
            break;
            case SHOUT_PROTOCOL_XAUDIOCAST:
                impl = shout_xaudiocast_impl;
            break;
            case SHOUT_PROTOCOL_ICY:
                impl = shout_icy_impl;
            break;
            case SHOUT_PROTOCOL_ROARAUDIO:
                impl = shout_roaraudio_impl;
            break;
        }

        self->connection = shout_connection_new(self, impl, &(self->source_plan));
        if (!self->connection)
            return self->error = SHOUTERR_MALLOC;

        shout_connection_set_callback(self->connection, shout_cb_connection_callback, self);

#ifdef HAVE_OPENSSL
        shout_connection_select_tlsmode(self->connection, self->tls_mode);
#endif
        self->connection->target_message_state = SHOUT_MSGSTATE_SENDING1;
        shout_connection_connect(self->connection, self);
    }

    ret = shout_connection_iter(self->connection, self);

    if (self->connection->current_message_state == SHOUT_MSGSTATE_SENDING1 && !self->send) {
        int rc;
        switch (self->format) {
            case SHOUT_FORMAT_OGG:
                rc = self->error = shout_open_ogg(self);
                break;
            case SHOUT_FORMAT_MP3:
                rc = self->error = shout_open_mp3(self);
                break;
            case SHOUT_FORMAT_AAC:
                rc = self->error = shout_open_aac(self);
                break;
            case SHOUT_FORMAT_WEBM:
            case SHOUT_FORMAT_MATROSKA:
                rc = self->error = shout_open_webm(self);
                break;

            default:
                rc = SHOUTERR_INSANE;
                break;
        }
        if (rc != SHOUTERR_SUCCESS) {
            return ret;
        }
    }

    return ret;
}
