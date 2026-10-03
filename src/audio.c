#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "media_internal.h"
#include <errno.h>
#include <math.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <pthread.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/json.h>
#include <stdlib.h>
#include <string.h>
#define AUDIO_RATE 48000
#define AUDIO_RING 96000
#define AUDIO_NODES 256
struct AudioNode {
    uint32_t id;
    char serial[32], name[256], description[256], kind[64];
};
struct CastAudio;
struct AudioLane {
    struct CastAudio *owner;
    struct pw_stream *stream;
    struct spa_hook listener;
    uint32_t node;
    bool dead;
    int rate, channels;
    uint64_t begin, end;
    float *samples;
    char error[CAST_ERR];
};
struct CastAudio {
    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct pw_metadata *metadata;
    struct spa_hook core_listener, registry_listener, metadata_listener;
    struct AudioNode nodes[AUDIO_NODES];
    int count, seq;
    uint64_t accept_after, virtual_accept_after, record_accept_after, stream_accept_after;
    uint64_t virtual_epoch_after, record_epoch_after, stream_epoch_after;
    bool done, server_dead, started, virtual_silent;
    char default_mic[256], error[CAST_ERR];
    Config cfg;
    struct AudioLane lane[2], virtual_lane;
};
static pthread_once_t pw_once = PTHREAD_ONCE_INIT;
static void initialize_pw(void)
{
    pw_init(NULL, NULL);
}
static uint64_t sample_time(uint64_t ns)
{
    return (ns / 1000000000) * AUDIO_RATE + (ns % 1000000000) * AUDIO_RATE / 1000000000;
}
static const char *prop(const struct spa_dict *d, const char *key)
{
    const char *v = d ? spa_dict_lookup(d, key) : NULL;
    return v ? v : "";
}
static void clear_lane(struct AudioLane *l)
{
    l->begin = l->end = 0;
    if (l->samples) {
        memset(l->samples, 0, AUDIO_RING * 2 * sizeof(float));
    }
}
static int metadata_property(void *data, uint32_t subject, const char *key, const char *type,
                             const char *value)
{
    (void)subject;
    (void)type;
    CastAudio *a = data;
    if (!key || strcmp(key, "default.audio.source")) {
        return 0;
    }
    a->default_mic[0] = 0;
    if (!value) {
        return 0;
    }
    struct spa_json it, obj;
    spa_json_init(&it, value, strlen(value));
    if (spa_json_enter_object(&it, &obj) > 0) {
        char k[128], v[256];
        while (spa_json_get_string(&obj, k, sizeof(k)) > 0) {
            if (spa_json_get_string(&obj, v, sizeof(v)) <= 0) {
                break;
            }
            if (!strcmp(k, "name")) {
                snprintf(a->default_mic, sizeof(a->default_mic), "%s", v);
            }
        }
    }
    return 0;
}
static const struct pw_metadata_events metadata_events = {.version = PW_VERSION_METADATA_EVENTS,
                                                          .property = metadata_property};
static void registry_global(void *data, uint32_t id, uint32_t permissions, const char *type,
                            uint32_t version, const struct spa_dict *props)
{
    (void)permissions;
    (void)version;
    CastAudio *a = data;
    if (!strcmp(type, PW_TYPE_INTERFACE_Metadata) &&
        !strcmp(prop(props, "metadata.name"), "default") && !a->metadata) {
        a->metadata =
            pw_registry_bind(a->registry, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0);
        if (a->metadata) {
            pw_metadata_add_listener(a->metadata, &a->metadata_listener, &metadata_events, a);
        }
        return;
    }
    if (strcmp(type, PW_TYPE_INTERFACE_Node) || !props || a->count >= AUDIO_NODES) {
        return;
    }
    const char *kind = prop(props, PW_KEY_MEDIA_CLASS);
    if (strncmp(kind, "Audio/Source", 12) && strcmp(kind, "Audio/Sink") &&
        strcmp(kind, "Stream/Output/Audio")) {
        return;
    }
    struct AudioNode *v = &a->nodes[a->count++];
    v->id = id;
    snprintf(v->serial, sizeof(v->serial), "%s", prop(props, PW_KEY_OBJECT_SERIAL));
    snprintf(v->name, sizeof(v->name), "%s", prop(props, PW_KEY_NODE_NAME));
    snprintf(v->kind, sizeof(v->kind), "%s", kind);
    const char *description = prop(props, PW_KEY_NODE_DESCRIPTION);
    if (!*description) {
        description = prop(props, PW_KEY_APP_NAME);
    }
    snprintf(v->description, sizeof(v->description), "%s", description);
}
static void registry_remove(void *data, uint32_t id)
{
    CastAudio *a = data;
    for (int i = 0; i < a->count; i++) {
        if (a->nodes[i].id == id) {
            a->nodes[i] = a->nodes[--a->count];
            break;
        }
    }
    for (int i = 0; i < 2; i++) {
        if (a->lane[i].stream && a->lane[i].node == id) {
            a->lane[i].dead = true;
            clear_lane(&a->lane[i]);
            snprintf(a->lane[i].error, sizeof(a->lane[i].error),
                     "selected %s audio source disappeared; select a new source with cast audio %s "
                     "source NAME (no fallback)",
                     i ? "desktop" : "mic", i ? "desktop" : "mic");
        }
    }
}
static const struct pw_registry_events registry_events = {.version = PW_VERSION_REGISTRY_EVENTS,
                                                          .global = registry_global,
                                                          .global_remove = registry_remove};
