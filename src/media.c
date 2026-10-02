#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "media_internal.h"
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>
struct Media {
    Config cfg;
    CastCamera *camera;
    CastOutput *output;
    CastAudio *audio;
    CastRecorder *recorder;
    bool live_silent, record_silent, record_cut;
    uint64_t live_drops;
    char camera_error[CAST_ERR], live_error[CAST_ERR];
};
Media *media_open(const Config *cfg, char *e, size_t n)
{
    Media *m = calloc(1, sizeof(*m));
    if (!m) {
        snprintf(e, n, "media allocation failed");
        return NULL;
    }
    m->cfg = *cfg;
    m->live_silent = true;
    m->output = output_open(cfg, e, n);
    if (!m->output) {
        goto fail;
    }
    if (cfg->camera_enabled) {
        m->camera = camera_open(cfg, m->camera_error, sizeof(m->camera_error));
    }
    m->audio = audio_open(cfg, e, n);
    if (!m->audio) {
        goto fail;
    }
    m->recorder = recorder_open(m->audio);
    if (!m->recorder) {
        snprintf(e, n, "recording worker creation failed");
        goto fail;
    }
    return m;
fail:
    media_close(m);
    return NULL;
}
void media_close(Media *m)
{
    if (!m) {
        return;
    }
    recorder_close(m->recorder);
    audio_close(m->audio);
    camera_close(m->camera);
    output_close(m->output);
    free(m);
}
void media_camera_list(char *out, size_t n)
{
    camera_list(out, n);
}

