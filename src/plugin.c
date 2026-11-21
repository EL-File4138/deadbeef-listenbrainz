// SPDX-License-Identifier: Unlicense
#define _POSIX_C_SOURCE 200809L
#include "api.h"

#include <deadbeef/deadbeef.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CFG "listenbrainz."
#define MAX_RECORDS 1000
#define MAX_AGE (30 * 24 * 60 * 60)
#define MAX_STORE (16 * 1024 * 1024)
#define MAX_BATCH 25

static DB_misc_t plugin;
static DB_functions_t *ddb;
static GAsyncQueue *events;
static GMutex event_lock;
static GThread *thread;
static gint stopping, playback_serial, queue_run;
static gboolean curl_ready;

typedef enum { CONFIG, NOW, FINISHED, CLEAR, STATUS, RETRY, RESUME, DISCARD, STOP } EventType;
typedef struct {
    EventType type;
    json_t *listen;
    gint serial;
    char *base, *key;
} Event;

typedef struct {
    LbConfig config;
    CURL *curl;
    json_t *queue, *now;
    char *path, *account;
    int lockfd;
    gboolean dirty, validate, blocked, clear_pending, now_sent, batch_failed;
    gint serial, cancel_serial;
    guint failures;
    gint64 next_request, maintenance;
} Worker;

static void
log_line(const char *message)
{
    ddb->log_detailed(DB_PLUGIN(&plugin), 0, "ListenBrainz: %s\n", message);
}

static const char *
text(json_t *object, const char *key)
{
    const char *value = json_string_value(json_object_get(object, key));
    return value ? value : "";
}

static void
free_config(LbConfig *c)
{
    g_free(c->base);
    g_free(c->token);
    g_free(c->key);
    g_free(c->proxy);
    g_free(c->proxy_user);
    g_free(c->proxy_password);
}

static LbConfig
read_config(void)
{
    LbConfig c = {0};
    ddb->conf_lock();
    c.base = lb_base(ddb->conf_get_str_fast(CFG "api_base", LB_OFFICIAL),
                     ddb->conf_get_int(CFG "allow_http", 0));
    c.token = g_strdup(ddb->conf_get_str_fast(CFG "secret.token", ""));
    c.enabled = ddb->conf_get_int(CFG "enable", 0);
    c.now_playing = ddb->conf_get_int(CFG "now_playing", 1);
    c.clear = ddb->conf_get_int(CFG "clear", 1);
    c.trust_identity = (c.base && !strcmp(c.base, LB_OFFICIAL)) ||
                       ddb->conf_get_int(CFG "trust_identity", 0);
    c.batch = ddb->conf_get_int(CFG "batch", 0);
    if (ddb->conf_get_int("network.proxy", 0)) {
        c.proxy = g_strdup(ddb->conf_get_str_fast("network.proxy.address", ""));
        c.proxy_user = g_strdup(ddb->conf_get_str_fast("network.proxy.username", ""));
        c.proxy_password = g_strdup(ddb->conf_get_str_fast("network.proxy.password", ""));
        c.proxy_port = ddb->conf_get_int("network.proxy.port", 8080);
        const char *type = ddb->conf_get_str_fast("network.proxy.type", "HTTP");
        c.proxy_type = CURLPROXY_HTTP;
        if (!g_ascii_strcasecmp(type, "HTTP_1_0")) c.proxy_type = CURLPROXY_HTTP_1_0;
        if (!g_ascii_strcasecmp(type, "SOCKS4")) c.proxy_type = CURLPROXY_SOCKS4;
        if (!g_ascii_strcasecmp(type, "SOCKS4A")) c.proxy_type = CURLPROXY_SOCKS4A;
        if (!g_ascii_strcasecmp(type, "SOCKS5")) c.proxy_type = CURLPROXY_SOCKS5;
        if (!g_ascii_strcasecmp(type, "SOCKS5_HOSTNAME")) c.proxy_type = CURLPROXY_SOCKS5_HOSTNAME;
    }
    ddb->conf_unlock();
    if (c.base && lb_valid_token(c.token)) {
        GChecksum *hash = g_checksum_new(G_CHECKSUM_SHA256);
        g_checksum_update(hash, (const guchar *)c.base, -1);
        g_checksum_update(hash, (const guchar *)"\0", 1);
        g_checksum_update(hash, (const guchar *)c.token, -1);
        c.key = g_strdup(g_checksum_get_string(hash));
        g_checksum_free(hash);
    }
    return c;
}