static void core_done(void *data, uint32_t id, int seq)
{
    CastAudio *a = data;
    if (id == PW_ID_CORE && seq == a->seq) {
        a->done = true;
        pw_thread_loop_signal(a->loop, false);
    }
}
static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    (void)seq;
    CastAudio *a = data;
    snprintf(a->error, sizeof(a->error), "PipeWire: %s (%s)",
             message ? message : "connection error", strerror(-res));
    if (id == PW_ID_CORE) {
        a->server_dead = true;
        for (int i = 0; i < 2; i++) {
            a->lane[i].dead = true;
            clear_lane(&a->lane[i]);
        }
    }
    pw_thread_loop_signal(a->loop, false);
}
static const struct pw_core_events core_events = {
    .version = PW_VERSION_CORE_EVENTS, .done = core_done, .error = core_error};
static int roundtrip(CastAudio *a)
{
    a->done = false;
    a->seq = pw_core_sync(a->core, PW_ID_CORE, a->seq);
    struct timespec until;
    pw_thread_loop_get_time(a->loop, &until, 2000000000);
    while (!a->done && !a->server_dead) {
        if (pw_thread_loop_timed_wait_full(a->loop, &until) < 0) {
            return -1;
        }
    }
    return a->server_dead ? -1 : 0;
}
static void lane_state(void *data, enum pw_stream_state old, enum pw_stream_state state,
                       const char *error)
{
    (void)old;
    struct AudioLane *l = data;
    if (state == PW_STREAM_STATE_ERROR) {
        l->dead = true;
        clear_lane(l);
        snprintf(l->error, sizeof(l->error), "PipeWire stream: %s", error ? error : "failed");
    }
    pw_thread_loop_signal(l->owner->loop, false);
}
static void lane_format(void *data, uint32_t id, const struct spa_pod *param)
{
    struct AudioLane *l = data;
    if (id != SPA_PARAM_Format || !param) {
        return;
    }
    struct spa_audio_info_raw f = {0};
    if (spa_format_audio_raw_parse(param, &f) < 0 || f.format != SPA_AUDIO_FORMAT_F32 ||
        f.rate != AUDIO_RATE || f.channels != 2) {
        l->dead = true;
        snprintf(l->error, sizeof(l->error),
                 "audio stream renegotiated an unsupported format; select source again");
        clear_lane(l);
        return;
    }
    l->rate = (int)f.rate;
    l->channels = (int)f.channels;
}
static void put_samples(struct AudioLane *l, uint64_t ns, const float *src, int count)
{
    uint64_t start = sample_time(ns);
    uint64_t captured_at = start;
    if (start < l->owner->accept_after) {
        return;
    }
    if (count > AUDIO_RING) {
        int skip = count - AUDIO_RING;
        src += (size_t)skip * 2;
        start += (uint64_t)skip;
        count = AUDIO_RING;
    }
    if (l->end && start > l->end && start - l->end < 960) {
        start = l->end;
    }
    if (l->end && start < l->end && l->end - start < 960) {
        start = l->end;
    }
    /* A late callback can deliver a chunk captured before one lane's boundary.
     * Retire that entire chunk for this lane, while retaining the other mix. */
    uint64_t end = start + (uint64_t)count;
    if (captured_at < l->owner->virtual_epoch_after && end > l->owner->virtual_accept_after) {
        l->owner->virtual_accept_after = end;
    }
    if (captured_at < l->owner->record_epoch_after && end > l->owner->record_accept_after) {
        l->owner->record_accept_after = end;
    }
    if (captured_at < l->owner->stream_epoch_after && end > l->owner->stream_accept_after) {
        l->owner->stream_accept_after = end;
    }
    if (!l->end || start > l->end || start + (uint64_t)count < l->begin) {
        l->begin = start;
        l->end = start;
    }
    for (int i = 0; i < count; i++) {
        size_t p = (size_t)((start + (uint64_t)i) % AUDIO_RING) * 2;
        l->samples[p] = isfinite(src[i * 2]) ? src[i * 2] : 0;
        l->samples[p + 1] = isfinite(src[i * 2 + 1]) ? src[i * 2 + 1] : 0;
    }
    l->end = start + (uint64_t)count;
    if (l->end - l->begin > AUDIO_RING) {
        l->begin = l->end - AUDIO_RING;
    }
}
static void capture_process(void *data)
{
    struct AudioLane *l = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(l->stream);
    if (!b) {
        return;
    }
    struct spa_buffer *buf = b->buffer;
    if (buf->n_datas && buf->datas[0].data && buf->datas[0].chunk && !l->dead &&
        l->rate == AUDIO_RATE && l->channels == 2) {
        struct spa_data *d = &buf->datas[0];
        uint32_t offset = d->chunk->offset, size = d->chunk->size;
        if (offset <= d->maxsize && size <= d->maxsize - offset) {
            int count = (int)(size / (sizeof(float) * 2));
            uint64_t now = cast_now_ns();
            uint64_t span = (uint64_t)count * 1000000000 / AUDIO_RATE;
            if (now > span) {
                put_samples(l, now - span, (const float *)((const uint8_t *)d->data + offset),
                            count);
            }
        }
    }
    pw_stream_queue_buffer(l->stream, b);
}
static void read_locked(CastAudio *a, uint64_t ns, float *dst, int count)
{
    memset(dst, 0, (size_t)count * 2 * sizeof(float));
    uint64_t start = sample_time(ns);
    for (int k = 0; k < 2; k++) {
        struct AudioLane *l = &a->lane[k];
        if (!(k ? a->cfg.desktop : a->cfg.mic) || l->dead) {
            continue;
        }
        float gain = (float)(k ? a->cfg.desktop_gain : a->cfg.mic_gain);
        for (int i = 0; i < count; i++) {
            uint64_t p = start + (uint64_t)i;
            if (p < l->begin || p >= l->end) {
                continue;
            }
            size_t q = (size_t)(p % AUDIO_RING) * 2;
            dst[i * 2] += l->samples[q] * gain;
            dst[i * 2 + 1] += l->samples[q + 1] * gain;
        }
    }
    for (int i = 0; i < count * 2; i++) {
        dst[i] = fmaxf(-1.f, fminf(1.f, dst[i]));
    }
}
static void read_virtual_locked(CastAudio *a, uint64_t ns, float *dst, int count)
{
    if (a->virtual_silent || a->virtual_lane.dead) {
        memset(dst, 0, (size_t)count * 2 * sizeof(float));
    } else {
        read_locked(a, ns, dst, count);
        uint64_t start = sample_time(ns);
        uint64_t stale = a->virtual_accept_after > start ? a->virtual_accept_after - start : 0;
        if (stale > (uint64_t)count) {
            stale = (uint64_t)count;
        }
        memset(dst, 0, (size_t)stale * 2 * sizeof(float));
    }
}
static void virtual_process(void *data)
{
    struct AudioLane *l = data;
    CastAudio *a = l->owner;
    struct pw_buffer *b = pw_stream_dequeue_buffer(l->stream);
    if (!b) {
        return;
    }
    struct spa_buffer *buf = b->buffer;
    if (buf->n_datas && buf->datas[0].data && buf->datas[0].chunk) {
        struct spa_data *d = &buf->datas[0];
        uint32_t count = d->maxsize / (sizeof(float) * 2);
        if (b->requested && b->requested < count) {
            count = (uint32_t)b->requested;
        }
        float *p = d->data;
        uint64_t now = cast_now_ns(), span = (uint64_t)count * 1000000000 / AUDIO_RATE;
        if (now < span) {
            memset(p, 0, (size_t)count * 2 * sizeof(float));
        } else {
            read_virtual_locked(a, now - span, p, (int)count);
        }
        d->chunk->offset = 0;
        d->chunk->size = count * sizeof(float) * 2;
        d->chunk->stride = sizeof(float) * 2;
        b->size = count;
    }
    pw_stream_queue_buffer(l->stream, b);
}
static const struct pw_stream_events capture_events = {.version = PW_VERSION_STREAM_EVENTS,
                                                       .state_changed = lane_state,
                                                       .param_changed = lane_format,
                                                       .process = capture_process};
