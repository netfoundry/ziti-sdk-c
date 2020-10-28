/*
Copyright 2019-2020 NetFoundry, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <stdlib.h>

#include "zt_internal.h"
#include "utils.h"
#include "endian_internal.h"

static const char *TYPE_BIND = "Bind";
static const char *TYPE_DIAL = "Dial";

#define crypto(func) crypto_secretstream_xchacha20poly1305_##func

struct ziti_conn_req {
    struct ziti_conn *conn;
    char *service_name;
    const char *session_type;
    ziti_service *service;
    ziti_channel_t *channel;
    int chan_tries;
    ziti_conn_cb cb;

    uv_timer_t *conn_timeout;
    bool failed;

};

static void flush_to_client(uv_async_t *fl);

static void ziti_connect_async(uv_async_t *ar);

static int send_fin_message(ziti_connection conn);

int ziti_channel_start_connection(struct ziti_conn_req *req);

static void free_handle(uv_handle_t *h) {
    free(h);
}

static void free_conn_req(struct ziti_conn_req *r) {
    FREE(r->service_name);
    if (r->conn_timeout) {
        uv_close((uv_handle_t *) r->conn_timeout, free_handle);
    }
    free(r);
};

int close_conn_internal(struct ziti_conn *conn) {
    if (conn->state == Closed && conn->write_reqs == 0) {
        ZITI_LOG(VERBOSE, "removing connection[%d]", conn->conn_id);
        LIST_REMOVE(conn, next);
        FREE(conn->rx);
        conn->flusher->data = NULL;
        uv_close((uv_handle_t *) conn->flusher, free_handle);
        if (buffer_available(conn->inbound) > 0) {
            ZITI_LOG(WARN, "dumping %zd bytes of undelivered data conn[%d]",
                     buffer_available(conn->inbound), conn->conn_id);
        }
        free_buffer(conn->inbound);
        ZITI_LOG(TRACE, "connection[%d] is being free()'d", conn->conn_id);
        FREE(conn);
        return 1;
    }
    return 0;
}

void on_write_completed(struct ziti_conn *conn, struct ziti_write_req_s *req, int status) {
    ZITI_LOG(TRACE, "connection[%d] status %d", conn->conn_id, status);    
    if (req->conn == NULL) {
        ZITI_LOG(DEBUG, "write completed for timed out or closed connection");
        free(req);
        return;
    }
    conn->write_reqs--;

    if (req->timeout != NULL) {
        uv_timer_stop(req->timeout);
        uv_close((uv_handle_t *) req->timeout, free_handle);
    }

    if (req->cb != NULL) {
        if (status == 0) {
            status = req->len;
        }

        if (status < 0) {
            conn->state = Closed;
            ZITI_LOG(TRACE, "connection[%d] state is now Closed", conn->conn_id);
        }

        req->cb(conn, status, req->ctx);
    }

    free(req);

    if (conn->write_reqs == 0 && conn->state == CloseWrite) {
        ZITI_LOG(DEBUG, "sending FIN");
        send_fin_message(conn);
    }
}

static int
send_message(struct ziti_conn *conn, uint32_t content, uint8_t *body, uint32_t body_len, struct ziti_write_req_s *wr) {
    ziti_channel_t *ch = conn->channel;
    int32_t conn_id = htole32(conn->conn_id);
    int32_t msg_seq = htole32(conn->edge_msg_seq++);
    hdr_t headers[] = {
            {
                    .header_id = ConnIdHeader,
                    .length = sizeof(conn_id),
                    .value = (uint8_t *) &conn_id
            },
            {
                    .header_id = SeqHeader,
                    .length = sizeof(msg_seq),
                    .value = (uint8_t *) &msg_seq
            }
    };
    return ziti_channel_send(ch, content, headers, 2, body, body_len, wr);
}

static void on_channel_connected(ziti_channel_t *ch, void *ctx, int status) {
    struct ziti_conn_req *req = ctx;
    req->chan_tries--;

    // if channel was already selected
    if (req->channel != NULL) {
        ZITI_LOG(TRACE, "conn[%d] is already using another channel", req->conn->conn_id);
    }
    else {
        if (status < 0) {
            ZITI_LOG(ERROR, "ch[%d] failed to connect status[%d](%s)", ch->id, status, uv_strerror(status));
            model_map_remove(&ch->ctx->channels, ch->ingress);
        }
        else if (req->failed) {
            ZITI_LOG(DEBUG, "request already timed out or closed");
        }
        else { // first channel to connect
            ZITI_LOG(TRACE, "channel connected status[%d]", status);

            req->channel = ch;
            req->conn->channel = ch;
            req->chan_tries++;
            ziti_channel_start_connection(req);
        }
    }

    if (req->chan_tries == 0) {
        // callback was already called with timeout
        if (!req->failed && req->channel == NULL) { // no more outstanding channel tries
            req->conn->state = Closed;
            req->cb(req->conn, ZITI_GATEWAY_UNAVAILABLE);
        }
        free_conn_req(req);
    }
}

static void connect_timeout(uv_timer_t *timer) {
    struct ziti_conn_req *req = timer->data;
    struct ziti_conn *conn = req->conn;

    if (conn->state == Connecting) {
        ZITI_LOG(WARN, "ziti connection timed out");
        conn->state = Timedout;
        req->failed = true;
        req->cb(conn, ZITI_TIMEOUT);
    }
    else {
        ZITI_LOG(ERROR, "timeout for connection[%d] in unexpected state[%d]", conn->conn_id, conn->state);
    }
    uv_close((uv_handle_t *) timer, free_handle);
    req->conn_timeout = NULL;
}

static int ziti_connect(struct ziti_ctx *ctx, const ziti_net_session *session, struct ziti_conn_req *req) {
    struct ziti_conn *conn = req->conn;
    conn->token = session->token;

    ziti_edge_router **er;
    for (er = session->edge_routers; *er != NULL; er++) {
        req->chan_tries++;
        ZITI_LOG(TRACE, "connecting to %s(%s) for session[%s]", (*er)->name, (*er)->ingress.tls, conn->token);
        ziti_channel_connect(ctx, (*er)->ingress.tls, on_channel_connected, req);
    }

    return 0;
}

static void connect_get_service_cb(ziti_service* s, ziti_error *err, void *ctx) {
    uv_async_t *ar = ctx;
    struct ziti_conn_req *req = ar->data;
    struct ziti_ctx *ztx = req->conn->ziti_ctx;

    if (err != NULL) {
        ZITI_LOG(ERROR, "failed to load service (%s): %s(%s)", req->service_name, err->code, err->message);
    }
    if (s == NULL) {
        req->cb(req->conn, ZITI_SERVICE_UNAVAILABLE);
        free_conn_req(req);
    }
    else {
        ZITI_LOG(INFO, "got service[%s] id[%s]", s->name, s->id);
        for (int i = 0; s->permissions[i] != NULL; i++) {
            if (strcmp(s->permissions[i], "Dial") == 0) {
                 s->perm_flags |= ZITI_CAN_DIAL;
            }
            if (strcmp(s->permissions[i], "Bind") == 0) {
                s->perm_flags |= ZITI_CAN_BIND;
            }
        }

        model_map_set(&ztx->services, s->name, s);
        req->service = s;
        ziti_connect_async(ar);
    }

    free_ziti_error(err);
}

static void connect_get_net_session_cb(ziti_net_session * s, ziti_error *err, void *ctx) {
    uv_async_t *ar = ctx;
    struct ziti_conn_req *req = ar->data;
    struct ziti_ctx *ztx = req->conn->ziti_ctx;

    if (err != NULL) {
        ZITI_LOG(ERROR, "failed to load service[%s]: %s(%s)", req->service_name, err->code, err->message);
    }
    if (s == NULL) {
        req->cb(req->conn, ZITI_SERVICE_UNAVAILABLE);
        free_conn_req(req);
    }
    else {
        ZITI_LOG(INFO, "got session[%s] for service[%s]", s->id, req->service->name);
        s->service_id = strdup(req->service->id);
        model_map_set(&ztx->sessions, s->service_id, s);
        ziti_connect_async(ar);
    }

    free_ziti_error(err);
}

static void ziti_connect_async(uv_async_t *ar) {
    struct ziti_conn_req *req = ar->data;
    struct ziti_ctx *ctx = req->conn->ziti_ctx;
    uv_loop_t *loop = ar->loop;

    const ziti_net_session *net_session = NULL;

    // find service
    if (req->service == NULL) {
        req->service = model_map_get(&ctx->services, req->service_name);

        if (req->service == NULL) {
            ZITI_LOG(DEBUG, "service[%s] not loaded yet, requesting it", req->service_name);
            ziti_ctrl_get_service(&ctx->controller, req->service_name, connect_get_service_cb, ar);
            return;
        }
    }

    net_session = model_map_get(&ctx->sessions, req->service->id);
    if (net_session == NULL || strcmp(net_session->session_type, req->session_type) != 0) {
        ZITI_LOG(DEBUG, "requesting '%s' session for service[%s]", req->session_type, req->service_name);
        ziti_ctrl_get_net_session(&ctx->controller, req->service, req->session_type, connect_get_net_session_cb, ar);
        return;
    }
    else {
        req->conn_timeout = malloc(sizeof(uv_timer_t));
        uv_timer_init(loop, req->conn_timeout);
        req->conn_timeout->data = req;
        uv_timer_start(req->conn_timeout, connect_timeout, req->conn->timeout, 0);

        ZITI_LOG(DEBUG, "starting connection for service[%s] with session[%s]", req->service_name, net_session->id);
        ziti_connect(ctx, net_session, req);
    }

    uv_close((uv_handle_t *) ar, free_handle);
}

int ziti_dial(ziti_connection conn, const char *service, ziti_conn_cb conn_cb, ziti_data_cb data_cb) {

    PREPF(ziti, ziti_errorstr);
    if (conn->state != Initial) {
        TRY(ziti, ZITI_INVALID_STATE);
    }


    NEWP(req, struct ziti_conn_req);

    req->service_name = strdup(service);
    req->session_type = TYPE_DIAL;
    req->conn = conn;
    req->cb = conn_cb;

    conn->data_cb = data_cb;
    conn->state = Connecting;

    CATCH(ziti) {
        return ERR(ziti);
    }

    NEWP(async_cr, uv_async_t);
    uv_async_init(conn->ziti_ctx->loop, async_cr, ziti_connect_async);

    conn->flusher = calloc(1, sizeof(uv_async_t));
    uv_async_init(conn->ziti_ctx->loop, conn->flusher, flush_to_client);
    conn->flusher->data = conn;
    uv_unref((uv_handle_t *) conn->flusher);

    async_cr->data = req;

    return uv_async_send(async_cr);
}

static void ziti_write_timeout(uv_timer_t *t) {
    struct ziti_write_req_s *req = t->data;
    struct ziti_conn *conn = req->conn;

    conn->write_reqs--;
    req->timeout = NULL;
    req->conn = NULL;

    if (conn->state != Closed) {
        conn->state = Closed;
        req->cb(conn, ZITI_TIMEOUT, req->ctx);
    }

    uv_close((uv_handle_t *) t, free_handle);
}

static void ziti_write_async(uv_async_t *ar) {
    struct ziti_write_req_s *req = ar->data;
    struct ziti_conn *conn = req->conn;

    if (conn->state == Closed) {
        ZITI_LOG(WARN, "got write req for closed conn[%d]", conn->conn_id);
        conn->write_reqs--;

        req->cb(conn, ZITI_CONN_CLOSED, req->ctx);
        free(req);
    }
    else {
        if (req->cb) {
            req->timeout = calloc(1, sizeof(uv_timer_t));
            uv_timer_init(ar->loop, req->timeout);
            req->timeout->data = req;
            uv_timer_start(req->timeout, ziti_write_timeout, conn->timeout, 0);
        }

        if (conn->encrypted) {
            uint32_t crypto_len = req->len + crypto_secretstream_xchacha20poly1305_abytes();
            unsigned char *cipher_text = malloc(crypto_len);
            crypto_secretstream_xchacha20poly1305_push(&conn->crypt_o, cipher_text, NULL, req->buf, req->len, NULL, 0,
                                                       0);
            send_message(conn, ContentTypeData, cipher_text, crypto_len, req);
            free(cipher_text);
        }
        else {
            send_message(conn, ContentTypeData, req->buf, req->len, req);
        }
    }
    uv_close((uv_handle_t *) ar, free_handle);
}

int ziti_write_req(struct ziti_write_req_s *req) {
    NEWP(ar, uv_async_t);
    uv_async_init(req->conn->ziti_ctx->loop, ar, ziti_write_async);
    req->conn->write_reqs++;
    ar->data = req;

    if (uv_thread_self() == req->conn->ziti_ctx->loop_thread) {
        ziti_write_async(ar);
        return 0;
    }
    return uv_async_send(ar);
}

static void ziti_disconnect_cb(ziti_connection conn, ssize_t status, void *ctx) {
    conn->state = Closed;
}

static void ziti_disconnect_async(uv_async_t *ar) {
    struct ziti_conn *conn = ar->data;
    uv_close((uv_handle_t *) ar, free_handle);
    switch (conn->state) {
        case Bound:
        case Accepting:
        case Connected:
        case CloseWrite: {
            NEWP(wr, struct ziti_write_req_s);
            wr->conn = conn;
            wr->cb = ziti_disconnect_cb;
            conn->write_reqs++;
            send_message(conn, ContentTypeStateClosed, NULL, 0, wr);
            break;
        }

        default:
            ZITI_LOG(DEBUG, "conn[%d] can't send StateClosed in state[%d]", conn->conn_id, conn->state);
    }
}

int ziti_disconnect(struct ziti_conn *conn) {
    NEWP(ar, uv_async_t);
    uv_async_init(conn->channel->ctx->loop, ar, ziti_disconnect_async);
    ar->data = conn;
    return uv_async_send(ar);
}

static void crypto_wr_cb(ziti_connection conn, ssize_t status, void *ctx) {
    if (status < 0) {
        ZITI_LOG(ERROR, "crypto header write failed with status[%zd]", status);
        conn->state = Closed;
        conn->data_cb(conn, NULL, status);
    }
}

int establish_crypto(ziti_connection conn, message *msg) {

    size_t peer_key_len;
    uint8_t *peer_key;
    bool peer_key_sent = message_get_bytes_header(msg, PublicKeyHeader, &peer_key, &peer_key_len);
    if (!peer_key_sent) {
        if (conn->encrypted) {
            ZITI_LOG(ERROR, "conn[%d] failed to establish crypto for encrypted service: did not receive peer key",
                     conn->conn_id);
            return ZITI_CRYPTO_FAIL;
        }
        else {
            // service is not required to be encrypted and hosting side did not send the key
            return ZITI_OK;
        }
    }
    conn->encrypted = true;

    conn->tx = calloc(1, crypto_secretstream_xchacha20poly1305_KEYBYTES);
    conn->rx = calloc(1, crypto_secretstream_xchacha20poly1305_KEYBYTES);
    int rc;
    if (conn->state == Connecting) {
        rc = crypto_kx_client_session_keys(conn->rx, conn->tx, conn->pk, conn->sk, peer_key);
    }
    else if (conn->state == Accepting) {
        rc = crypto_kx_server_session_keys(conn->rx, conn->tx, conn->parent->pk, conn->parent->sk, peer_key);
    }
    else {
        ZITI_LOG(ERROR, "conn[%d] cannot establish crypto in %d state", conn->conn_id, conn->state);
        return ZITI_INVALID_STATE;
    }
    if (rc != 0) {
        ZITI_LOG(ERROR, "conn[%d] failed to establish encryption: crypto error", conn->state);
        return ZITI_CRYPTO_FAIL;
    }
    return ZITI_OK;
}

static int send_crypto_header(ziti_connection conn) {
    if (conn->encrypted) {
        NEWP(wr, struct ziti_write_req_s);
        wr->conn = conn;
        uint8_t *header = calloc(1, crypto_secretstream_xchacha20poly1305_headerbytes());
        wr->buf = header;
        wr->cb = crypto_wr_cb;

        crypto_secretstream_xchacha20poly1305_init_push(&conn->crypt_o, header, conn->tx);
        conn->write_reqs++;
        send_message(conn, ContentTypeData, header, crypto_secretstream_xchacha20poly1305_headerbytes(), wr);
        free(header);
        memset(conn->tx, 0, crypto_secretstream_xchacha20poly1305_KEYBYTES);
        FREE(conn->tx);
    }
    return ZITI_OK;
}

static void flush_to_client(uv_async_t *fl) {
    ziti_connection conn = fl->data;
    if (conn == NULL || conn->state == Closed) {
        return;
    }

    // if fin was received and all data is flushed, signal EOF
    if (conn->fin_recv && buffer_available(conn->inbound) == 0) {
        conn->data_cb(conn, NULL, ZITI_EOF);
        return;
    }

    ZITI_LOG(TRACE, "flushing %zd bytes to client", buffer_available(conn->inbound));

    while (buffer_available(conn->inbound) > 0) {
        uint8_t *chunk;
        ssize_t chunk_len = buffer_get_next(conn->inbound, 16 * 1024, &chunk);
        ssize_t consumed = conn->data_cb(conn, chunk, chunk_len);
        if (consumed < 0) {
            ZITI_LOG(WARN, "client conn[%d] indicated error[%zd] accepting data (%zd bytes buffered)",
                     conn->conn_id, consumed, buffer_available(conn->inbound));
        }
        else if (consumed < chunk_len) {
            buffer_push_back(conn->inbound, (chunk_len - consumed));
            ZITI_LOG(DEBUG, "client conn[%d] stalled: %zd bytes buffered", conn->conn_id,
                     buffer_available(conn->inbound));
            // client indicated that it cannot accept any more data
            // schedule retry
            uv_async_send(fl);
            return;
        }
    }
}

void conn_inbound_data_msg(ziti_connection conn, message *msg) {
    uint8_t *plain_text = NULL;
    if (conn->state == Closed || conn->fin_recv) {
        ZITI_LOG(WARN, "inbound data on closed connection");
        return;
    }

    plain_text = malloc(msg->header.body_len);
    if (conn->encrypted) {
        PREP(crypto);
        // first message is expected to be peer crypto header
        if (conn->rx != NULL) {
            ZITI_LOG(VERBOSE, "conn[%d] processing crypto header(%d bytes)", conn->conn_id, msg->header.body_len);
            TRY(crypto, msg->header.body_len != crypto_secretstream_xchacha20poly1305_HEADERBYTES);
            TRY(crypto, crypto_secretstream_xchacha20poly1305_init_pull(&conn->crypt_i, msg->body, conn->rx));
            ZITI_LOG(VERBOSE, "conn[%d] processed crypto header", conn->conn_id);
            FREE(conn->rx);
        } else {
            unsigned long long plain_len;
            unsigned char tag;
            if (msg->header.body_len > 0) {
                ZITI_LOG(VERBOSE, "conn[%d] decrypting %d bytes", conn->conn_id, msg->header.body_len);
                TRY(crypto, crypto_secretstream_xchacha20poly1305_pull(&conn->crypt_i,
                                                                       plain_text, &plain_len, &tag,
                                                                       msg->body, msg->header.body_len, NULL, 0));
                ZITI_LOG(VERBOSE, "conn[%d] decrypted %lld bytes", conn->conn_id, plain_len);
                buffer_append(conn->inbound, plain_text, plain_len);
                metrics_rate_update(&conn->ziti_ctx->down_rate, (int64_t) plain_len);
            }
        }

        CATCH(crypto) {
            FREE(plain_text);
            conn->state = Closed;
            conn->data_cb(conn, NULL, ZITI_CRYPTO_FAIL);
            return;
        }
    }
    else if (msg->header.body_len > 0) {
        memcpy(plain_text, msg->body, msg->header.body_len);
        buffer_append(conn->inbound, plain_text, msg->header.body_len);
        metrics_rate_update(&conn->ziti_ctx->down_rate, msg->header.body_len);
    }

    int32_t flags;
    if (message_get_int32_header(msg, FlagsHeader, &flags) && (flags & EDGE_FIN)) {
        conn->fin_recv = true;
    }

    flush_to_client(conn->flusher);
}

void connect_reply_cb(void *ctx, message *msg) {
    struct ziti_conn_req *req = ctx;
    struct ziti_conn *conn = req->conn;

    req->chan_tries--;

    if (req->conn_timeout != NULL) {
        uv_timer_stop(req->conn_timeout);
    }

    switch (msg->header.content) {
        case ContentTypeStateClosed:
            ZITI_LOG(ERROR, "edge conn_id[%d]: failed to %s, reason=%*.*s",
                     conn->conn_id, conn->state == Binding ? "bind" : "connect",
                     msg->header.body_len, msg->header.body_len, msg->body);
            conn->state = Closed;
            req->cb(conn, ZITI_CONN_CLOSED);
            req->failed = true;
            break;

        case ContentTypeStateConnected:
            if (conn->state == Connecting) {
                ZITI_LOG(TRACE, "edge conn_id[%d]: connected.", conn->conn_id);
                int rc = establish_crypto(conn, msg);
                if (rc == ZITI_OK && conn->encrypted) {
                    send_crypto_header(conn);
                }
                conn->state = rc == ZITI_OK ? Connected : Closed;
                req->cb(conn, rc);
            }
            else if (conn->state == Binding) {
                ZITI_LOG(TRACE, "edge conn_id[%d]: bound.", conn->conn_id);
                conn->state = Bound;
                req->cb(conn, ZITI_OK);
            }
            else if (conn->state == Accepting) {
                ZITI_LOG(TRACE, "edge conn_id[%d]: accepted.", conn->conn_id);
                if (conn->encrypted) {
                    send_crypto_header(conn);
                }
                conn->state = Connected;
                req->cb(conn, ZITI_OK);
            }
            else if (conn->state == Closed || conn->state == Timedout) {
                ZITI_LOG(WARN, "received connect reply for closed/timedout connection[%d]", conn->conn_id);
                ziti_disconnect(conn);
            }
            break;

        default:
            ZITI_LOG(WARN, "unexpected content_type[%d] conn_id[%d]", msg->header.content, conn->conn_id);
            ziti_disconnect(conn);
    }

    if (req->chan_tries == 0) {
        free_conn_req(req);
    }
}

int ziti_channel_start_connection(struct ziti_conn_req *req) {
    ziti_channel_t *ch = req->channel;

    req->conn->channel = ch;

    ZITI_LOG(TRACE, "ch[%d] => Edge Connect request token[%s] conn_id[%d]", ch->id, req->conn->token,
             req->conn->conn_id);

    uint32_t content_type;
    switch (req->conn->state) {
        case Binding:
            content_type = ContentTypeBind;
            break;
        case Connecting:
            content_type = ContentTypeConnect;
            break;
        case Closed:
            ZITI_LOG(WARN, "channel did not connect in time for connection[%d]. ", req->conn->conn_id);
            return ZITI_OK;
        default:
            ZITI_LOG(ERROR, "connection[%d] is in unexpected state[%d]", req->conn->conn_id, req->conn->state);
            return ZITI_WTF;
    }

    LIST_INSERT_HEAD(&ch->connections, req->conn, next);

    int32_t conn_id = htole32(req->conn->conn_id);
    int32_t msg_seq = htole32(0);

    hdr_t headers[] = {
            {
                    .header_id = ConnIdHeader,
                    .length = sizeof(conn_id),
                    .value = (uint8_t *) &conn_id
            },
            {
                    .header_id = SeqHeader,
                    .length = sizeof(msg_seq),
                    .value = (uint8_t *) &msg_seq
            },
            {
                    .header_id = PublicKeyHeader,
                    .length = sizeof(req->conn->pk),
                    .value = req->conn->pk,
            }
    };
    int nheaders = 2;
    // always prepare encryption on client side in case hosting side expects it
    if (req->service->encryption || content_type == ContentTypeConnect) {
        req->conn->encrypted = req->service->encryption;
        crypto_kx_keypair(req->conn->pk, req->conn->sk);
        nheaders = 3;
    }
    ziti_channel_send_for_reply(ch, content_type, headers, nheaders, req->conn->token, strlen(req->conn->token),
                                connect_reply_cb, req);

    return ZITI_OK;
}

int ziti_bind(ziti_connection conn, const char *service, ziti_listen_cb listen_cb, ziti_client_cb on_clt_cb) {
    NEWP(req, struct ziti_conn_req);

    req->service_name = strdup(service);
    req->session_type = TYPE_BIND;
    req->conn = conn;
    req->cb = listen_cb;

    conn->client_cb = on_clt_cb;
    conn->state = Binding;

    NEWP(async_cr, uv_async_t);
    uv_async_init(conn->ziti_ctx->loop, async_cr, ziti_connect_async);
    async_cr->data = req;
    return uv_async_send(async_cr);

}

int ziti_accept(ziti_connection conn, ziti_conn_cb cb, ziti_data_cb data_cb) {

    ziti_channel_t *ch = conn->parent->channel;

    conn->channel = ch;
    conn->data_cb = data_cb;

    conn->flusher = calloc(1, sizeof(uv_async_t));
    uv_async_init(conn->ziti_ctx->loop, conn->flusher, flush_to_client);
    conn->flusher->data = conn;
    uv_unref((uv_handle_t *) &conn->flusher);

    LIST_INSERT_HEAD(&ch->connections, conn, next);

    ZITI_LOG(TRACE, "ch[%d] => Edge Accept conn_id[%d] parent_conn_id[%d]", ch->id, conn->conn_id,
             conn->parent->conn_id);

    uint32_t content_type = ContentTypeDialSuccess;

    int32_t conn_id = htole32(conn->parent->conn_id);
    int32_t msg_seq = htole32(0);
    int32_t reply_id = htole32(conn->dial_req_seq);
    int32_t clt_conn_id = htole32(conn->conn_id);
    hdr_t headers[] = {
            {
                    .header_id = ConnIdHeader,
                    .length = sizeof(conn_id),
                    .value = (uint8_t *) &conn_id
            },
            {
                    .header_id = SeqHeader,
                    .length = sizeof(msg_seq),
                    .value = (uint8_t *) &msg_seq
            },
            {
                    .header_id = ReplyForHeader,
                    .length = sizeof(reply_id),
                    .value = (uint8_t *) &reply_id
            },
    };
    NEWP(req, struct ziti_conn_req);
    req->channel = conn->channel;
    req->conn = conn;
    req->cb = cb;

    ziti_channel_send_for_reply(ch, content_type, headers, 3, (const uint8_t *) &clt_conn_id, sizeof(clt_conn_id),
                                connect_reply_cb, req);

    return ZITI_OK;
}

int ziti_process_connect_reqs(ziti_context ztx) {
    ZITI_LOG(WARN, "TODO");

    return ZITI_OK;
}

static int send_fin_message(ziti_connection conn) {
    ziti_channel_t *ch = conn->channel;
    int32_t conn_id = htole32(conn->conn_id);
    int32_t msg_seq = htole32(conn->edge_msg_seq++);
    int32_t flags = htole32(EDGE_FIN);
    hdr_t headers[] = {
            {
                    .header_id = ConnIdHeader,
                    .length = sizeof(conn_id),
                    .value = (uint8_t *) &conn_id
            },
            {
                    .header_id = SeqHeader,
                    .length = sizeof(msg_seq),
                    .value = (uint8_t *) &msg_seq
            },
            {
                    .header_id = FlagsHeader,
                    .length = sizeof(flags),
                    .value = (uint8_t *) &flags
            },
    };
    NEWP(wr, struct ziti_write_req_s);
    return ziti_channel_send(ch, ContentTypeData, headers, 3, NULL, 0, wr);
}

int ziti_close_write(ziti_connection conn) {
    if (conn->fin_sent || conn->state == Closed) {
        return ZITI_OK;
    }
    conn->state = CloseWrite;
    if (conn->write_reqs == 0) {
        return send_fin_message(conn);
    }
    return ZITI_OK;
}
