// SPDX-License-Identifier: Unlicense
#define _POSIX_C_SOURCE 200809L
#include <deadbeef/deadbeef.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *base, *token, *state;
static int now_playing, batch, queue_operation, queue_run;
static DB_plugin_t *plugin;

static int integer(const char *key, int fallback)
{
    if (!strcmp(key, "listenbrainz.now_playing")) return now_playing;
    if (!strcmp(key, "listenbrainz.batch")) return batch;
    if (!strcmp(key, "listenbrainz.queue_operation")) return queue_operation;
    if (!strcmp(key, "listenbrainz.queue_run")) return queue_run;
    if (!strcmp(key, "listenbrainz.enable") || !strcmp(key, "listenbrainz.allow_http") ||
        !strcmp(key, "listenbrainz.trust_identity")) return 1;
    return fallback;
}

static const char *string(const char *key, const char *fallback)
{
    if (!strcmp(key, "listenbrainz.api_base")) return base;
    if (!strcmp(key, "listenbrainz.secret.token")) return token;
    return fallback;
}

static void noop(void) {}
static const char *directory(int id) { (void)id; return state; }
static float duration(DB_playItem_t *track) { (void)track; return 10; }

static int meta(DB_playItem_t *track, const char *key, char *value, size_t size)
{
    (void)track;
    const char *tag = !strcmp(key, "artist") ? "Björk" :
        !strcmp(key, "title") ? "Test \"title\"" :
        !strcmp(key, "musicbrainz_trackid") ? "98255a8c-017a-4bc7-8dd6-1fa36124572b" : NULL;
    return tag && snprintf(value, size, "%s", tag) > 0;
}

static void log_detail(DB_plugin_t *p, uint32_t flags, const char *format, ...)
{
    (void)p; (void)flags;
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    base = argv[1];
    token = argv[2];
    state = argv[3];
    now_playing = strchr(argv[4], 'n') != NULL;
    batch = strchr(argv[4], 'b') != NULL;
    if (strchr(argv[4], 'd')) {
        queue_operation = 4;
        queue_run = 1;
    }
    DB_functions_t api = {
        .conf_lock = noop, .conf_unlock = noop, .conf_get_int = integer,
        .conf_get_str_fast = string, .get_system_dir = directory, .log_detailed = log_detail,
        .pl_get_item_duration = duration, .pl_get_meta_with_override = meta
    };
    void *handle = dlopen("./build/ddb_listenbrainz.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    DB_plugin_t *(*load)(DB_functions_t *);
    *(void **)(&load) = dlsym(handle, "ddb_listenbrainz_load");
    if (!load) return 1;
    plugin = load(&api);
    if (plugin->get_actions) return 1;
    if (plugin->start()) return 1;
    DB_playItem_t track = {0};
    ddb_event_track_t ev = {.track = &track, .started_timestamp = 1700000000};
    char line[64];
    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == 'n') plugin->message(DB_EV_SONGSTARTED, (uintptr_t)&ev, 0, 0);
        if (line[0] == 'f' || line[0] == 's') {
            ev.playtime = line[0] == 'f' ? 5 : 4.9f;
            plugin->message(DB_EV_SONGFINISHED, (uintptr_t)&ev, 0, 0);
            ++ev.started_timestamp;
        }
        if (line[0] == 'c') {
            ddb_event_trackchange_t change = {.from = &track};
            plugin->message(DB_EV_SONGCHANGED, (uintptr_t)&change, 0, 0);
        }
        if (line[0] == 'o') queue_operation = atoi(line + 1);
        if (line[0] == 'x') queue_run = atoi(line + 1);
        if (line[0] == 'o' || line[0] == 'x')
            plugin->message(DB_EV_CONFIGCHANGED, 0, 0, 0);
        if (line[0] == 'q') break;
    }
    plugin->stop();
    dlclose(handle);
    return 0;
}