static gboolean
matches(const Worker *w, json_t *record)
{
    return w->config.key && !strcmp(w->config.key, text(record, "key")) &&
           !strcmp(w->config.base, text(record, "base"));
}

static void
queue_status(const Worker *w)
{
    guint pending = 0, rejected = 0, held = 0;
    size_t i;
    json_t *record;
    json_array_foreach(w->queue, i, record) {
        if (json_is_true(json_object_get(record, "rejected"))) ++rejected;
        else if (matches(w, record)) ++pending;
        else ++held;
    }
    g_autofree char *message = g_strdup_printf(
        "Queue: %u pending, %u held, %u rejected%s.", pending, held, rejected,
        w->path ? "" : " (storage unavailable)");
    log_line(message);
}

static void
open_store(Worker *w)
{
    const char *state = ddb->get_system_dir(DDB_SYS_DIR_STATE);
    g_autofree char *dir = NULL, *lock = NULL, *path = NULL;
    g_autofree char *data = NULL;
    GStatBuf st;
    size_t length = 0;
    if (!state || !*state)
        goto fail;
    dir = g_build_filename(state, "listenbrainz", NULL);
    lock = g_build_filename(dir, "queue.lock", NULL);
    path = g_build_filename(dir, "queue.json", NULL);
    if (g_mkdir_with_parents(dir, 0700) || g_lstat(dir, &st) ||
        !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077))
        goto fail;
    w->lockfd = g_open(lock, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (w->lockfd < 0 || flock(w->lockfd, LOCK_EX | LOCK_NB))
        goto fail;
    if (g_lstat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) ||
            st.st_size > MAX_STORE ||
            !g_file_get_contents(path, &data, &length, NULL))
            goto fail;
        json_t *loaded = json_loadb(data, length, JSON_REJECT_DUPLICATES, NULL);
        gboolean valid = json_is_array(loaded) && json_array_size(loaded) <= MAX_RECORDS;
        size_t i;
        json_t *record;
        json_array_foreach(loaded, i, record) {
            json_t *listen = json_object_get(record, "listen");
            json_t *meta = json_object_get(listen, "track_metadata");
            valid = valid && *text(record, "base") && *text(record, "key") &&
                    json_integer_value(json_object_get(record, "added")) > 0 &&
                    json_integer_value(json_object_get(listen, "listened_at")) > 0 &&
                    *text(meta, "artist_name") && *text(meta, "track_name");
        }
        if (!valid) {
            json_decref(loaded);
            goto fail;
        }
        json_decref(w->queue);
        w->queue = loaded;
    } else if (errno != ENOENT) {
        goto fail;
    }
    w->path = g_steal_pointer(&path);
    return;
fail:
    log_line("Cannot open the queue (unavailable, locked or invalid); existing data was not replaced.");
}

static gboolean
save_queue(Worker *w)
{
    if (!w->dirty)
        return TRUE;
    char *data = json_dumps(w->queue, JSON_COMPACT);
    gboolean saved = w->path && data && strlen(data) <= MAX_STORE &&
        g_file_set_contents_full(w->path, data, -1,
            G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, NULL);
    free(data);
    if (saved) w->dirty = FALSE;
    return saved;
}

static void
prune(Worker *w)
{
    gint64 cutoff = time(NULL) - MAX_AGE;
    guint removed = 0;
    for (size_t i = json_array_size(w->queue); i > 0; --i) {
        if (json_integer_value(json_object_get(json_array_get(w->queue, i - 1), "added")) <= cutoff) {
            json_array_remove(w->queue, i - 1);
            ++removed;
        }
    }
    if (removed) {
        w->dirty = TRUE;
        g_autofree char *message = g_strdup_printf("Expired %u queued listens.", removed);
        log_line(message);
        queue_status(w);
    }
}