static const struct pw_stream_events virtual_events = {.version = PW_VERSION_STREAM_EVENTS,
                                                       .state_changed = lane_state,
                                                       .param_changed = lane_format,
                                                       .process = virtual_process};
static struct AudioNode *resolve(CastAudio *a, const char *name, bool desktop)
{
    if (!*name || !strcmp(name, "default")) {
        if (desktop) {
            return NULL;
        }
        name = a->default_mic;
    }
    for (int i = 0; i < a->count; i++) {
        struct AudioNode *v = &a->nodes[i];
        char id[32];
        snprintf(id, sizeof(id), "%u", v->id);
        if (strcmp(v->name, name) && strcmp(v->serial, name) && strcmp(id, name)) {
            continue;
        }
        if (!strcmp(v->name, a->cfg.virtual_name) || !strncmp(v->name, "cast.capture.", 13)) {
            return NULL;
        }
        if (!desktop && strncmp(v->kind, "Audio/Source", 12)) {
            return NULL;
        }
        return v;
    }
    return NULL;
}
static int validate_locked(CastAudio *a, const Config *cfg, char *e, size_t n)
{
    if (!a->core || a->server_dead) {
        if (cfg->mic || cfg->desktop || cfg->virtual_audio) {
            snprintf(e, n, "PipeWire unavailable: %s. Start the user PipeWire/WirePlumber services",
                     a->error);
            return -1;
        }
        return 0;
    }
    struct AudioNode *mic = cfg->mic ? resolve(a, cfg->mic_source, false) : NULL,
                     *desktop = cfg->desktop ? resolve(a, cfg->desktop_source, true) : NULL;
    if (cfg->mic && !mic) {
        snprintf(e, n, "mic source '%s' is unavailable or not Audio/Source; use cast audio list",
                 cfg->mic_source[0] ? cfg->mic_source : "default");
        return -1;
    }
    if (cfg->desktop && !desktop) {
        snprintf(e, n,
                 "desktop capture requires an explicit available source; '%s' unavailable (cast "
                 "audio list); no speaker fallback",
                 cfg->desktop_source);
        return -1;
    }
    if (mic && desktop && mic->id == desktop->id) {
        snprintf(e, n,
                 "mic and desktop select the same source, which would duplicate microphone audio");
        return -1;
    }
    if ((mic && !strcmp(mic->name, cfg->virtual_name)) ||
        (desktop && !strcmp(desktop->name, cfg->virtual_name))) {
        snprintf(e, n, "cast's virtual microphone cannot be a capture source (feedback)");
        return -1;
    }
    return 0;
}
static void destroy_lane(struct AudioLane *l)
{
    if (l->stream) {
        spa_hook_remove(&l->listener);
        pw_stream_destroy(l->stream);
        l->stream = NULL;
    }
    clear_lane(l);
    l->error[0] = 0;
    l->dead = false;
}
static int connect_lane(struct AudioLane *l, struct AudioNode *node, bool virt, const Config *cfg,
                        char *e, size_t n)
{
    CastAudio *a = l->owner;
    struct pw_properties *p =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                          virt ? "Capture" : "Capture", PW_KEY_MEDIA_ROLE, "Communication",
                          "node.dont-reconnect", "true", "node.dont-move", "true", NULL);
    if (!p) {
        snprintf(e, n, "PipeWire properties allocation failed");
        return -1;
    }
    if (virt) {
        pw_properties_set(p, PW_KEY_MEDIA_CLASS, "Audio/Source");
        pw_properties_set(p, PW_KEY_NODE_NAME, cfg->virtual_name);
        pw_properties_set(p, PW_KEY_NODE_DESCRIPTION, cfg->virtual_name);
        pw_properties_set(p, "node.virtual", "true");
        pw_properties_set(p, "node.pause-on-idle", "false");
    } else {
        l->node = node->id;
        pw_properties_set(p, PW_KEY_TARGET_OBJECT, node->serial[0] ? node->serial : node->name);
        pw_properties_set(p, PW_KEY_NODE_NAME, "cast.capture.audio");
        if (!strcmp(node->kind, "Audio/Sink")) {
            pw_properties_set(p, PW_KEY_STREAM_CAPTURE_SINK, "true");
        }
    }
    l->stream = pw_stream_new(a->core, virt ? cfg->virtual_name : "cast selected audio", p);
    if (!l->stream) {
        snprintf(e, n, "PipeWire stream allocation failed");
        return -1;
    }
    pw_stream_add_listener(l->stream, &l->listener, virt ? &virtual_events : &capture_events, l);
    uint8_t storage[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const struct spa_pod *params[] = {spa_format_audio_raw_build(
        &b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32, .rate = AUDIO_RATE, .channels = 2,
                                 .position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR}))};
    enum pw_stream_flags flags = PW_STREAM_FLAG_MAP_BUFFERS;
    if (!virt) {
        flags |= PW_STREAM_FLAG_AUTOCONNECT;
    }
    int rc = pw_stream_connect(l->stream, virt ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
                               PW_ID_ANY, flags, params, 1);
    if (rc < 0) {
        snprintf(e, n, "connect PipeWire source: %s", strerror(-rc));
        destroy_lane(l);
        return -1;
    }
    struct timespec until;
    pw_thread_loop_get_time(a->loop, &until, 2000000000);
    while (pw_stream_get_state(l->stream, NULL) == PW_STREAM_STATE_CONNECTING && !a->server_dead) {
        if (pw_thread_loop_timed_wait_full(a->loop, &until) < 0) {
            snprintf(e, n, "PipeWire source connection timed out");
            destroy_lane(l);
            return -1;
        }
    }
    if (a->server_dead || l->dead ||
        pw_stream_get_state(l->stream, NULL) == PW_STREAM_STATE_ERROR) {
        snprintf(e, n, "%s", l->error[0] ? l->error : a->error);
        destroy_lane(l);
        return -1;
    }
    return 0;
}
CastAudio *audio_open(const Config *cfg, char *e, size_t n)
{
    pthread_once(&pw_once, initialize_pw);
    CastAudio *a = calloc(1, sizeof(*a));
    if (!a) {
        snprintf(e, n, "audio allocation failed");
        return NULL;
    }
    a->cfg = *cfg;
    a->virtual_silent = true;
    for (int i = 0; i < 2; i++) {
        a->lane[i].owner = a;
        a->lane[i].samples = calloc(AUDIO_RING * 2, sizeof(float));
        if (!a->lane[i].samples) {
            snprintf(e, n, "audio ring allocation failed");
            audio_close(a);
            return NULL;
        }
    }
    a->virtual_lane.owner = a;
    a->loop = pw_thread_loop_new("cast audio", NULL);
    if (!a->loop) {
        goto offline;
    }
    a->context = pw_context_new(pw_thread_loop_get_loop(a->loop), NULL, 0);
    if (!a->context) {
        goto offline;
    }
    a->core = pw_context_connect(a->context, NULL, 0);
    if (!a->core) {
        goto offline;
    }
    pw_core_add_listener(a->core, &a->core_listener, &core_events, a);
    a->registry = pw_core_get_registry(a->core, PW_VERSION_REGISTRY, 0);
    if (!a->registry) {
        goto offline;
    }
    pw_registry_add_listener(a->registry, &a->registry_listener, &registry_events, a);
    if (pw_thread_loop_start(a->loop) < 0) {
        goto offline;
    }
    a->started = true;
    pw_thread_loop_lock(a->loop);
    int rc = roundtrip(a);
    if (rc == 0) {
        rc = roundtrip(a);
    }
    pw_thread_loop_unlock(a->loop);
    if (rc < 0) {
        goto offline;
    }
    if (audio_configure(a, cfg, e, n) < 0) {
        audio_close(a);
        return NULL;
    }
    return a;
offline:
    if (!a->error[0]) {
        snprintf(a->error, sizeof(a->error), "%s", strerror(errno));
    }
    a->server_dead = true;
    if (cfg->mic || cfg->desktop || cfg->virtual_audio) {
        snprintf(e, n, "PipeWire unavailable: %s", a->error);
        audio_close(a);
        return NULL;
    }
    return a;
}
void audio_close(CastAudio *a)
{
    if (!a) {
        return;
    }
    if (a->started) {
        pw_thread_loop_stop(a->loop);
    }
    for (int i = 0; i < 2; i++) {
        destroy_lane(&a->lane[i]);
        free(a->lane[i].samples);
    }
    destroy_lane(&a->virtual_lane);
    if (a->metadata) {
        spa_hook_remove(&a->metadata_listener);
        pw_proxy_destroy((struct pw_proxy *)a->metadata);
    }
    if (a->registry) {
        spa_hook_remove(&a->registry_listener);
        pw_proxy_destroy((struct pw_proxy *)a->registry);
    }
    if (a->core) {
        spa_hook_remove(&a->core_listener);
        pw_core_disconnect(a->core);
    }
    if (a->context) {
        pw_context_destroy(a->context);
    }
    if (a->loop) {
        pw_thread_loop_destroy(a->loop);
    }
    free(a);
}
int audio_validate(CastAudio *a, const Config *cfg, char *e, size_t n)
{
    if (a->started) {
        pw_thread_loop_lock(a->loop);
    }
    int r = validate_locked(a, cfg, e, n);
    if (a->started) {
        pw_thread_loop_unlock(a->loop);
    }
    return r;
}
int audio_configure(CastAudio *a, const Config *cfg, char *e, size_t n)
{
    if (a->started) {
        pw_thread_loop_lock(a->loop);
    }
    int rc = validate_locked(a, cfg, e, n);
    if (rc < 0) {
        goto done;
    }
    if (a->server_dead || !a->core) {
        a->cfg = *cfg;
        goto done;
    }
    /* Create every changed lane before replacing any old lane. Candidate callbacks
     * write candidate rings only; failing one connection leaves the old routes intact. */
    struct AudioLane *candidate[3] = {NULL, NULL, NULL};
    bool change[3] = {
        cfg->mic != a->cfg.mic || strcmp(cfg->mic_source, a->cfg.mic_source) ||
            (cfg->mic && (!a->lane[0].stream || a->lane[0].dead)),
        cfg->desktop != a->cfg.desktop || strcmp(cfg->desktop_source, a->cfg.desktop_source) ||
            (cfg->desktop && (!a->lane[1].stream || a->lane[1].dead)),
        cfg->virtual_audio != a->cfg.virtual_audio ||
            strcmp(cfg->virtual_name, a->cfg.virtual_name) ||
            (cfg->virtual_audio && (!a->virtual_lane.stream || a->virtual_lane.dead))};
    for (int i = 0; i < 3; i++) {
        if (change[i] && (i == 0 ? cfg->mic : i == 1 ? cfg->desktop : cfg->virtual_audio)) {
            candidate[i] = calloc(1, sizeof(**candidate));
            if (!candidate[i]) {
                snprintf(e, n, "audio candidate allocation failed");
                rc = -1;
                break;
            }
            candidate[i]->owner = a;
            if (i < 2) {
                candidate[i]->samples = calloc(AUDIO_RING * 2, sizeof(float));
                if (!candidate[i]->samples) {
                    snprintf(e, n, "audio candidate ring allocation failed");
                    rc = -1;
                    break;
                }
            }
            if (connect_lane(candidate[i],
                             i < 2 ? resolve(a, i ? cfg->desktop_source : cfg->mic_source, i == 1)
                                   : NULL,
                             i == 2, cfg, e, n) < 0) {
                rc = -1;
                break;
            }
        }
    }
    if (rc == 0) {
        for (int i = 0; i < 3; i++) {
            if (change[i]) {
                struct AudioLane *old = i < 2 ? &a->lane[i] : &a->virtual_lane;
                float *previous_samples = old->samples;
                destroy_lane(old);
                old->samples = NULL;
                /* Listener userdata points to stable allocated candidates, so move callback
                 * registration after copying the lane to its permanent address. */
                if (candidate[i]) {
                    free(previous_samples);
                    spa_hook_remove(&candidate[i]->listener);
                    *old = *candidate[i];
                    pw_stream_add_listener(old->stream, &old->listener,
                                           i == 2 ? &virtual_events : &capture_events, old);
                    candidate[i]->stream = NULL;
                    candidate[i]->samples = NULL;
                } else {
                    memset(old, 0, sizeof(*old));
                    old->owner = a;
                    old->samples = previous_samples;
                }
            }
        }
        a->cfg = *cfg;
    }
    for (int i = 0; i < 3; i++) {
        if (candidate[i]) {
            destroy_lane(candidate[i]);
            free(candidate[i]->samples);
            free(candidate[i]);
        }
    }
done:
    if (a->started) {
        pw_thread_loop_unlock(a->loop);
    }
    return rc;
}
void audio_barrier(CastAudio *a, bool silent)
{
    if (a->started) {
        pw_thread_loop_lock(a->loop);
    }
    a->virtual_silent = silent;
    a->accept_after = sample_time(cast_now_ns());
    a->virtual_accept_after = a->record_accept_after = a->stream_accept_after = a->accept_after;
    a->virtual_epoch_after = a->record_epoch_after = a->stream_epoch_after = a->accept_after;
    for (int i = 0; i < 2; i++) {
        clear_lane(&a->lane[i]);
        if (a->lane[i].stream) {
            for (int k = 0; k < 32; k++) {
                struct pw_buffer *b = pw_stream_dequeue_buffer(a->lane[i].stream);
                if (!b) {
                    break;
                }
                pw_stream_queue_buffer(a->lane[i].stream, b);
            }
            pw_stream_flush(a->lane[i].stream, false);
        }
    }
    if (a->virtual_lane.stream) {
        pw_stream_flush(a->virtual_lane.stream, false);
    }
    if (a->started) {
        pw_thread_loop_unlock(a->loop);
    }
}
static uint64_t privacy_after(CastAudio *a, uint64_t after)
{
    for (int i = 0; i < 2; i++) {
        if (a->lane[i].end > after) {
            after = a->lane[i].end;
        }
    }
    return after;
}
void audio_virtual_privacy(CastAudio *a, bool silent)
{
    if (a->started) {
        pw_thread_loop_lock(a->loop);
    }
    a->virtual_silent = silent;
    a->virtual_epoch_after = sample_time(cast_now_ns());
    a->virtual_accept_after = privacy_after(a, a->virtual_epoch_after);
    if (a->virtual_lane.stream) {
        pw_stream_flush(a->virtual_lane.stream, false);
    }
    if (a->started) {
        pw_thread_loop_unlock(a->loop);
    }
}
void audio_record_privacy(CastAudio *a)
{
    if (a->started) {
        pw_thread_loop_lock(a->loop);
    }
    a->record_epoch_after = sample_time(cast_now_ns());
    a->record_accept_after = privacy_after(a, a->record_epoch_after);
    if (a->started) {
        pw_thread_loop_unlock(a->loop);
    }
}
void audio_read(CastAudio *a, uint64_t ns, float *dst, int count)
{
    if (a->started) {
        pw_thread_loop_lock(a->loop);
    }
    read_locked(a, ns, dst, count);
    uint64_t start = sample_time(ns);
    uint64_t stale = a->record_accept_after > start ? a->record_accept_after - start : 0;
    if (stale > (uint64_t)count) {
        stale = (uint64_t)count;
    }
    memset(dst, 0, (size_t)stale * 2 * sizeof(float));
    if (a->started) {
        pw_thread_loop_unlock(a->loop);
    }
}
void audio_stream_privacy(CastAudio *a)
{
    if (a->started) {
        pw_thread_loop_lock(a->loop);
    }
    a->stream_epoch_after = sample_time(cast_now_ns());
    a->stream_accept_after = privacy_after(a, a->stream_epoch_after);
    if (a->started) {
        pw_thread_loop_unlock(a->loop);
    }
}
void audio_stream_read(CastAudio *a, uint64_t ns, float *dst, int count)
{
    if (a->started) {
        pw_thread_loop_lock(a->loop);
    }
    read_locked(a, ns, dst, count);
    uint64_t start = sample_time(ns);
    uint64_t stale = a->stream_accept_after > start ? a->stream_accept_after - start : 0;
    if (stale > (uint64_t)count) {
        stale = (uint64_t)count;
    }
    memset(dst, 0, (size_t)stale * 2 * sizeof(float));
    if (a->started) {
        pw_thread_loop_unlock(a->loop);
    }
}
void audio_list(CastAudio *a, char *out, size_t n)
{
    if (a->started) {
        pw_thread_loop_lock(a->loop);
    }
    size_t used = 0;
    if (n) {
        out[0] = 0;
    }
    if (a->server_dead) {
        snprintf(out, n, "PipeWire unavailable: %s", a->error);
    } else {
        for (int i = 0; i < a->count && used < n; i++) {
            struct AudioNode *v = &a->nodes[i];
            int r = snprintf(out + used, n - used, "%s [id=%u serial=%s class=%s] %s%s\n", v->name,
                             v->id, v->serial, v->kind, v->description,
                             !strcmp(v->name, a->default_mic) ? " (default mic)" : "");
            if (r > 0) {
                used += (size_t)r;
            }
        }
    }
    if (a->started) {
        pw_thread_loop_unlock(a->loop);
    }
}
void audio_status(CastAudio *a, char *out, size_t n)
{
    /* Names remain in Config; this JSON only reports route readiness and silence,
     * avoiding unescaped arbitrary device descriptions. */
    if (a->started) {
        pw_thread_loop_lock(a->loop);
    }
    snprintf(out, n,
             "{\"available\":%s,\"mic_ready\":%s,\"desktop_ready\":%s,\"virtual_ready\":%s,"
             "\"virtual_silent\":%s}",
             a->server_dead ? "false" : "true",
             a->lane[0].stream && !a->lane[0].dead ? "true" : "false",
             a->lane[1].stream && !a->lane[1].dead ? "true" : "false",
             a->virtual_lane.stream && !a->virtual_lane.dead ? "true" : "false",
             a->virtual_silent ? "true" : "false");
    if (a->started) {
        pw_thread_loop_unlock(a->loop);
    }
}
void audio_error(CastAudio *a, char *out, size_t n)
{
    if (a->started) {
        pw_thread_loop_lock(a->loop);
    }
    snprintf(out, n, "%s",
             a->cfg.mic && a->lane[0].error[0]                        ? a->lane[0].error
             : a->cfg.desktop && a->lane[1].error[0]                  ? a->lane[1].error
             : (a->cfg.mic || a->cfg.desktop || a->cfg.virtual_audio) ? a->error
                                                                      : "");
    if (a->started) {
        pw_thread_loop_unlock(a->loop);
    }
}
#ifdef CAST_TEST
CastAudio *audio_test_open(const Config *config)
{
    /* The synthetic source exercises the same ring readers and privacy epochs
     * without connecting to a PipeWire server or loading its input modules. */
    pthread_once(&pw_once, initialize_pw);
    CastAudio *audio = calloc(1, sizeof(*audio));
    if (!audio) {
        return NULL;
    }
    audio->cfg = *config;
    audio->virtual_silent = true;
    audio->virtual_lane.owner = audio;
    for (int lane = 0; lane < 2; lane++) {
        audio->lane[lane].owner = audio;
        audio->lane[lane].samples = calloc(AUDIO_RING * 2, sizeof(float));
        if (!audio->lane[lane].samples) {
            audio_close(audio);
            return NULL;
        }
    }
    audio->loop = pw_thread_loop_new("cast synthetic audio", NULL);
    if (!audio->loop || pw_thread_loop_start(audio->loop) < 0) {
        audio_close(audio);
        return NULL;
    }
    audio->started = true;
    return audio;
}
void audio_test_virtual_read(CastAudio *a, uint64_t ns, float *dst, int count)
{
    if (a->started) {
        pw_thread_loop_lock(a->loop);
    }
    read_virtual_locked(a, ns, dst, count);
    if (a->started) {
        pw_thread_loop_unlock(a->loop);
    }
}
void audio_test_push(CastAudio *a, int lane, uint64_t ns, const float *src, int count)
{
    if (a->started) {
        pw_thread_loop_lock(a->loop);
    }
    if (lane == 0) {
        a->cfg.mic = true;
    } else {
        a->cfg.desktop = true;
    }
    put_samples(&a->lane[lane], ns, src, count);
    if (a->started) {
        pw_thread_loop_unlock(a->loop);
    }
}
#endif
