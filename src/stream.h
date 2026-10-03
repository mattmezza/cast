#ifndef CAST_STREAM_H
#define CAST_STREAM_H
#include "cast.h"
typedef enum {
    STREAM_STOPPED,
    STREAM_CONNECTING,
    STREAM_STREAMING,
    STREAM_RECONNECTING,
    STREAM_STOPPING,
    STREAM_FAILED
} StreamLifecycle;
typedef struct StreamSnapshot {
    StreamLifecycle state;
    bool active;
    uint64_t generation, session_started_ns, connected_started_ns;
    uint64_t bytes_written, video_frames, audio_samples, dropped_frames;
    unsigned queue_depth, retry_attempt;
    double outgoing_bitrate_kbps;
    uint64_t next_retry_ns;
    char error[CAST_ERR];
} StreamSnapshot;
typedef struct CastStream CastStream;
typedef struct CastAudio CastAudio;
CastStream *stream_open(CastAudio *);
void stream_close(CastStream *);
int stream_start(CastStream *, const Config *, char *, size_t);
int stream_stop(CastStream *, char *, size_t);
int stream_frame(CastStream *, const Frame *, bool, char *, size_t);
void stream_privacy(CastStream *, bool);
void stream_barrier(CastStream *);
void stream_status(CastStream *, StreamSnapshot *);
void stream_doctor(const Config *, char *, size_t);
bool stream_settings_changed(const StreamConfig *, const StreamConfig *);
const char *stream_lifecycle_name(StreamLifecycle);
/* Dispatch before opening any devices or starting threads. fd is inherited via posix_spawn. */
int stream_worker_main(int fd);
/* Read-only validation never opens a network connection. No secret appears in diagnostics. */
int stream_key_validate(const char *, char *, size_t);
#ifdef CAST_TEST
void stream_test_hold(CastStream *, int);
void stream_test_stage(CastStream *, int);
#endif
#endif