static void
enqueue_listen(Worker *w, const Event *ev)
{
    if (!ev->key)
        return;
    if (!w->path) {
        log_line("Listen not queued: storage is unavailable.");
        return;
    }
    const char *account = w->account && !g_strcmp0(w->config.key, ev->key) ? w->account : "";
    json_t *record = json_pack("{s:s,s:s,s:s,s:I,s:O}",
        "base", ev->base, "key", ev->key, "account", account,
        "added", (json_int_t)time(NULL), "listen", ev->listen);
    if (!record || json_array_append_new(w->queue, record)) {
        log_line("Could not enqueue listen.");
        return;
    }
    w->dirty = TRUE;
    if (json_array_size(w->queue) > MAX_RECORDS) {
        json_array_remove(w->queue, 0);
        log_line("Queue full; evicted its oldest listen.");
        queue_status(w);
    }
}

static int
cancel_request(void *opaque, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
    (void)a; (void)b; (void)c; (void)d;
    Worker *w = opaque;
    return g_atomic_int_get(&stopping) ||
           (w->cancel_serial && w->cancel_serial != g_atomic_int_get(&playback_serial));
}

static LbReply
request(Worker *w, const char *endpoint, json_t *body)
{
    LbReply reply = lb_request(w->curl, &w->config, endpoint, body, cancel_request, w);
    w->next_request = g_get_monotonic_time() +
        (gint64)MAX(1u, reply.wait_seconds) * G_USEC_PER_SEC;
    if (reply.transport == CURLE_OK && (reply.status == 401 || reply.status == 403)) {
        w->blocked = TRUE;
        log_line("Server rejected authorization; submissions paused.");
    }
    return reply;
}

static void
backoff(Worker *w)
{
    w->failures = MIN(w->failures + 1, 9u);
    w->next_request = MAX(w->next_request, g_get_monotonic_time() +
        (gint64)MIN(1u << w->failures, 300u) * G_USEC_PER_SEC);
}

static gboolean
transient(const LbReply *r)
{
    return r->transport != CURLE_OK || r->status == 408 || r->status == 429 || r->status >= 500;
}

static void
validate(Worker *w)
{
    LbReply r = request(w, "/validate-token", NULL);
    w->validate = FALSE;
    if (r.transport == CURLE_OK && r.status == 200) {
        json_t *valid = json_object_get(r.body, "valid");
        if (json_is_false(valid)) {
            w->blocked = TRUE;
            log_line("Token is invalid; submissions paused.");
        } else if (json_is_true(valid)) {
            const char *account = text(r.body, "user_name");
            if (w->config.trust_identity && *account) {
                g_free(w->account);
                w->account = g_strdup(account);
                size_t i;
                json_t *record;
                json_array_foreach(w->queue, i, record) {
                    if (matches(w, record) && !*text(record, "account")) {
                        json_object_set_new(record, "account", json_string(account));
                        w->dirty = TRUE;
                    }
                }
            }
            log_line("Server accepted the token.");
        } else {
            log_line("Server did not provide a token validation result.");
        }
    } else if (r.status == 429) {
        w->validate = TRUE;
        backoff(w);
    } else if (!w->blocked && !g_atomic_int_get(&stopping)) {
        log_line("Could not validate token; submission will be tried separately.");
    }
    json_decref(r.body);
}

static void
submit_now(Worker *w, gboolean clear)
{
    if (w->serial != g_atomic_int_get(&playback_serial))
        goto done;
    w->cancel_serial = w->serial;
    json_t *body = clear ? json_pack("{s:s}", "client", LB_CLIENT) :
        json_pack("{s:s,s:[O]}", "listen_type", "playing_now", "payload", w->now);
    if (body) {
        /* A canceled upload may already have reached the server. */
        if (!clear) w->now_sent = TRUE;
        LbReply r = request(w, clear ? "/playing-now/delete" : "/submit-listens", body);
        if (clear && (lb_accepted(&r) ||
            (r.transport == CURLE_OK && (r.status == 404 || r.status == 405 || r.status == 501))))
            w->now_sent = FALSE;
        json_decref(r.body);
        json_decref(body);
    }
    w->cancel_serial = 0;
done:
    json_decref(w->now);
    w->now = NULL;
    w->clear_pending = FALSE;
}

