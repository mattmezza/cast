#include "app_internal.h"
#include "project_session.h"
#include "timeline_audio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Real codec/FFmpeg probe/worker with isolated application handoff stubs. */
/* Session integration never opens a native output in unit tests. */
CastTimelineAudio *timeline_audio_native_open(const CastTimeline *timeline, const Config *config)
{
    (void)timeline;
    (void)config;
    return NULL;
}
static uint64_t duration;
static bool finalizing;
static unsigned applied;
#ifdef CAST_TEST
void project_session_test_autosave_due(CastProjectSession *);
void project_session_test_clock(CastProjectSession *, uint64_t);
#endif
uint64_t media_record_duration(Media *m)
{
    (void)m;
    return duration;
}
bool media_record_finalizing(Media *m)
{
    (void)m;
    return finalizing;
}
int app_apply_candidate(App *app, Config *config, char *error, size_t size)
{
    (void)error;
    (void)size;
    app->config = *config;
    ++applied;
    return 0;
}
static int command(App *app, int count, const char *const *args)
{
    char output[CAST_ERR];
    return project_session_command(app, count, (char **)args, output, sizeof output);
}
static void seek_review(App *app)
{
    const char *args[] = {"timeline", "seek", "100000000"};
    for (unsigned i = 0; i < 5000; ++i) {
        if (!command(app, 3, args)) {
            return;
        }
        project_session_tick(app);
        usleep(1000);
    }
    assert(!"preview cancellation did not finish");
}
static CastProjectSnapshot snapshot(App *a)
{
    CastProjectSnapshot s;
    project_session_snapshot(a->project, &s);
    return s;
}
static void wait_done(App *a)
{
    for (unsigned i = 0; i < 5000; ++i) {
        project_session_tick(a);
        if (!snapshot(a).busy) {
            return;
        }
        usleep(1000);
    }
    assert(!"project worker deadline exceeded");
}
static void little(FILE *file, unsigned value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) {
        assert(fputc((int)((value >> (8 * i)) & 255), file) != EOF);
    }
}
static void wav(const char *path)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    fputs("RIFF", f);
    little(f, 32036, 4);
    fputs("WAVEfmt ", f);
    little(f, 16, 4);
    little(f, 1, 2);
    little(f, 1, 2);
    little(f, 16000, 4);
    little(f, 32000, 4);
    little(f, 2, 2);
    little(f, 16, 2);
    fputs("data", f);
    little(f, 32000, 4);
    for (unsigned i = 0; i < 32000; ++i) {
        fputc(0, f);
    }
    assert(!fclose(f));
}
int main(void)
{
    char folder[] = "/tmp/cast-project-session-XXXXXX";
    assert(mkdtemp(folder));
    char project[PATH_MAX], audio[PATH_MAX], bad[PATH_MAX], capture[PATH_MAX];
    snprintf(project, sizeof project, "%s/test.cast", folder);
    snprintf(audio, sizeof audio, "%s/audio.wav", folder);
    snprintf(bad, sizeof bad, "%s/bad.m3u", folder);
    snprintf(capture, sizeof capture, "%s/capture.partial", folder);
    wav(audio);
    FILE *file = fopen(bad, "w");
    assert(file);
    fputs("#EXTM3U\nhttps://example.invalid/stream\n", file);
    fclose(file);
    App *a = calloc(1, sizeof *a);
    assert(a);
    config_defaults(&a->config);
    a->project = project_session_open();
    assert(a->project);
    project_session_tick(a);
    const char *new_project[] = {"project", "new"};
    assert(!command(a, 2, new_project));
    assert(snapshot(a).open && snapshot(a).dirty);
    const char *import[] = {"timeline", "import", audio, "1"};
    assert(!command(a, 4, import));
    assert(snapshot(a).busy);
    assert(command(a, 4, import));
    wait_done(a);
    CastProjectSnapshot s = snapshot(a);
    assert(s.clip_count == 1 && s.media_count == 1 && !*s.error);
    assert(s.clips[0].track == 1 && s.clips[0].duration_ns == UINT64_C(1000000000));
    a->config.width = 64;
    a->config.height = 64;
    a->config.fps = 10;
    snprintf(a->config.video_codec, sizeof a->config.video_codec, "ffv1");
    snprintf(a->config.audio_codec, sizeof a->config.audio_codec, "flac");
    snprintf(a->config.record_container, sizeof a->config.record_container, "matroska");
    char movie[PATH_MAX];
    snprintf(movie, sizeof movie, "%s/export.mkv", folder);
    const char *export_command[] = {"timeline", "export", movie};
    const char *export_status[] = {"timeline", "export", "status"};
    assert(!command(a, 3, export_command));
    assert(!command(a, 3, export_status));
    for (unsigned i = 0; i < 5000 && snapshot(a).exporting; ++i) {
        usleep(1000);
    }
    assert(snapshot(a).export.state == CAST_TIMELINE_EXPORT_COMPLETE);
    assert(!access(movie, R_OK));
    assert(!command(a, 3, export_command));
    for (unsigned i = 0; i < 5000 && snapshot(a).exporting; ++i) {
        usleep(1000);
    }
    assert(snapshot(a).export.state == CAST_TIMELINE_EXPORT_FAILED);
    assert(!unlink(movie));
    assert(!command(a, 3, export_command));
    const char *cancel_export[] = {"timeline", "export", "cancel"};
    assert(!command(a, 3, cancel_export));
    for (unsigned i = 0; i < 5000 && snapshot(a).exporting; ++i) {
        usleep(1000);
    }
    assert(snapshot(a).export.state == CAST_TIMELINE_EXPORT_CANCELLED);
    assert(access(movie, F_OK));
    project_session_tick(a);
    const char *seek[] = {"timeline", "seek", "100000000"};
    const char *live[] = {"timeline", "live"};
    seek_review(a);
    assert(snapshot(a).reviewing);
    for (unsigned i = 0; i < 5000; ++i) {
        project_session_tick(a);
        if (!snapshot(a).preview_busy) {
            break;
        }
        usleep(1000);
    }
    assert(project_session_review_frame(a->project));
#ifdef CAST_TEST
    const char *play[] = {"timeline", "play"}, *pause[] = {"timeline", "pause"};
    project_session_test_clock(a->project, UINT64_C(1000000000));
    assert(!command(a, 2, play));
    assert(snapshot(a).playing);
    assert(!snapshot(a).playback_audio_running && *snapshot(a).playback_audio_error);
    const char *silent[] = {"timeline", "play", "silent"},
               *bad_mode[] = {"timeline", "play", "loud"};
    assert(command(a, 3, bad_mode));
    assert(!command(a, 3, silent));
    assert(!snapshot(a).playback_audio_requested && !*snapshot(a).playback_audio_error);
    project_session_test_clock(a->project, UINT64_C(1250000000));
    project_session_tick(a);
    assert(snapshot(a).playhead_ns == UINT64_C(350000000));
    assert(!command(a, 2, pause));
    assert(!snapshot(a).playing && snapshot(a).reviewing);
    project_session_test_clock(a->project, UINT64_C(2000000000));
    project_session_tick(a);
    assert(snapshot(a).playhead_ns == UINT64_C(350000000));
    assert(!command(a, 2, play));
    project_session_test_clock(a->project, UINT64_C(4000000000));
    project_session_tick(a);
    assert(!snapshot(a).playing && snapshot(a).playhead_ns == UINT64_C(1000000000));
    for (unsigned i = 0; i < 5000; ++i) {
        project_session_tick(a);
        const Frame *last = project_session_review_frame(a->project);
        if (!snapshot(a).preview_busy && last && last->ts_ns == UINT64_C(999999999)) {
            break;
        }
        usleep(1000);
    }
    assert(project_session_review_frame(a->project)->ts_ns == UINT64_C(999999999));
#endif
    const Frame *acknowledged = project_session_review_frame(a->project);
    seek_review(a);
    assert(project_session_review_frame(a->project) == acknowledged);
    assert(!command(a, 2, live));
    assert(!snapshot(a).reviewing && !project_session_review_frame(a->project));
    seek_review(a);
    a->state.stream_active = true;
    project_session_tick(a);
    assert(!snapshot(a).reviewing && !project_session_review_frame(a->project));
    assert(command(a, 3, seek));
    a->state.stream_active = false;
    seek_review(a);
    a->config.mic_gain += .1;
    project_session_tick(a);
    assert(!snapshot(a).reviewing && !project_session_review_frame(a->project));
    char clip[32];
    snprintf(clip, sizeof clip, "%llu", (unsigned long long)s.clips[0].id);
    const char *gain[] = {"timeline", "gain", clip, "0.5,1"};
    assert(!command(a, 4, gain));
    assert(snapshot(a).clips[0].gain == .5f && snapshot(a).clips[0].muted);
    const char *save[] = {"project", "save", project};
    assert(!command(a, 3, save));
    gain[3] = "0.75,0";
    assert(!command(a, 4, gain));
    wait_done(a);
    assert(snapshot(a).dirty);
    assert(!command(a, 3, save));
    wait_done(a);
    assert(!snapshot(a).dirty);
    const char *open[] = {"project", "open", project};
    assert(!command(a, 3, open));
    a->config.mic_gain = 1.5;
    wait_done(a);
    assert(!applied && *snapshot(a).error);
    assert(!command(a, 3, open));
    wait_done(a);
    assert(applied == 1 && !*snapshot(a).error);
    a->state.stream_active = true;
    assert(command(a, 3, open));
    a->state.stream_active = false;
    const char *bad_import[] = {"timeline", "import", bad, "1"};
    assert(!command(a, 4, bad_import));
    wait_done(a);
    assert(*snapshot(a).error && snapshot(a).clip_count == 1);
    const char *network[] = {"timeline", "import", "https://example.invalid/stream", "1"};
    assert(!command(a, 4, network));
    wait_done(a);
    assert(*snapshot(a).error);
    /* Tick attaches one flattened take, protects it through finalization, and
     * continues to accept edits on the imported audio track. */
    a->media = (Media *)1;
    a->state.recording = true;
    a->state.record_started_ns = 100;
    strcpy(a->state.record_path, capture);
    duration = UINT64_C(2000000000);
    project_session_tick(a);
    s = snapshot(a);
    assert(s.recording && s.clip_count == 2 && s.clips[1].capture_locked);
    assert(command(a, 3, export_command));
    char captured_id[32];
    snprintf(captured_id, sizeof captured_id, "%llu", (unsigned long long)s.clips[1].id);
    const char *remove[] = {"timeline", "remove", captured_id};
    assert(command(a, 3, remove));
    assert(!command(a, 4, gain));
    assert(command(a, 3, open));
    a->state.recording = false;
    finalizing = true;
    project_session_tick(a);
    assert(snapshot(a).clips[1].capture_locked);
    finalizing = false;
    strcpy(a->state.record_path, audio);
    project_session_tick(a);
    s = snapshot(a);
    assert(!s.recording && !s.clips[1].capture_locked && !strcmp(s.media[1].label, "audio.wav"));
    assert(!command(a, 3, remove));
    assert(snapshot(a).clip_count == 1);
    const char *undo[] = {"timeline", "undo"};
    assert(!command(a, 2, undo));
    assert(snapshot(a).clip_count == 2);
    const char *redo[] = {"timeline", "redo"};
    assert(!command(a, 2, redo));
    assert(snapshot(a).clip_count == 1);
#ifdef CAST_TEST
    /* Automatic recovery writes a sibling and never marks the main project saved. */
    char recovery[PATH_MAX];
    snprintf(recovery, sizeof recovery, "%s/test.cast.autosave", folder);
    project_session_test_autosave_due(a->project);
    project_session_tick(a);
    assert(snapshot(a).busy);
    wait_done(a);
    s = snapshot(a);
    assert(s.dirty && !strcmp(s.path, project));
    struct stat recovery_st;
    assert(!stat(recovery, &recovery_st));
    assert((recovery_st.st_mode & 0777) == 0600);
    assert(!unlink(recovery));
    assert(!symlink(project, recovery));
    project_session_test_autosave_due(a->project);
    project_session_tick(a);
    wait_done(a);
    assert(*snapshot(a).error && snapshot(a).dirty && !strcmp(snapshot(a).path, project));
    assert(!lstat(recovery, &recovery_st) && S_ISLNK(recovery_st.st_mode));
    assert(!unlink(recovery));
    project_session_test_autosave_due(a->project);
    project_session_tick(a);
    wait_done(a);
    const char *recover[] = {"project", "recover", recovery};
    assert(!command(a, 3, recover));
    wait_done(a);
    assert(!snapshot(a).dirty && !strcmp(snapshot(a).path, recovery));
    assert(!unlink(recovery));
#endif
    /* Opening a moved project reports missing references; relink retains edits. */
    char moved[PATH_MAX], short_media[PATH_MAX], media_id[32];
    snprintf(moved, sizeof moved, "%s/moved.wav", folder);
    snprintf(short_media, sizeof short_media, "%s/short.wav", folder);
    assert(!rename(audio, moved));
    assert(!command(a, 3, open));
    wait_done(a);
    s = snapshot(a);
    assert(s.missing_count == 1 && s.media[0].missing);
    snprintf(media_id, sizeof media_id, "%llu", (unsigned long long)s.media[0].id);
    wav(short_media);
    FILE *short_file = fopen(short_media, "r+b");
    assert(short_file);
    assert(!ftruncate(fileno(short_file), 3244));
    assert(!fseek(short_file, 4, SEEK_SET));
    little(short_file, 3236, 4);
    assert(!fseek(short_file, 40, SEEK_SET));
    little(short_file, 3200, 4);
    assert(!fclose(short_file));
    const char *relink[] = {"timeline", "relink", media_id, short_media};
    uint64_t before_relink = s.revision;
    CastTimelineClip original_clip = s.clips[0];
    assert(!command(a, 4, relink));
    wait_done(a);
    assert(*snapshot(a).error && snapshot(a).revision == before_relink &&
           snapshot(a).missing_count == 1);
    relink[3] = moved;
    assert(!command(a, 4, relink));
    a->config.mic_gain += .1;
    wait_done(a);
    assert(*snapshot(a).error && snapshot(a).missing_count == 1);
    assert(!command(a, 4, relink));
    wait_done(a);
    s = snapshot(a);
    assert(!*s.error && s.dirty && !s.missing_count && !strcmp(s.media[0].label, "moved.wav"));
    assert(!memcmp(&original_clip, &s.clips[0], sizeof original_clip));
    assert(!command(a, 2, undo));
    assert(snapshot(a).missing_count == 1);
    assert(!command(a, 2, redo));
    assert(!snapshot(a).missing_count);
    a->state.stream_active = true;
    assert(command(a, 4, relink));
    a->state.stream_active = false;
    assert(!rename(moved, audio));
    assert(!unlink(short_media));
    project_session_close(a->project);
    free(a);
    assert(!unlink(project));
    assert(!unlink(audio));
    assert(!unlink(bad));
    assert(!rmdir(folder));
    puts("Project session worker, probe restrictions, stale opens, save revisions and capture "
         "locks passed");
    return 0;
}
