#include "project_session.h"
#include "app_internal.h"
#include "project.h"
#include "timeline_audio.h"
#include "timeline_preview.h"
#include "timeline_render.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Main-thread document/history. One bounded worker slot owns immutable copies
 * while busy; only job handoff/completion uses the mutex. No worker touches App. */
typedef enum {
    JOB_NONE,
    JOB_SAVE,
    JOB_AUTOSAVE,
    JOB_OPEN,
    JOB_IMPORT,
    JOB_RELINK
} JobKind;
typedef struct {
    JobKind kind;
    char path[PATH_MAX], error[CAST_ERR];
    Config config, original_config;
    CastTimeline timeline;
    CastProjectReport report;
    uint64_t source_revision, duration_ns, media_id;
    CastTimelineKind media_kind;
    unsigned track;
    int result;
} ProjectJob;
struct CastProjectSession {
    CastTimelinePreview *preview, *retired_preview;
    CastTimelineAudio *audio, *retired_audio;
    bool audio_requested, audio_running;
    char audio_error[256];
    Frame review_frame;
    bool reviewing, preview_busy, playing, transport_pending;
    uint64_t play_started_ns, play_started_position, next_video_ns;
#ifdef CAST_TEST
    uint64_t test_clock;
    bool test_clock_set;
#endif
    uint64_t seek_generation, copied_generation;
    char preview_error[1024];
    CastTimelineExport *export;
    CastTimelineExportSnapshot export_snapshot;
    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    bool stop, pending, done, busy, open, dirty;
    ProjectJob job;
    CastTimeline timeline;
    CastTimelineHistory *history;
    Config observed;
    bool observed_valid;
    uint64_t revision, playhead_ns, capture_clip, capture_start, next_autosave_ns;
    char path[PATH_MAX], cwd[PATH_MAX], status[256], error[256];
};
static uint64_t transport_now(CastProjectSession *s)
{
#ifdef CAST_TEST
    if (s->test_clock_set) {
        return s->test_clock;
    }
#else
    (void)s;
#endif
    return cast_now_ns();
}
#ifdef CAST_TEST
void project_session_test_clock(CastProjectSession *, uint64_t);
void project_session_test_clock(CastProjectSession *s, uint64_t now)
{
    s->test_clock = now;
    s->test_clock_set = true;
}
#endif
void project_session_silence(CastProjectSession *s)
{
    if (!s) {
        return;
    }
    timeline_audio_pause(s->audio);
    s->audio_requested = s->audio_running = false;
}
static void review_clear(CastProjectSession *s)
{
    s->reviewing = s->preview_busy = s->playing = s->transport_pending = false;
    s->play_started_ns = s->play_started_position = s->next_video_ns = 0;
    s->preview_error[0] = 0;
    s->audio_requested = s->audio_running = false;
    s->audio_error[0] = 0;
    if (s->audio) {
        timeline_audio_cancel(s->audio);
        s->retired_audio = s->audio;
        s->audio = NULL;
    }
    frame_free(&s->review_frame);
    if (s->preview) {
        timeline_preview_cancel(s->preview);
        s->retired_preview = s->preview;
    }
    s->preview = NULL;
    s->copied_generation = 0;
}
const Frame *project_session_review_frame(CastProjectSession *s)
{
    return s && s->reviewing && s->review_frame.data ? &s->review_frame : NULL;
}
static int fail(char *error, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (size) {
        vsnprintf(error, size, format, args);
    }
    va_end(args);
    return -1;
}
static uint64_t track_end(const CastTimeline *t, unsigned track)
{
    uint64_t end = 0;
    for (unsigned i = 0; i < t->clip_count; ++i) {
        if (t->clips[i].track == track && t->clips[i].start_ns + t->clips[i].duration_ns > end) {
            end = t->clips[i].start_ns + t->clips[i].duration_ns;
        }
    }
    return end;
}
static bool idle(const App *a)
{
    return !a->state.recording && !a->record_finalizing && !a->countdown &&
           !a->state.stream_active &&
           (a->state.stream_connection_state == STREAM_STOPPED ||
            a->state.stream_connection_state == STREAM_FAILED) &&
           (!a->media || !media_record_finalizing(a->media));
}
static int unsigned_number(const char *text, uint64_t *out)
{
    if (!text || *text < '0' || *text > '9') {
        return -1;
    }
    errno = 0;
    char *end;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end) {
        return -1;
    }
    *out = (uint64_t)value;
    return 0;
}
static int pair(const char *text, uint64_t *first, uint64_t *second)
{
    if (!text || strlen(text) >= 128) {
        return -1;
    }
    char buffer[128];
    strcpy(buffer, text);
    char *comma = strchr(buffer, ',');
    if (!comma || strchr(comma + 1, ',')) {
        return -1;
    }
    *comma++ = 0;
    return unsigned_number(buffer, first) || unsigned_number(comma, second) ? -1 : 0;
}
typedef struct {
    int fd;
    uint64_t deadline;
} ProbeIO;
static int probe_interrupt(void *opaque)
{
    return cast_now_ns() > ((ProbeIO *)opaque)->deadline;
}
static int probe_read(void *opaque, uint8_t *data, int size)
{
    ProbeIO *io = opaque;
    if (probe_interrupt(io)) {
        return AVERROR_EXIT;
    }
    ssize_t result;
    do {
        result = read(io->fd, data, (size_t)size);
    } while (result < 0 && errno == EINTR);
    return result < 0 ? AVERROR(errno) : result == 0 ? AVERROR_EOF : (int)result;
}
static int64_t probe_seek(void *opaque, int64_t offset, int whence)
{
    ProbeIO *io = opaque;
    if (probe_interrupt(io)) {
        return AVERROR_EXIT;
    }
    if (whence == AVSEEK_SIZE) {
        struct stat st;
        return fstat(io->fd, &st) ? AVERROR(errno) : st.st_size;
    }
    off_t result = lseek(io->fd, offset, whence & ~AVSEEK_FORCE);
    return result < 0 ? AVERROR(errno) : result;
}
static int probe(ProjectJob *job)
{
    int fd = open(job->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 0) {
        return fail(job->error, sizeof job->error, "cannot open local media: %s", strerror(errno));
    }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd);
        return fail(job->error, sizeof job->error, "import requires a nonempty local regular file");
    }
    ProbeIO io = {fd, cast_now_ns() + UINT64_C(10000000000)};
    AVFormatContext *format = avformat_alloc_context();
    unsigned char *buffer = av_malloc(32768);
    AVIOContext *input =
        buffer ? avio_alloc_context(buffer, 32768, 0, &io, probe_read, NULL, probe_seek) : NULL;
    AVDictionary *options = NULL;
    int result = -1;
    if (!format || !input) {
        fail(job->error, sizeof job->error, "cannot allocate media probe");
        goto finish;
    }
    format->max_streams = 16;
    format->max_index_size = 1024 * 1024;
    format->pb = input;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    format->interrupt_callback = (AVIOInterruptCB){probe_interrupt, &io};
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "format_whitelist",
                "mov,matroska,webm,wav,mp3,flac,ogg,aac,png_pipe,jpeg_pipe", 0);
    av_dict_set(&options, "probesize", "1048576", 0);
    av_dict_set(&options, "analyzeduration", "2000000", 0);
    if (avformat_open_input(&format, job->path, NULL, &options) < 0 ||
        avformat_find_stream_info(format, NULL) < 0) {
        fail(job->error, sizeof job->error, "unsupported or unreadable local media");
        goto finish;
    }
    bool audio = false, video = false, image = false;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVCodecParameters *codec = format->streams[i]->codecpar;
        if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            video = true;
            image = codec->codec_id == AV_CODEC_ID_PNG || codec->codec_id == AV_CODEC_ID_MJPEG;
        }
        audio |= codec->codec_type == AVMEDIA_TYPE_AUDIO;
    }
    if (!video && !audio) {
        fail(job->error, sizeof job->error, "media contains no supported audio/video stream");
        goto finish;
    }
    job->media_kind = image && !audio ? CAST_TIMELINE_IMAGE
                      : video         ? CAST_TIMELINE_VIDEO
                                      : CAST_TIMELINE_AUDIO;
    if (format->duration > 0) {
        job->duration_ns =
            (uint64_t)av_rescale_q(format->duration, AV_TIME_BASE_Q, (AVRational){1, 1000000000});
    }
    if ((!job->duration_ns && job->media_kind != CAST_TIMELINE_IMAGE) ||
        job->duration_ns > CAST_TIMELINE_LIMIT_NS) {
        fail(job->error, sizeof job->error, "media duration is unknown or exceeds seven days");
        goto finish;
    }
    char *absolute = realpath(job->path, NULL);
    if (!absolute || strlen(absolute) >= sizeof job->path) {
        free(absolute);
        fail(job->error, sizeof job->error, "media path cannot be resolved");
        goto finish;
    }
    struct stat current;
    if (stat(absolute, &current) || current.st_dev != st.st_dev || current.st_ino != st.st_ino) {
        free(absolute);
        fail(job->error, sizeof job->error, "media path changed while probing");
        goto finish;
    }
    strcpy(job->path, absolute);
    free(absolute);
    result = 0;
