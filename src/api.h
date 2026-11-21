// SPDX-License-Identifier: Unlicense
#ifndef DDB_LISTENBRAINZ_API_H
#define DDB_LISTENBRAINZ_API_H

#include <curl/curl.h>
#include <glib.h>
#include <jansson.h>

#define LB_OFFICIAL "https://api.listenbrainz.org/1"
#define LB_CLIENT "DeaDBeeF ListenBrainz Plugin"

typedef struct {
    char *base, *token, *key;
    char *proxy, *proxy_user, *proxy_password;
    long proxy_port, proxy_type;
    gboolean enabled, now_playing, clear, trust_identity, batch;
} LbConfig;

typedef struct {
    CURLcode transport;
    long status;
    guint wait_seconds;
    json_t *body;
} LbReply;

char *lb_base(const char *input, gboolean allow_http);
gboolean lb_valid_token(const char *token);
LbReply lb_request(CURL *curl, const LbConfig *config, const char *endpoint,
                   json_t *payload, curl_xferinfo_callback cancel, void *state);
gboolean lb_accepted(const LbReply *reply);

#endif