static size_t
pending_items(const Worker *w, size_t indexes[MAX_BATCH])
{
    size_t n = 0, i;
    size_t limit = w->config.batch && !w->batch_failed ? MAX_BATCH : 1;
    json_t *record;
    json_array_foreach(w->queue, i, record) {
        if (matches(w, record) && !json_is_true(json_object_get(record, "rejected"))) {
            indexes[n++] = i;
            if (n == limit) break;
        }
    }
    return n;
}

static void
submit_pending(Worker *w, const size_t *indexes, size_t n)
{
    json_t *listens = json_array();
    for (size_t i = 0; listens && i < n; ++i) {
        if (json_array_append(listens, json_object_get(json_array_get(w->queue, indexes[i]), "listen"))) {
            json_decref(listens);
            listens = NULL;
        }
    }
    json_t *body = listens ? json_pack("{s:s,s:o}", "listen_type", n > 1 ? "import" : "single",
                                       "payload", listens) : NULL;
    if (!body) {
        backoff(w);
        return;
    }
    LbReply r = request(w, "/submit-listens", body);
    if (lb_accepted(&r)) {
        while (n) json_array_remove(w->queue, indexes[--n]);
        w->dirty = TRUE;
        w->failures = 0;
    } else if (w->blocked || g_atomic_int_get(&stopping)) {
        /* Keep every unacknowledged listen. */
    } else if (transient(&r)) {
        if (!w->failures) log_line("Delivery failed; queued listens will be retried.");
        backoff(w);
    } else if (r.status == 400 || r.status == 409 || r.status == 413 || r.status == 422) {
        if (n > 1) {
            w->batch_failed = TRUE;
            log_line("Batch rejected; falling back to individual submissions.");
        } else {
            json_object_set_new(json_array_get(w->queue, indexes[0]), "rejected", json_true());
            w->dirty = TRUE;
            log_line("A listen was rejected; other queued listens can still be sent.");
            queue_status(w);
        }
    } else {
        w->blocked = TRUE;
        log_line("Unexpected submission response; queue held. Check the API base and retry.");
    }
    json_decref(r.body);
    json_decref(body);
}

static void
reload(Worker *w)
{
    LbConfig next = read_config();
    gboolean changed = g_strcmp0(next.key, w->config.key) ||
        next.enabled != w->config.enabled || next.trust_identity != w->config.trust_identity ||
        g_strcmp0(next.proxy, w->config.proxy) || g_strcmp0(next.proxy_user, w->config.proxy_user) ||
        g_strcmp0(next.proxy_password, w->config.proxy_password) ||
        next.proxy_port != w->config.proxy_port || next.proxy_type != w->config.proxy_type;
    if (next.batch != w->config.batch) w->batch_failed = FALSE;
    if (changed) {
        g_clear_pointer(&w->account, g_free);
        w->now_sent = w->blocked = w->batch_failed = FALSE;
        w->validate = next.enabled && next.key;
        w->failures = 0;
        w->clear_pending = FALSE;
        json_decref(w->now);
        w->now = NULL;
    }
    free_config(&w->config);
    w->config = next;
    if (next.enabled && !next.key)
        log_line("Set a valid API base and token before submitting.");
}

static void
resume_held(Worker *w)
{
    if (!w->config.enabled || w->blocked || !w->account || !w->config.key) {
        log_line("Cannot resume held listens without a verified account.");
        return;
    }
    size_t i;
    json_t *record;
    json_array_foreach(w->queue, i, record) {
        if (!matches(w, record) && !strcmp(w->config.base, text(record, "base")) &&
            !strcmp(w->account, text(record, "account"))) {
            json_object_set_new(record, "key", json_string(w->config.key));
            w->dirty = TRUE;
        }
    }
    queue_status(w);
}