finish:
    av_dict_free(&options);
    avformat_close_input(&format);
    if (input) {
        av_freep(&input->buffer);
        avio_context_free(&input);
    } else {
        av_free(buffer);
    }
    close(fd);
    return result;
}
static void *worker(void *opaque)
{
    CastProjectSession *s = opaque;
    pthread_mutex_lock(&s->mutex);
    for (;;) {
        while (!s->pending && !s->stop) {
            pthread_cond_wait(&s->wake, &s->mutex);
        }
        if (s->stop) {
            break;
        }
        s->pending = false;
        ProjectJob *job = &s->job;
        pthread_mutex_unlock(&s->mutex);
        if (job->kind == JOB_SAVE || job->kind == JOB_AUTOSAVE) {
            job->result = cast_project_save(job->path, &job->config, &job->timeline, job->error,
                                            sizeof job->error);
        } else if (job->kind == JOB_OPEN) {
            job->result = cast_project_load(job->path, &job->config, &job->timeline, &job->report,
                                            job->error, sizeof job->error);
        } else {
            job->result = probe(job);
        }
        if (!job->result && job->kind != JOB_IMPORT) {
            char *absolute = realpath(job->path, NULL);
            if (absolute && strlen(absolute) < sizeof job->path) {
                strcpy(job->path, absolute);
            }
            free(absolute);
        }
        pthread_mutex_lock(&s->mutex);
        s->done = true;
    }
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}
CastProjectSession *project_session_open(void)
{
    CastProjectSession *s = calloc(1, sizeof *s);
    if (!s) {
        return NULL;
    }
    if (!getcwd(s->cwd, sizeof s->cwd)) {
        free(s);
        return NULL;
    }
    s->next_autosave_ns = cast_now_ns() + UINT64_C(30000000000);
    timeline_init(&s->timeline);
    s->history = timeline_history_open(8);
    if (!s->history) {
        free(s);
        return NULL;
    }
    if (pthread_mutex_init(&s->mutex, NULL)) {
        timeline_history_close(s->history);
        free(s);
        return NULL;
    }
    if (pthread_cond_init(&s->wake, NULL)) {
        pthread_mutex_destroy(&s->mutex);
        timeline_history_close(s->history);
        free(s);
        return NULL;
    }
    if (pthread_create(&s->worker, NULL, worker, s)) {
        pthread_cond_destroy(&s->wake);
        pthread_mutex_destroy(&s->mutex);
        timeline_history_close(s->history);
        free(s);
        return NULL;
    }
    snprintf(s->status, sizeof s->status,
             "No project; recording takes will appear as flattened media");
    return s;
}
void project_session_close(CastProjectSession *s)
{
    if (!s) {
        return;
    }
    review_clear(s);
    timeline_preview_close(s->retired_preview);
    timeline_audio_close(s->retired_audio);
    timeline_export_close(s->export);
    pthread_mutex_lock(&s->mutex);
    s->stop = true;
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->mutex);
    pthread_join(s->worker, NULL);
    pthread_cond_destroy(&s->wake);
    pthread_mutex_destroy(&s->mutex);
    timeline_history_close(s->history);
    free(s);
}
static int queue(App *a, JobKind kind, const char *path, uint64_t reference, char *out, size_t size)
{
    CastProjectSession *s = a->project;
    if (s->busy) {
        return fail(out, size, "project worker busy; retry after completion");
    }
    if (!path || !*path || strlen(path) >= sizeof s->job.path) {
        return fail(out, size, "project/media path is empty or too long");
    }
    pthread_mutex_lock(&s->mutex);
    memset(&s->job, 0, sizeof s->job);
    s->job.kind = kind;
    strcpy(s->job.path, path);
    s->job.config = s->job.original_config = a->config;
    if ((kind == JOB_SAVE || kind == JOB_AUTOSAVE) && *s->job.config.logo_path &&
        s->job.config.logo_path[0] != '/') {
        char resolved[PATH_MAX];
        int count = snprintf(resolved, sizeof resolved, "%s/%s", s->cwd, s->job.config.logo_path);
        if (count < 0 || (size_t)count >= sizeof resolved) {
            pthread_mutex_unlock(&s->mutex);
            return fail(out, size, "logo reference exceeds path bound");
        }
        strcpy(s->job.config.logo_path, resolved);
    }
    s->job.timeline = s->timeline;
    s->job.source_revision = s->revision;
    s->job.track = kind == JOB_IMPORT ? (unsigned)reference : 0;
    s->job.media_id = kind == JOB_RELINK ? reference : 0;
    s->busy = true;
    s->done = false;
    s->pending = true;
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->mutex);
    s->error[0] = 0;
    snprintf(s->status, sizeof s->status, "%s queued",
             kind == JOB_SAVE       ? "Project save"
             : kind == JOB_AUTOSAVE ? "Recovery autosave"
             : kind == JOB_OPEN     ? "Project open"
             : kind == JOB_RELINK   ? "Media relink"
                                    : "Media import");
    snprintf(out, size, "OK %s", s->status);
    return 0;
}
static void changed(CastProjectSession *s)
{
    review_clear(s);
    ++s->revision;
    s->dirty = true;
    s->open = true;
}
static void complete(App *a)
{
    CastProjectSession *s = a->project;
    pthread_mutex_lock(&s->mutex);
    bool done = s->done;
    s->done = false;
    pthread_mutex_unlock(&s->mutex);
    if (!done) {
        return;
    }
    ProjectJob *job = &s->job;
    if (job->result) {
        snprintf(s->error, sizeof s->error, "%.255s", job->error);
    } else if (job->kind == JOB_AUTOSAVE) {
        snprintf(s->status, sizeof s->status,
                 "Recovery snapshot saved; main project remains unsaved");
    } else if (job->kind == JOB_SAVE) {
        s->next_autosave_ns = cast_now_ns() + UINT64_C(30000000000);
        strcpy(s->path, job->path);
        s->open = true;
        if (s->revision == job->source_revision &&
            !memcmp(&a->config, &job->original_config, sizeof a->config)) {
            s->dirty = false;
        }
        snprintf(s->status, sizeof s->status, "Project saved%s",
                 s->dirty ? "; newer edits remain unsaved" : "");
    } else if (job->kind == JOB_OPEN) {
        if (!idle(a) || s->revision != job->source_revision ||
            memcmp(&a->config, &job->original_config, sizeof a->config)) {
            snprintf(s->error, sizeof s->error,
                     "Project open cancelled: outputs or settings changed while loading");
        } else if (app_apply_candidate(a, &job->config, job->error, sizeof job->error)) {
            snprintf(s->error, sizeof s->error, "%.255s", job->error);
        } else {
            review_clear(s);
            s->timeline = job->timeline;
            strcpy(s->path, job->path);
            s->open = true;
            s->dirty = false;
            ++s->revision;
            s->playhead_ns = 0;
            s->capture_clip = 0;
            s->observed = a->config;
            s->observed_valid = true;
            CastTimelineHistory *fresh = timeline_history_open(8);
            timeline_history_close(s->history);
            s->history = fresh;
            snprintf(s->status, sizeof s->status,
                     "Project opened; %u media and %u logo assets missing",
                     job->report.missing_media_count, job->report.missing_asset_count);
        }
    } else if (job->kind == JOB_RELINK) {
        if (!idle(a) || s->capture_clip || s->revision != job->source_revision ||
            memcmp(&a->config, &job->original_config, sizeof a->config)) {
            snprintf(s->error, sizeof s->error,
                     "Relink cancelled: document, outputs or settings changed");
        } else {
            CastTimeline *candidate = malloc(sizeof *candidate);
            if (!candidate) {
                snprintf(s->error, sizeof s->error, "Out of memory relinking media");
            } else {
                *candidate = s->timeline;
                CastTimelineMedia *media =
                    (CastTimelineMedia *)timeline_media(candidate, job->media_id);
                bool invalid = !media || media->kind != job->media_kind;
                for (unsigned i = 0; i < candidate->clip_count; ++i) {
                    const CastTimelineClip *clip = &candidate->clips[i];
                    if (clip->media_id == job->media_id &&
                        (clip->capture_locked ||
                         (job->media_kind != CAST_TIMELINE_IMAGE &&
                          clip->source_in_ns + clip->duration_ns > job->duration_ns))) {
                        invalid = true;
                    }
                }
                if (invalid) {
                    snprintf(s->error, sizeof s->error,
                             "Replacement kind/duration does not cover existing clips");
                } else {
                    snprintf(media->path, sizeof media->path, "%s", job->path);
                    media->duration_ns = job->duration_ns;
                    media->missing = false;
                    ++candidate->revision;
                    if (timeline_validate(candidate, job->error, sizeof job->error) ||
                        timeline_checkpoint(s->history, &s->timeline, job->error,
                                            sizeof job->error)) {
                        snprintf(s->error, sizeof s->error, "%.255s", job->error);
                    } else {
                        s->timeline = *candidate;
                        changed(s);
                        snprintf(s->status, sizeof s->status, "Relinked media %" PRIu64,
                                 job->media_id);
                    }
                }
                free(candidate);
            }
        }
    } else {
        CastTimeline *candidate = malloc(sizeof *candidate);
        if (!candidate) {
            snprintf(s->error, sizeof s->error, "Out of memory importing media");
        } else {
            *candidate = s->timeline;
            uint64_t media_id, clip_id;
            uint64_t duration = job->duration_ns ? job->duration_ns : UINT64_C(5000000000);
            if ((s->capture_clip && !job->track) ||
                timeline_add_media(candidate, job->path, job->media_kind, job->duration_ns,
                                   &media_id, job->error, sizeof job->error) ||
                timeline_add_clip(candidate, media_id, job->track, track_end(candidate, job->track),
                                  0, duration, &clip_id, job->error, sizeof job->error) ||
                timeline_checkpoint(s->history, &s->timeline, job->error, sizeof job->error)) {
                snprintf(s->error, sizeof s->error, "%.255s",
                         *job->error ? job->error
                                     : "Import to track 0 is unavailable while recording");
            } else {
                s->timeline = *candidate;
                changed(s);
                snprintf(s->status, sizeof s->status, "Imported media as clip %" PRIu64, clip_id);
            }
            free(candidate);
        }
    }
    if (*s->error) {
        snprintf(s->status, sizeof s->status, "Project operation failed");
    }
    s->busy = false;
}
static void recording(App *a)
{
    CastProjectSession *s = a->project;
    bool active = a->state.recording || a->record_finalizing ||
                  (a->media && media_record_finalizing(a->media));
    if (s->capture_clip && a->state.recording && s->capture_start != a->state.record_started_ns) {
        CastTimelineClip *previous =
            (CastTimelineClip *)timeline_clip(&s->timeline, s->capture_clip);
        if (previous) {
            previous->capture_locked = false;
        }
        s->capture_clip = 0;
        changed(s);
    }
    if (a->state.recording && !s->capture_clip && *a->state.record_path) {
        CastTimeline *candidate = malloc(sizeof *candidate);
        if (!candidate) {
            snprintf(s->error, sizeof s->error, "Out of memory adding recorded take");
            return;
        }
        *candidate = s->timeline;
        uint64_t media_id, clip_id;
        char error[CAST_ERR];
        uint64_t duration = a->media ? media_record_duration(a->media) : 0;
        if (!duration) {
            duration = 1;
        }
        uint64_t start = track_end(candidate, 0);
        char source_path[PATH_MAX];
        int count = snprintf(source_path, sizeof source_path, "%s%s%s",
                             a->state.record_path[0] == '/' ? "" : s->cwd,
                             a->state.record_path[0] == '/' ? "" : "/", a->state.record_path);
        if (count < 0 || (size_t)count >= sizeof source_path) {
            free(candidate);
            snprintf(s->error, sizeof s->error, "Captured media path exceeds bound");
            return;
        }
        if (timeline_add_media(candidate, source_path, CAST_TIMELINE_VIDEO, 0, &media_id, error,
                               sizeof error) ||
            timeline_add_clip(candidate, media_id, 0, start, 0, duration, &clip_id, error,
                              sizeof error)) {
            snprintf(s->error, sizeof s->error, "Cannot add captured take: %.220s", error);
        } else {
            CastTimelineClip *clip = (CastTimelineClip *)timeline_clip(candidate, clip_id);
            clip->capture_locked = true;
            s->timeline = *candidate;
            s->capture_clip = clip_id;
            s->capture_start = a->state.record_started_ns;
            changed(s);
            snprintf(s->status, sizeof s->status, "Recording flattened composition to track 0");
        }
        free(candidate);
    }
    if (!s->capture_clip) {
        return;
    }
    const CastTimelineClip *clip = timeline_clip(&s->timeline, s->capture_clip);
    if (!clip) {
        s->capture_clip = 0;
        return;
    }
    uint64_t duration = a->media ? media_record_duration(a->media) : clip->duration_ns;
    if (duration < clip->duration_ns) {
        duration = clip->duration_ns;
    }
    bool updated = false;
    if (duration != clip->duration_ns || clip->capture_locked != active) {
        char error[CAST_ERR];
        if (timeline_capture_update(&s->timeline, s->capture_clip, duration, active, error,
                                    sizeof error)) {
            snprintf(s->error, sizeof s->error, "Captured timeline update failed: %.215s", error);
        } else {
            updated = true;
        }
    }
    if (!active) {
        CastTimelineMedia *media =
            (CastTimelineMedia *)timeline_media(&s->timeline, clip->media_id);
        if (media && *a->state.record_path) {
            char final_path[PATH_MAX];
            int count = snprintf(final_path, sizeof final_path, "%s%s%s",
                                 a->state.record_path[0] == '/' ? "" : s->cwd,
                                 a->state.record_path[0] == '/' ? "" : "/", a->state.record_path);
            if (count >= 0 && (size_t)count < sizeof final_path) {
                strcpy(media->path, final_path);
            }
        }
        s->capture_clip = 0;
        updated = true;
        snprintf(s->status, sizeof s->status, "Captured flattened take finalized");
    }
    if (updated) {
        changed(s);
    }
}
void project_session_tick(App *a)
{
    if (!a || !a->project) {
        return;
    }
    CastProjectSession *s = a->project;
    if (s->retired_preview && timeline_preview_finished(s->retired_preview)) {
        timeline_preview_close(s->retired_preview);
        s->retired_preview = NULL;
    }
    if (s->retired_audio && timeline_audio_finished(s->retired_audio)) {
        timeline_audio_close(s->retired_audio);
        s->retired_audio = NULL;
    }
    if (a->state.group_paused && s->audio_requested) {
        project_session_silence(s);
    }
    if (s->audio && s->audio_requested) {
        CastTimelineAudioStatus audio;
        timeline_audio_status(s->audio, &audio);
        s->audio_running = audio.running;
        snprintf(s->audio_error, sizeof s->audio_error, "%s", audio.error);
        s->audio_requested = audio.requested;
    }
    if (!s->observed_valid) {
        s->observed = a->config;
        s->observed_valid = true;
    } else if (memcmp(&s->observed, &a->config, sizeof a->config)) {
        s->observed = a->config;
        if (s->open) {
            changed(s);
        }
    }
    if (!idle(a)) {
        review_clear(s);
    }
    complete(a);
    recording(a);
    if (s->reviewing && s->preview) {
        uint64_t clock = transport_now(s), duration = timeline_duration(&s->timeline);
        if (s->playing) {
            uint64_t elapsed = clock >= s->play_started_ns ? clock - s->play_started_ns : 0;
            uint64_t remaining = duration - s->play_started_position;
            s->playhead_ns = elapsed >= remaining ? duration : s->play_started_position + elapsed;
            s->transport_pending = true;
            if (s->playhead_ns == duration) {
                s->playing = false;
                project_session_silence(s);
            }
        }
        CastTimelinePreviewSnapshot view;
        timeline_preview_snapshot(s->preview, &view);
        s->preview_busy = view.busy;
        snprintf(s->preview_error, sizeof s->preview_error, "%s", view.error);
        if (view.ready && view.completed_generation == s->seek_generation &&
            view.completed_generation != s->copied_generation) {
            uint64_t generation = 0;
            if (!timeline_preview_copy(s->preview, &s->review_frame, &generation)) {
                s->copied_generation = generation;
            } else {
                snprintf(s->preview_error, sizeof s->preview_error,
                         "Cannot copy timeline review frame");
            }
        }
        if (*view.error) {
            s->playing = s->transport_pending = false;
        }
        if (s->transport_pending && !view.busy && (clock >= s->next_video_ns || !s->playing)) {
            uint64_t target = s->playhead_ns;
            if (target == duration && target) {
                --target;
            }
            if (!timeline_preview_request(s->preview, target, ++s->seek_generation)) {
                s->preview_busy = true;
                s->transport_pending = false;
                unsigned fps = a->config.fps > 30 ? 30 : (unsigned)a->config.fps;
                s->next_video_ns = clock + UINT64_C(1000000000) / fps;
            } else {
                s->playing = s->transport_pending = false;
                snprintf(s->preview_error, sizeof s->preview_error, "Cannot queue playback frame");
            }
        }
    }
    uint64_t now = cast_now_ns();
    if (!s->busy && s->dirty && *s->path && now >= s->next_autosave_ns) {
        s->next_autosave_ns = now + UINT64_C(30000000000);
        char recovery[PATH_MAX], reply[CAST_ERR];
        int count = snprintf(recovery, sizeof recovery, "%s.autosave", s->path);
        if (count >= 0 && (size_t)count < sizeof recovery) {
            (void)queue(a, JOB_AUTOSAVE, recovery, 0, reply, sizeof reply);
        } else {
            snprintf(s->error, sizeof s->error, "Project path too long for recovery sibling");
        }
    }
}
void project_session_snapshot(CastProjectSession *s, CastProjectSnapshot *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (!s) {
        return;
    }
    if (s->export) {
        timeline_export_snapshot(s->export, &s->export_snapshot);
    }
    out->export = s->export_snapshot;
    out->exporting = s->export && s->export_snapshot.state < CAST_TIMELINE_EXPORT_COMPLETE;
    out->playback_audio_requested = s->audio_requested;
    out->playback_audio_running = s->audio_running;
    snprintf(out->playback_audio_error, sizeof out->playback_audio_error, "%s", s->audio_error);
    out->playing = s->playing;
    out->reviewing = s->reviewing;
    out->preview_busy = s->preview_busy;
    snprintf(out->preview_error, sizeof out->preview_error, "%s", s->preview_error);
    out->open = s->open;
    out->busy = s->busy;
    out->dirty = s->dirty;
    out->recording = s->capture_clip != 0;
    out->revision = s->revision;
    out->duration_ns = timeline_duration(&s->timeline);
    out->playhead_ns = s->playhead_ns;
    out->clip_count = s->timeline.clip_count;
    out->media_count = s->timeline.media_count;
    strcpy(out->path, s->path);
    const char *name = strrchr(s->path, '/');
    name = name ? name + 1 : s->path;
    snprintf(out->name, sizeof out->name, "%.127s", *name ? name : "Untitled project");
    strcpy(out->status, s->status);
    strcpy(out->error, s->error);
    memcpy(out->clips, s->timeline.clips, s->timeline.clip_count * sizeof *out->clips);
    for (unsigned i = 0; i < s->timeline.media_count; ++i) {
        const CastTimelineMedia *m = &s->timeline.media[i];
        CastProjectMediaView *view = &out->media[i];
        view->id = m->id;
        view->kind = m->kind;
        view->missing = m->missing;
        out->missing_count += m->missing;
        name = strrchr(m->path, '/');
        name = name ? name + 1 : m->path;
        snprintf(view->label, sizeof view->label, "%.95s", name);
    }
}
static bool reserves_recording(const CastProjectSession *s, const CastTimeline *candidate)
{
    if (!s->capture_clip) {
        return false;
    }
    const CastTimelineClip *active = timeline_clip(candidate, s->capture_clip);
    if (!active) {
        return true;
    }
    for (unsigned i = 0; i < candidate->clip_count; ++i) {
        const CastTimelineClip *clip = &candidate->clips[i];
        if (clip->id != active->id && clip->track == active->track &&
            clip->start_ns >= active->start_ns) {
            return true;
        }
    }
    return false;
}
int project_session_command(App *a, int argc, char **argv, char *out, size_t size)
{
    if (!a || !a->project || argc < 2) {
        return fail(out, size, "project/timeline command required");
    }
    CastProjectSession *s = a->project;
    if (out && size) {
        snprintf(out, size, "invalid timeline edit arguments");
    }
    if (!strcmp(argv[0], "project")) {
        if (!strcmp(argv[1], "status") && argc == 2) {
            snprintf(out, size, "OK %s; revision=%" PRIu64 " clips=%u dirty=%d busy=%d", s->status,
                     s->revision, s->timeline.clip_count, s->dirty, s->busy);
            return 0;
        }
        if (!strcmp(argv[1], "save") && (argc == 2 || argc == 3)) {
            return queue(a, JOB_SAVE, argc == 3 ? argv[2] : s->path, 0, out, size);
        }
        if ((!strcmp(argv[1], "open") || !strcmp(argv[1], "recover")) && argc == 3) {
            if (!idle(a)) {
                return fail(out, size,
                            "stop recording/finalization and streaming before opening a project");
            }
            review_clear(s);
            return queue(a, JOB_OPEN, argv[2], 0, out, size);
        }
        if (!strcmp(argv[1], "new") && argc == 2) {
            if (s->busy || !idle(a)) {
                return fail(out, size, "new project requires idle outputs and worker");
            }
            timeline_init(&s->timeline);
            CastTimelineHistory *fresh = timeline_history_open(8);
            timeline_history_close(s->history);
            s->history = fresh;
            s->path[0] = s->error[0] = 0;
            s->playhead_ns = 0;
            s->capture_clip = 0;
            changed(s);
            snprintf(s->status, sizeof s->status, "New project; current composition retained");
            snprintf(out, size, "OK %s", s->status);
            return 0;
        }
        return fail(out, size, "project new|open PATH|save [PATH]|recover PATH|status");
    }
    if (strcmp(argv[0], "timeline")) {
        return fail(out, size, "unknown project command");
    }
    if (!strcmp(argv[1], "export")) {
        if (s->export) {
            timeline_export_snapshot(s->export, &s->export_snapshot);
        }
        bool running = s->export && s->export_snapshot.state < CAST_TIMELINE_EXPORT_COMPLETE;
        if (argc == 3 && !strcmp(argv[2], "status")) {
            snprintf(out, size,
                     "OK export state=%d completed_ns=%" PRIu64 " duration_ns=%" PRIu64 " error=%s",
                     s->export_snapshot.state, s->export_snapshot.completed_ns,
                     s->export_snapshot.duration_ns, s->export_snapshot.error);
            return 0;
        }
        if (argc == 3 && !strcmp(argv[2], "cancel")) {
            if (!running) {
                return fail(out, size, "no active timeline export");
            }
            timeline_export_cancel(s->export);
            snprintf(out, size, "OK export cancellation requested");
            return 0;
        }
        if (argc != 3) {
            return fail(out, size, "timeline export PATH|status|cancel");
        }
        if (running || s->busy) {
            return fail(out, size, "wait for current export/project worker");
        }
        for (unsigned i = 0; i < s->timeline.clip_count; ++i) {
            const CastTimelineClip *clip = &s->timeline.clips[i];
            const CastTimelineMedia *media = timeline_media(&s->timeline, clip->media_id);
            if (clip->capture_locked || !media || media->missing) {
                return fail(out, size, "clip %" PRIu64 " has locked or missing media", clip->id);
            }
        }
        if (!s->timeline.clip_count) {
            return fail(out, size, "timeline is empty");
        }
        char path[PATH_MAX];
        int length = snprintf(path, sizeof path, "%s%s%s", argv[2][0] == '/' ? "" : s->cwd,
                              argv[2][0] == '/' ? "" : "/", argv[2]);
        if (!*argv[2] || length < 0 || (size_t)length >= sizeof path) {
            return fail(out, size, "invalid export path");
        }
        timeline_export_close(s->export);
        s->export = NULL;
        s->export = timeline_export_start_noreplace(&s->timeline, &a->config, path, out, size);
        if (!s->export) {
            return -1;
        }
        timeline_export_snapshot(s->export, &s->export_snapshot);
        snprintf(out, size, "OK timeline export queued");
        return 0;
    }
    if (s->busy && s->job.kind != JOB_SAVE && s->job.kind != JOB_AUTOSAVE) {
        return fail(out, size, "timeline changes wait for project open/import/relink completion");
    }
    if (!strcmp(argv[1], "relink") && argc == 4) {
        uint64_t id;
        if (!idle(a) || s->capture_clip) {
            return fail(out, size, "media relink requires idle outputs");
        }
        if (unsigned_number(argv[2], &id) || !timeline_media(&s->timeline, id)) {
            return fail(out, size, "unknown media identifier");
        }
        return queue(a, JOB_RELINK, argv[3], id, out, size);
    }
    if (!strcmp(argv[1], "import") && (argc == 3 || argc == 4)) {
        uint64_t track = 0;
        if (argc == 4 && (unsigned_number(argv[3], &track) || track >= CAST_TIMELINE_TRACK_MAX)) {
            return fail(out, size, "invalid import track");
        }
        if (s->capture_clip && !track) {
            return fail(out, size, "import on a different track while recording");
        }
        return queue(a, JOB_IMPORT, argv[2], (unsigned)track, out, size);
    }
    if (!strcmp(argv[1], "pause") && argc == 2) {
        s->playing = false;
        project_session_silence(s);
        snprintf(out, size, "OK review paused");
        return 0;
    }
    if (!strcmp(argv[1], "play") && (argc == 2 || argc == 3)) {
        bool silent = argc == 3;
        if (silent && strcmp(argv[2], "silent")) {
            return fail(out, size, "timeline play [silent]");
        }
        if (!idle(a) || s->capture_clip) {
            return fail(out, size, "timeline playback requires idle outputs");
        }
        uint64_t duration = timeline_duration(&s->timeline);
        if (!duration) {
            return fail(out, size, "timeline is empty");
        }
        uint64_t position = s->playhead_ns >= duration ? 0 : s->playhead_ns;
        if (!s->reviewing || !s->preview || position != s->playhead_ns) {
            char text[32];
            snprintf(text, sizeof text, "%" PRIu64, position);
            char *seek[] = {"timeline", "seek", text};
            if (project_session_command(a, 3, seek, out, size)) {
                return -1;
            }
        }
        s->playing = true;
        s->transport_pending = true;
        s->play_started_ns = transport_now(s);
        s->play_started_position = position;
        s->next_video_ns = s->play_started_ns;
        if (silent) {
            project_session_silence(s);
            s->audio_error[0] = 0;
        } else if (!a->state.group_paused) {
            if (!s->audio && s->retired_audio && timeline_audio_finished(s->retired_audio)) {
                timeline_audio_close(s->retired_audio);
                s->retired_audio = NULL;
            }
            if (!s->audio && !s->retired_audio) {
                s->audio = timeline_audio_native_open(&s->timeline, &a->config);
            }
            s->audio_error[0] = 0;
            if (s->audio) {
                timeline_audio_play(s->audio, position, s->play_started_ns);
                s->audio_requested = true;
            } else {
                snprintf(s->audio_error, sizeof s->audio_error,
                         "Playback audio unavailable; video review continues");
            }
        } else {
            project_session_silence(s);
            snprintf(s->audio_error, sizeof s->audio_error,
                     "Playback audio blocked by global privacy; use play after resuming");
        }
        snprintf(out, size, "OK video review playing%s",
                 silent ? " silently" : "; audio output requested");
        return 0;
    }
    if (!strcmp(argv[1], "live") && argc == 2) {
        review_clear(s);
        snprintf(out, size, "OK live composition");
        return 0;
    }
    if (!strcmp(argv[1], "seek") && argc == 3) {
        if (!idle(a) || s->capture_clip) {
            return fail(out, size, "timeline review requires idle outputs");
        }
        uint64_t position;
        if (unsigned_number(argv[2], &position) || position > timeline_duration(&s->timeline)) {
            return fail(out, size, "seek exceeds timeline duration");
        }
        if (!s->preview) {
            if (s->retired_preview) {
                if (!timeline_preview_finished(s->retired_preview)) {
                    return fail(out, size, "previous preview cancelling; retry");
                }
                timeline_preview_close(s->retired_preview);
                s->retired_preview = NULL;
            }
            Config preview = a->config;
            double scale = fmin(1., fmin(1920. / preview.width, 1080. / preview.height));
            preview.width = (int)(preview.width * scale);
            preview.height = (int)(preview.height * scale);
            if (preview.width < 64) {
                preview.width = 64;
            }
            if (preview.height < 64) {
                preview.height = 64;
            }
            if (preview.width % 2) {
                --preview.width;
            }
            if (preview.height % 2) {
                --preview.height;
            }
            s->preview = timeline_preview_open(&s->timeline, &preview, out, size);
            if (!s->preview) {
                return -1;
            }
        }
        if (timeline_preview_request(s->preview, position, ++s->seek_generation)) {
            return fail(out, size, "cannot queue timeline review");
        }
        s->reviewing = true;
        s->preview_busy = true;
        s->preview_error[0] = 0;
        s->playing = s->transport_pending = false;
        project_session_silence(s);
        s->playhead_ns = position;
        snprintf(out, size, "OK playhead=%" PRIu64, position);
        return 0;
    }
    if ((!strcmp(argv[1], "undo") || !strcmp(argv[1], "redo")) && argc == 2) {
        int result = !strcmp(argv[1], "undo") ? timeline_undo(s->history, &s->timeline, out, size)
                                              : timeline_redo(s->history, &s->timeline, out, size);
        if (!result) {
            changed(s);
            snprintf(out, size, "OK timeline %s", argv[1]);
        }
        return result;
    }
    if (argc < 3) {
        return fail(out, size, "timeline import|move|trim|trim-left|split|remove|gain|undo|redo|seek");
    }
    uint64_t id, first = 0, second = 0, new_id = 0;
    if (unsigned_number(argv[2], &id)) {
        return fail(out, size, "invalid clip identifier");
    }
    CastTimeline *candidate = malloc(sizeof *candidate);
    if (!candidate) {
        return fail(out, size, "out of memory editing timeline");
    }
    *candidate = s->timeline;
    int result = -1;
    if (!strcmp(argv[1], "remove") && argc == 3) {
        result = timeline_remove(candidate, id, out, size);
    } else if (!strcmp(argv[1], "split") && argc == 4 && !unsigned_number(argv[3], &first)) {
        result = timeline_split(candidate, id, first, &new_id, out, size);
    } else if (!strcmp(argv[1], "trim") && argc == 4 && !pair(argv[3], &first, &second)) {
        result = timeline_trim(candidate, id, first, second, out, size);
    } else if (!strcmp(argv[1], "trim-left") && argc == 4 && !unsigned_number(argv[3], &first)) {
        result = timeline_trim_left(candidate, id, first, out, size);
    } else if (!strcmp(argv[1], "move") && argc == 4 && !pair(argv[3], &first, &second) &&
               first < CAST_TIMELINE_TRACK_MAX) {
        result = timeline_move(candidate, id, (unsigned)first, second, out, size);
    } else if (!strcmp(argv[1], "gain") && argc == 4 && strlen(argv[3]) < 128) {
        char value[128];
        strcpy(value, argv[3]);
        char *comma = strchr(value, ',');
        bool muted = false;
        if (comma) {
            *comma++ = 0;
            if (unsigned_number(comma, &first) || first > 1) {
                goto finished;
            }
            muted = first != 0;
        }
        char *end;
        errno = 0;
        float gain = strtof(value, &end);
        if (!errno && end != value && !*end && isfinite(gain)) {
            result = timeline_gain(candidate, id, gain, muted, out, size);
        }
    } else {
        fail(out, size, "invalid timeline edit arguments");
    }
    if (!result && reserves_recording(s, candidate)) {
        result = fail(out, size, "track 0 after the active take is reserved until finalization");
    }
    if (!result && timeline_checkpoint(s->history, &s->timeline, out, size)) {
        result = -1;
    }
    if (!result) {
        s->timeline = *candidate;
        changed(s);
        s->error[0] = 0;
        snprintf(out, size, "OK timeline %s%s", argv[1], new_id ? "; split clip added" : "");
    }
finished:
    free(candidate);
    return result;
}

#ifdef CAST_TEST
void project_session_test_autosave_due(CastProjectSession *);
void project_session_test_autosave_due(CastProjectSession *s)
{
    if (s) {
        s->next_autosave_ns = 0;
    }
}
#endif
