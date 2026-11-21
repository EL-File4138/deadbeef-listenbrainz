// SPDX-License-Identifier: Unlicense
#include "api.h"

#include <errno.h>
#include <string.h>
#include <time.h>

char *
lb_base(const char *input, gboolean allow_http)
{
    CURLU *url = curl_url();
    if (!url)
        return NULL;
    char *scheme = NULL, *path = NULL, *value = NULL, *canonical = NULL;
    char *result = NULL;
    if (!input || !*input || strpbrk(input, "?#\\\r\n\t") ||
        curl_url_set(url, CURLUPART_URL, input, CURLU_DISALLOW_USER) ||
        curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) ||
        (strcmp(scheme, "https") && (!allow_http || strcmp(scheme, "http"))) ||
        curl_url_get(url, CURLUPART_PATH, &path, 0))
        goto done;
    size_t n = strlen(path);
    while (n && path[n - 1] == '/')
        path[--n] = '\0';
    value = g_str_has_suffix(path, "/1") ? g_strdup(path) : g_strconcat(path, "/1", NULL);
    if (!curl_url_set(url, CURLUPART_PATH, value, 0) &&
        !curl_url_get(url, CURLUPART_URL, &canonical, CURLU_NO_DEFAULT_PORT))
        result = g_strdup(canonical);
done:
    g_free(value);
    curl_free(canonical);
    curl_free(scheme);
    curl_free(path);
    curl_url_cleanup(url);
    return result;
}

gboolean
lb_valid_token(const char *token)
{
    if (!token || !*token || strlen(token) > 1024)
        return FALSE;
    for (const unsigned char *p = (const unsigned char *)token; *p; ++p)
        if (*p <= ' ' || *p >= 127)
            return FALSE;
    return TRUE;
}

typedef struct {
    GString *body;
    gboolean exhausted;
    guint retry_after, reset_in, reset;
} Response;

static size_t
receive_body(char *data, size_t size, size_t nmemb, void *opaque)
{
    Response *r = opaque;
    size_t bytes = size * nmemb;
    if (bytes > 65536 - r->body->len)
        return 0;
    g_string_append_len(r->body, data, bytes);
    return bytes;
}

static guint
delay_until(time_t date)
{
    time_t now = time(NULL);
    return date > now ? (guint)MIN(difftime(date, now), G_MAXUINT) : 0;
}

static size_t
receive_header(char *data, size_t size, size_t nmemb, void *opaque)
{
    Response *r = opaque;
    size_t bytes = size * nmemb;
    if (bytes >= 5 && !memcmp(data, "HTTP/", 5)) {
        r->exhausted = FALSE;
        r->retry_after = r->reset_in = r->reset = 0;
        g_string_truncate(r->body, 0);
        return bytes;
    }
    const char *colon = memchr(data, ':', bytes);
    if (!colon || bytes > 1024)
        return bytes;
    g_autofree char *name = g_strndup(data, colon - data);
    g_autofree char *storage = g_strndup(colon + 1, bytes - (colon + 1 - data));
    char *value = g_strstrip(storage);
    if (g_ascii_strcasecmp(name, "Retry-After") &&
        g_ascii_strcasecmp(name, "X-RateLimit-Remaining") &&
        g_ascii_strcasecmp(name, "X-RateLimit-Reset-In") &&
        g_ascii_strcasecmp(name, "X-RateLimit-Reset"))
        return bytes;
    char *end;
    errno = 0;
    guint64 n = g_ascii_strtoull(value, &end, 10);
    gboolean numeric = g_ascii_isdigit(*value) && !*end && !errno;
    if (!g_ascii_strcasecmp(name, "Retry-After"))
        r->retry_after = numeric ? MIN(n, G_MAXUINT) : delay_until(curl_getdate(value, NULL));
    else if (numeric && !g_ascii_strcasecmp(name, "X-RateLimit-Remaining"))
        r->exhausted = n == 0;
    else if (numeric && !g_ascii_strcasecmp(name, "X-RateLimit-Reset-In"))
        r->reset_in = MIN(n, G_MAXUINT);
    else if (numeric && !g_ascii_strcasecmp(name, "X-RateLimit-Reset") && n <= G_MAXINT64)
        r->reset = delay_until((time_t)n);
    return bytes;
}

LbReply
lb_request(CURL *curl, const LbConfig *config, const char *endpoint,
           json_t *payload, curl_xferinfo_callback cancel, void *state)
{
    LbReply result = {.transport = CURLE_OUT_OF_MEMORY};
    g_autofree char *url = g_strconcat(config->base, endpoint, NULL);
    g_autofree char *auth = g_strconcat("Authorization: Token ", config->token, NULL);
    char *data = payload ? json_dumps(payload, JSON_COMPACT) : NULL;
    struct curl_slist *headers = NULL;
    Response response = {.body = g_string_new(NULL)};
    const char *values[] = {auth, "Accept: application/json", "Content-Type: application/json"};
    if (payload && !data)
        goto done;
    for (size_t i = 0; i < G_N_ELEMENTS(values); ++i) {
        struct curl_slist *next = curl_slist_append(headers, values[i]);
        if (!next)
            goto done;
        headers = next;
    }
    curl_easy_reset(curl);
#define SET(option, value) do { \
    result.transport = curl_easy_setopt(curl, option, value); \
    if (result.transport != CURLE_OK) goto done; \
} while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_HTTPHEADER, headers);
    SET(CURLOPT_PROTOCOLS_STR, "https,http");
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    SET(CURLOPT_TIMEOUT_MS, 10000L);
    SET(CURLOPT_USERAGENT, "ddb-listenbrainz/0.1");
    SET(CURLOPT_WRITEFUNCTION, receive_body);
    SET(CURLOPT_WRITEDATA, &response);
    SET(CURLOPT_HEADERFUNCTION, receive_header);
    SET(CURLOPT_HEADERDATA, &response);
    SET(CURLOPT_NOPROGRESS, 0L);
    SET(CURLOPT_XFERINFOFUNCTION, cancel);
    SET(CURLOPT_XFERINFODATA, state);
    if (config->proxy && *config->proxy) {
        SET(CURLOPT_PROXY, config->proxy);
        SET(CURLOPT_PROXYPORT, config->proxy_port);
        SET(CURLOPT_PROXYTYPE, config->proxy_type);
        SET(CURLOPT_PROXYUSERNAME, config->proxy_user);
        SET(CURLOPT_PROXYPASSWORD, config->proxy_password);
    }
    if (payload) {
        SET(CURLOPT_POSTFIELDS, data);
        SET(CURLOPT_POSTFIELDSIZE, (long)strlen(data));
    }
#undef SET
    result.transport = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    result.wait_seconds = MAX(response.retry_after,
        response.exhausted || result.status == 429
            ? (response.reset_in ? response.reset_in : response.reset) : 0);
    if (result.transport == CURLE_OK)
        result.body = json_loadb(response.body->str, response.body->len, JSON_REJECT_DUPLICATES, NULL);
done:
    curl_slist_free_all(headers);
    g_string_free(response.body, TRUE);
    free(data);
    return result;
}

gboolean
lb_accepted(const LbReply *reply)
{
    const char *status = json_string_value(json_object_get(reply->body, "status"));
    return reply->transport == CURLE_OK && reply->status >= 200 && reply->status < 300 &&
           status && !strcmp(status, "ok");
}