static void
handle(Worker *w, Event *ev)
{
    if (ev->type == CONFIG || ev->type == STATUS || ev->type == RETRY ||
        ev->type == RESUME || ev->type == DISCARD)
        reload(w);
    switch (ev->type) {
    case CONFIG: break;
    case NOW:
    case CLEAR:
        json_decref(w->now);
        w->serial = ev->serial;
        w->now = ev->type == NOW && ev->key && !g_strcmp0(w->config.key, ev->key) &&
                 w->config.now_playing ? json_incref(ev->listen) : NULL;
        w->clear_pending = ev->type == CLEAR && w->config.clear && w->now_sent;
        break;
    case FINISHED: enqueue_listen(w, ev); break;
    case STATUS: queue_status(w); break;
    case RETRY:
        w->blocked = w->batch_failed = FALSE;
        w->validate = w->config.enabled && w->config.key;
        g_clear_pointer(&w->account, g_free);
        w->failures = 0;
        break;
    case RESUME:
        if (ev->key && !g_strcmp0(ev->key, w->config.key)) resume_held(w);
        break;
    case DISCARD:
        if (g_strcmp0(ev->key, w->config.key)) break;
        for (size_t i = json_array_size(w->queue); i > 0; --i) {
            json_t *record = json_array_get(w->queue, i - 1);
            if (!matches(w, record) || json_is_true(json_object_get(record, "rejected"))) {
                json_array_remove(w->queue, i - 1);
                w->dirty = TRUE;
            }
        }
        queue_status(w);
        break;
    case STOP: break;
    }
}

static gpointer
work(gpointer unused)
{
    (void)unused;
    Worker w = {.queue = json_array(), .curl = curl_easy_init(), .lockfd = -1};
    open_store(&w);
    reload(&w);
    gboolean done = FALSE, storage_failed = FALSE;
    while (!done) {
        Event *ev;
        while ((ev = g_async_queue_try_pop(events))) {
            done = ev->type == STOP;
            handle(&w, ev);
            json_decref(ev->listen);
            g_free(ev->base);
            g_free(ev->key);
            g_free(ev);
            if (done) break;
        }
        gint64 now = g_get_monotonic_time();
        if (now >= w.maintenance) {
            prune(&w);
            w.maintenance = now + 60 * G_USEC_PER_SEC;
        }
        gboolean saved = save_queue(&w);
        if (!saved && !storage_failed)
            log_line("Could not save the queue; delivery paused until storage is writable.");
        storage_failed = !saved;
        if (done) break;
        gint64 wake = MIN(w.maintenance, now + (saved ? 60 : 5) * G_USEC_PER_SEC);
        if (!g_atomic_int_get(&stopping) && w.curl && w.config.enabled && w.config.key && !w.blocked) {
            size_t indexes[MAX_BATCH];
            size_t count = saved ? pending_items(&w, indexes) : 0;
            if (w.validate || w.now || w.clear_pending || count) {
                if (now >= w.next_request) {
                    if (w.validate) validate(&w);
                    else if (w.now || w.clear_pending) submit_now(&w, w.clear_pending);
                    else submit_pending(&w, indexes, count);
                    continue;
                }
                wake = MIN(wake, w.next_request);
            }
        }
        /* A wake-up must not reorder the event relative to later playback events. */
        ev = g_async_queue_timeout_pop(events, MAX((gint64)1, wake - g_get_monotonic_time()));
        if (ev) g_async_queue_push_front(events, ev);
    }
    if (w.lockfd >= 0) close(w.lockfd);
    free_config(&w.config);
    g_free(w.account);
    g_free(w.path);
    json_decref(w.queue);
    json_decref(w.now);
    if (w.curl) curl_easy_cleanup(w.curl);
    return NULL;
}

static void
send_event(EventType type, json_t *listen, gint serial)
{
    Event *ev = g_new0(Event, 1);
    *ev = (Event){.type = type, .listen = listen, .serial = serial};
    if (type == FINISHED || type == NOW || type == RESUME || type == DISCARD) {
        LbConfig c = read_config();
        if (c.enabled || type == DISCARD) {
            ev->key = g_steal_pointer(&c.key);
            ev->base = g_steal_pointer(&c.base);
        }
        free_config(&c);
    }
    g_mutex_lock(&event_lock);
    if (events && (!g_atomic_int_get(&stopping) || type == STOP))
        g_async_queue_push(events, ev);
    else {
        json_decref(ev->listen);
        g_free(ev->base);
        g_free(ev->key);
        g_free(ev);
    }
    g_mutex_unlock(&event_lock);
}

