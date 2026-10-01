#ifndef MEDIA_INTERNAL_H
#define MEDIA_INTERNAL_H
#include "cast.h"
#include <stdio.h>
typedef struct CastCamera CastCamera;
typedef struct CastOutput CastOutput;
typedef struct CastAudio CastAudio;
typedef struct CastRecorder CastRecorder;
CastCamera *camera_open(const Config *, char *, size_t);
void camera_close(CastCamera *);
void camera_barrier(CastCamera *);
bool camera_failed(CastCamera *);
int camera_frame(CastCamera *, Frame *, char *, size_t);
void camera_list(char *, size_t);
CastOutput *output_open(const Config *, char *, size_t);
void output_close(CastOutput *);
int output_frame(CastOutput *, const Frame *, char *, size_t);
void camera_doctor(const Config *, char *, size_t);
CastAudio *audio_open(const Config *, char *, size_t);
void audio_close(CastAudio *);
int audio_validate(CastAudio *, const Config *, char *, size_t);
int audio_configure(CastAudio *, const Config *, char *, size_t);
void audio_barrier(CastAudio *, bool);
void audio_read(CastAudio *, uint64_t, float *, int);
void audio_list(CastAudio *, char *, size_t);
void audio_status(CastAudio *, char *, size_t);
void audio_error(CastAudio *, char *, size_t);
CastRecorder *recorder_open(CastAudio *);
void recorder_close(CastRecorder *);
int recorder_start(CastRecorder *, const Config *, const char *, char *, size_t);
int recorder_stop(CastRecorder *, char *, size_t);
bool recorder_finalizing(CastRecorder *);
int recorder_pause(CastRecorder *, bool, char *, size_t);
int recorder_frame(CastRecorder *, const Frame *, char *, size_t);
void recorder_barrier(CastRecorder *);
void recorder_status(CastRecorder *, bool *, bool *, uint64_t *, char *, size_t);
uint64_t recorder_duration(CastRecorder *);
void recorder_path(CastRecorder *, char *, size_t);
/* Deterministic synthetic integration tests; no CLI/runtime input injection. */
#ifdef CAST_TEST
CastOutput *output_test_open(const Config *, int (*)(int, unsigned long, void *), char *, size_t);
struct AVFrame;
struct SwsContext;
struct SwsContext *camera_test_scaler(struct SwsContext *, const struct AVFrame *);
void audio_test_push(CastAudio *, int, uint64_t, const float *, int);
void audio_test_virtual_read(CastAudio *, uint64_t, float *, int);
CastAudio *media_test_audio(Media *);
void recorder_test_slow(CastRecorder *, int);
void recorder_test_hold(CastRecorder *, int);
bool recorder_test_codec_busy(CastRecorder *);
void recorder_test_failure(CastRecorder *, int);
void recorder_test_write_limit(CastRecorder *, int64_t);
CastRecorder *media_test_recorder(Media *);
#endif
#endif