int media_camera(Media *m, Frame *f, char *e, size_t n)
{
    if (!m->cfg.camera_enabled) {
        frame_free(f);
        return 0;
    }
    if (!m->camera) {
        snprintf(e, n, "%s",
                 m->camera_error[0]
                     ? m->camera_error
                     : "camera unavailable; select a device with cast camera device /dev/videoN");
        frame_free(f);
        return -1;
    }
    int rc = camera_frame(m->camera, f, e, n);
    if (rc < 0) {
        snprintf(m->camera_error, sizeof(m->camera_error), "%s", e);
        frame_free(f);
    } else {
        m->camera_error[0] = 0;
    }
    return rc;
}
int media_live(Media *m, const Frame *f, bool silent, char *e, size_t n)
{
    if (silent != m->live_silent) {
        audio_live_privacy(m->audio, silent);
        m->live_silent = silent;
    }
    int rc = output_frame(m->output, f, e, n);
    if (rc != 0) {
        m->live_drops++;
        snprintf(m->live_error, sizeof(m->live_error), "%s", e);
        return -1;
    }
    m->live_error[0] = 0;
    return 0;
}
int media_record_start(Media *m, const Config *cfg, const char *path, char *e, size_t n)
{
    audio_record_privacy(m->audio);
    int rc = recorder_start(m->recorder, cfg, path, e, n);
    if (!rc) {
        m->record_cut = m->record_silent = false;
    }
    return rc;
}
int media_record_stop(Media *m, char *e, size_t n)
{
    int rc = recorder_stop(m->recorder, e, n);
    if (!rc) {
        m->record_cut = m->record_silent = false;
    }
    return rc;
}
int media_record_pause(Media *m, bool paused, char *e, size_t n)
{
    if (paused != m->record_cut) {
        audio_record_privacy(m->audio);
    }
    int rc = recorder_pause(m->recorder, paused, e, n);
    if (!rc) {
        m->record_cut = paused;
    }
    return rc;
}
int media_record_frame(Media *m, const Frame *f, char *e, size_t n)
{
    int rc = recorder_frame(m->recorder, f, e, n);
    return rc < 0 ? -1 : 0;
}
int media_record_silence(Media *m, bool silent, char *e, size_t n)
{
    if (silent != m->record_silent) {
        audio_record_privacy(m->audio);
    }
    int result = recorder_silence(m->recorder, silent, e, n);
    if (!result) {
        m->record_silent = silent;
    }
    return result;
}
void media_barrier(Media *m)
{
    camera_barrier(m->camera);
    audio_barrier(m->audio, m->live_silent);
    recorder_barrier(m->recorder);
}
void media_record_barrier(Media *m)
{
    audio_record_privacy(m->audio);
    recorder_barrier(m->recorder);
}
int media_privacy(Media *m, bool live_silent, bool record_silent, bool record_cut, char *error,
                  size_t n)
{
    if (live_silent != m->live_silent) {
        audio_live_privacy(m->audio, live_silent);
        m->live_silent = live_silent;
    }
    bool active, cut;
    uint64_t dropped;
    char recorder_error[CAST_ERR];
    recorder_status(m->recorder, &active, &cut, &dropped, recorder_error, sizeof recorder_error);
    if (!active) {
        m->record_silent = false;
        m->record_cut = false;
        return 0;
    }
    if (record_silent != m->record_silent && media_record_silence(m, record_silent, error, n)) {
        return -1;
    }
    if (record_cut != m->record_cut && media_record_pause(m, record_cut, error, n)) {
        return -1;
    }
    return 0;
}
static bool encoding_changed(const Config *a, const Config *b)
{
    return strcmp(a->video_codec, b->video_codec) || strcmp(a->audio_codec, b->audio_codec) ||
           strcmp(a->record_container, b->record_container) ||
           strcmp(a->record_preset, b->record_preset) || a->record_crf != b->record_crf ||
           a->record_queue != b->record_queue;
}
int media_reconfigure(Media *m, const Config *cfg, bool recording, char *e, size_t n)
{
    if (m->cfg.width != cfg->width || m->cfg.height != cfg->height || m->cfg.fps != cfg->fps ||
        strcmp(m->cfg.output_device, cfg->output_device) ||
        m->cfg.live_enabled != cfg->live_enabled) {
        snprintf(e, n, "output device/dimensions/fps changes require restart");
        return -1;
    }
    if (recording && encoding_changed(&m->cfg, cfg)) {
        snprintf(e, n,
                 "recording codec/container/queue changes require stopping the current recording");
        return -1;
    }
    if (audio_validate(m->audio, cfg, e, n) < 0) {
        return -1;
    }
    bool camera_change = m->cfg.camera_enabled != cfg->camera_enabled ||
                         strcmp(m->cfg.camera_device, cfg->camera_device) ||
                         (cfg->camera_enabled && (!m->camera || camera_failed(m->camera)));
    CastCamera *candidate = NULL;
    if (camera_change && cfg->camera_enabled) {
        candidate = camera_open(cfg, e, n);
        if (!candidate) {
            return -1;
        }
    }
    if (audio_configure(m->audio, cfg, e, n) < 0) {
        camera_close(candidate);
        return -1;
    }
    if (camera_change) {
        camera_close(m->camera);
        m->camera = candidate;
        m->camera_error[0] = 0;
    }
    m->cfg = *cfg;
    media_barrier(m);
    return 0;
}
static int set_bool(bool *value, const char *word, char *e, size_t n)
{
    if (!strcmp(word, "on")) {
        *value = true;
    } else if (!strcmp(word, "off")) {
        *value = false;
    } else if (!strcmp(word, "toggle")) {
        *value = !*value;
    } else {
        snprintf(e, n, "expected on|off|toggle");
        return -1;
    }
    return 0;
}
static int gain_value(const char *text, double *gain, char *e, size_t n)
{
    errno = 0;
    char *end;
    double v = strtod(text, &end);
    if (errno || end == text || !isfinite(v) || strcmp(end, "%") || v < 0 || v > 400) {
        snprintf(e, n, "audio gain must be 0%%..400%%, e.g. 80%%");
        return -1;
    }
    *gain = v / 100;
    return 0;
}
int media_audio_command(Media *m, Config *cfg, int argc, char **argv, char *reply, size_t n)
{
    if (argc == 2 && !strcmp(argv[1], "list")) {
        audio_list(m->audio, reply, n);
        return 0;
    }
    if (argc < 3 || argc > 4 ||
        (strcmp(argv[1], "mic") && strcmp(argv[1], "desktop") && strcmp(argv[1], "virtual"))) {
        snprintf(reply, n,
                 "audio list | audio mic|desktop on|off|toggle | audio mic|desktop source NAME | "
                 "audio mic|desktop gain PERCENT | audio virtual on|off|toggle");
        return -1;
    }
    Config candidate = *cfg;
    bool desktop = !strcmp(argv[1], "desktop"), virt = !strcmp(argv[1], "virtual");
    bool source = false;
    if (argc == 3) {
        bool *v = virt ? &candidate.virtual_audio : desktop ? &candidate.desktop : &candidate.mic;
        if (set_bool(v, argv[2], reply, n) < 0) {
            return -1;
        }
    } else if (virt) {
        snprintf(reply, n, "audio virtual supports only on|off|toggle");
        return -1;
    } else if (!strcmp(argv[2], "source")) {
        char *v = desktop ? candidate.desktop_source : candidate.mic_source;
        if (!argv[3][0] || strlen(argv[3]) >= 256) {
            snprintf(reply, n,
                     "audio source must be a nonempty name or PipeWire node id/serial, at most 255 "
                     "bytes");
            return -1;
        }
        snprintf(v, 256, "%s", argv[3]);
        source = true;
    } else if (!strcmp(argv[2], "gain")) {
        if (gain_value(argv[3], desktop ? &candidate.desktop_gain : &candidate.mic_gain, reply, n) <
            0) {
            return -1;
        }
    } else {
        snprintf(reply, n, "expected source NAME or gain PERCENT");
        return -1;
    }
    if (source) {
        Config validation = candidate;
        if (desktop) {
            validation.desktop = true;
        } else {
            validation.mic = true;
        }
        if (audio_validate(m->audio, &validation, reply, n) < 0) {
            return -1;
        }
    }
    if (audio_configure(m->audio, &candidate, reply, n) < 0) {
        return -1;
    }
    *cfg = candidate;
    m->cfg = candidate;
    media_barrier(m);
    snprintf(reply, n, "audio routing updated; off lanes and missing sources produce silence");
    return 0;
}
void media_status(Media *m, bool *active, bool *paused, uint64_t *drops, char *error, size_t n)
{
    recorder_status(m->recorder, active, paused, drops, error, n);
    *drops += m->live_drops;
    if (!error[0]) {
        audio_error(m->audio, error, n);
    }
    if (!error[0] && m->camera_error[0]) {
        snprintf(error, n, "%s", m->camera_error);
    }
    if (!error[0] && m->live_error[0]) {
        snprintf(error, n, "%s", m->live_error);
    }
}
void media_record_error(Media *m, char *out, size_t n)
{
    bool active, paused;
    uint64_t drops;
    recorder_status(m->recorder, &active, &paused, &drops, out, n);
}
bool media_record_finalizing(Media *m)
{
    return recorder_finalizing(m->recorder);
}
uint64_t media_record_duration(Media *m)
{
    return recorder_duration(m->recorder);
}
void media_record_path(Media *m, char *out, size_t n)
{
    recorder_path(m->recorder, out, n);
}
void media_audio_status(Media *m, char *out, size_t n)
{
    audio_status(m->audio, out, n);
}
void media_doctor(const Config *cfg, char *out, size_t n)
{
    camera_doctor(cfg, out, n);
    size_t used = strlen(out);
    snprintf(
        out + used, n - used,
        "Encoders: video %s=%s, audio %s=%s (software libx264/aac baseline).\n", cfg->video_codec,
        avcodec_find_encoder_by_name(cfg->video_codec) ? "available" : "MISSING", cfg->audio_codec,
        avcodec_find_encoder_by_name(cfg->audio_codec) ? "available" : "MISSING");
    used = strlen(out);
    struct statvfs fs;
    if (statvfs(cfg->record_dir, &fs) < 0 || access(cfg->record_dir, W_OK | X_OK) < 0) {
        snprintf(out + used, n - used,
                 "Recording directory %s: not writable/searchable: %s; choose/create a writable "
                 "directory.\n",
                 cfg->record_dir, strerror(errno));
    } else {
        snprintf(out + used, n - used,
                 "Recording directory %s: %.1f GiB available%s; explicit filenames use exclusive "
                 "creation.\n",
                 cfg->record_dir, (double)fs.f_bavail * fs.f_frsize / (1024 * 1024 * 1024),
                 fs.f_flag & ST_RDONLY ? " (READ ONLY)" : "");
    }
    Config probe = *cfg;
    probe.mic = probe.desktop = probe.virtual_audio = false;
    char e[CAST_ERR];
    CastAudio *a = audio_open(&probe, e, sizeof(e));
    used = strlen(out);
    if (a) {
        char sources[2048];
        audio_list(a, sources, sizeof(sources));
        snprintf(out + used, n - used,
                 "PipeWire sources (read-only enumeration):\n%s\nDesktop sinks include any call "
                 "audio routed to that sink. Stream/Output/Audio selects one application; no "
                 "source fallback.\n",
                 sources);
        audio_close(a);
    } else {
        snprintf(out + used, n - used, "PipeWire probe failed: %s\n", e);
    }
}
#ifdef CAST_TEST
CastAudio *media_test_audio(Media *m)
{
    return m->audio;
}
CastRecorder *media_test_recorder(Media *m)
{
    return m->recorder;
}
#endif