static void
add_tag(json_t *target, const char *field, DB_playItem_t *track, const char *tag, gboolean uuid)
{
    char buffer[512] = {0};
    if (ddb->pl_get_meta_with_override(track, tag, buffer, sizeof(buffer)) &&
        *buffer && g_utf8_validate(buffer, -1, NULL) && (!uuid || g_uuid_string_is_valid(buffer)))
        json_object_set_new(target, field, json_string(buffer));
}

static json_t *
snapshot(DB_playItem_t *track, time_t timestamp, float duration, float played)
{
    json_t *listen = json_pack("{s:{s:{s:s,s:s,s:s}}}", "track_metadata",
        "additional_info", "media_player", "DeaDBeeF", "submission_client", LB_CLIENT,
        "submission_client_version", "0.1");
    if (!listen) return NULL;
    json_t *meta = json_object_get(listen, "track_metadata");
    json_t *info = json_object_get(meta, "additional_info");
    add_tag(meta, "artist_name", track, "artist", FALSE);
    add_tag(meta, "track_name", track, "title", FALSE);
    if (!*text(meta, "artist_name") || !*text(meta, "track_name")) {
        json_decref(listen);
        return NULL;
    }
    add_tag(meta, "release_name", track, "album", FALSE);
    add_tag(info, "tracknumber", track, "track", FALSE);
    add_tag(info, "recording_mbid", track, "musicbrainz_trackid", TRUE);
    add_tag(info, "recording_mbid", track, "musicbrainz_recordingid", TRUE);
    add_tag(info, "track_mbid", track, "musicbrainz_releasetrackid", TRUE);
    add_tag(info, "release_mbid", track, "musicbrainz_albumid", TRUE);
    add_tag(info, "release_group_mbid", track, "musicbrainz_releasegroupid", TRUE);
    char artist[512] = {0};
    if (ddb->pl_get_meta_with_override(track, "musicbrainz_artistid", artist, sizeof(artist))) {
        g_auto(GStrv) ids = g_strsplit_set(artist, ";,/ ", -1);
        json_t *mbids = json_array();
        for (char **id = ids; *id; ++id)
            if (g_uuid_string_is_valid(*id)) json_array_append_new(mbids, json_string(*id));
        if (json_array_size(mbids)) json_object_set_new(info, "artist_mbids", mbids);
        else json_decref(mbids);
    }
    if (duration >= 0.001f && duration <= 2073600)
        json_object_set_new(info, "duration_ms", json_integer((json_int_t)(duration * 1000.0)));
    if (timestamp > 0) {
        json_object_set_new(listen, "listened_at", json_integer((json_int_t)timestamp));
        if (played >= 1 && played <= 2073600)
            json_object_set_new(info, "duration_played", json_integer((json_int_t)played));
    }
    return listen;
}

static int
message(uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2)
{
    (void)p1; (void)p2;
    if (g_atomic_int_get(&stopping)) return 0;
    if (id == DB_EV_CONFIGCHANGED) {
        static const EventType operations[] = {CONFIG, STATUS, RETRY, RESUME, DISCARD};
        ddb->conf_lock();
        int choice = ddb->conf_get_int(CFG "queue_operation", 0);
        gboolean run = ddb->conf_get_int(CFG "queue_run", 0) != 0;
        ddb->conf_unlock();
        gboolean changed = g_atomic_int_compare_and_exchange(&queue_run, !run, run);
        send_event(changed && run && choice > 0 && choice < (int)G_N_ELEMENTS(operations)
                   ? operations[choice] : CONFIG, NULL, 0);
    }
    if (id == DB_EV_SONGSTARTED && ctx) {
        ddb_event_track_t *ev = (void *)ctx;
        gint serial = g_atomic_int_add(&playback_serial, 1) + 1;
        json_t *listen = NULL;
        if (ev->track && ddb->conf_get_int(CFG "enable", 0) && ddb->conf_get_int(CFG "now_playing", 1))
            listen = snapshot(ev->track, 0, ddb->pl_get_item_duration(ev->track), 0);
        send_event(NOW, listen, serial);
    }
    if (id == DB_EV_SONGCHANGED && ctx && !((ddb_event_trackchange_t *)ctx)->to)
        send_event(CLEAR, NULL, g_atomic_int_add(&playback_serial, 1) + 1);
    if (id == DB_EV_SONGFINISHED && ctx && ddb->conf_get_int(CFG "enable", 0)) {
        ddb_event_track_t *ev = (void *)ctx;
        float duration = ev->track ? ddb->pl_get_item_duration(ev->track) : 0;
        if (isfinite(duration) && duration > 0 && isfinite(ev->playtime) &&
            ev->playtime >= MIN(duration / 2, 240.0f) && ev->started_timestamp >= 1033430400) {
            json_t *listen = snapshot(ev->track, ev->started_timestamp, duration, ev->playtime);
            if (listen) send_event(FINISHED, listen, 0);
        }
    }
    return 0;
}

static int
start(void)
{
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return -1;
    curl_ready = TRUE;
    g_mutex_lock(&event_lock);
    g_atomic_int_set(&stopping, 0);
    g_atomic_int_set(&queue_run, ddb->conf_get_int(CFG "queue_run", 0) != 0);
    events = g_async_queue_new();
    thread = g_thread_try_new("listenbrainz", work, NULL, NULL);
    if (!thread) {
        g_clear_pointer(&events, g_async_queue_unref);
        curl_global_cleanup();
        curl_ready = FALSE;
        g_mutex_unlock(&event_lock);
        return -1;
    }
    g_mutex_unlock(&event_lock);
    return 0;
}

static int
stop(void)
{
    if (thread) {
        g_atomic_int_set(&stopping, 1);
        send_event(STOP, NULL, 0);
        g_thread_join(thread);
        thread = NULL;
        g_mutex_lock(&event_lock);
        g_clear_pointer(&events, g_async_queue_unref);
        g_mutex_unlock(&event_lock);
    }
    if (curl_ready) {
        curl_global_cleanup();
        curl_ready = FALSE;
    }
    return 0;
}

static DB_misc_t plugin = {
    DDB_PLUGIN_SET_API_VERSION
    .plugin.version_major = 0,
    .plugin.version_minor = 1,
    .plugin.type = DB_PLUGIN_MISC,
    .plugin.id = "ddb_listenbrainz",
    .plugin.name = "ListenBrainz",
    .plugin.descr = "Submit listens to a ListenBrainz-compatible server.",
    .plugin.start = start,
    .plugin.stop = stop,
    .plugin.message = message,
    .plugin.configdialog =
        "property \"Enable scrobbling\" checkbox " CFG "enable 0;"
        "property \"ListenBrainz API base\" entry " CFG "api_base \"" LB_OFFICIAL "\";"
        "property \"User token\" password " CFG "secret.token \"\";"
        "property \"Allow unencrypted HTTP\" checkbox " CFG "allow_http 0;"
        "property \"Send now playing\" checkbox " CFG "now_playing 1;"
        "property \"Clear now playing on stop\" checkbox " CFG "clear 1;"
        "property \"Trust this server's account identity for token rotation\" checkbox " CFG "trust_identity 0;"
        "property \"Batch backlog replay (verified servers only)\" checkbox " CFG "batch 0;"
        "property \"Queue operation\" select[5] " CFG "queue_operation 0 "
            "\"None\" \"Log queue counts\" \"Recheck token and retry delivery\" "
            "\"Resume backlog for the verified account\" \"Discard all held and rejected listens\";"
        "property \"Run queue operation (uncheck to run again)\" checkbox " CFG "queue_run 0;"
};

__attribute__((visibility("default"))) DB_plugin_t *
ddb_listenbrainz_load(DB_functions_t *api)
{
    ddb = api;
    return DB_PLUGIN(&plugin);
}
