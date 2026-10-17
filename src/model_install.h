#ifndef CAST_MODEL_INSTALL_H
#define CAST_MODEL_INSTALL_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    const char *id, *label, *description, *languages;
    uint64_t bytes;
    const char *sha256, *url;
} CastSpeechModel;
typedef struct CastSpeechModelInstall CastSpeechModelInstall;
typedef struct {
    int busy, done, verified, cancelled;
    uint64_t downloaded, total;
    char path[4096], error[256];
} CastSpeechModelProgress;
const CastSpeechModel *cast_speech_models(size_t *count);
int cast_speech_model_directory(char *path, size_t capacity);
/* start verifies an existing regular file first; download=0 performs local verification only.
 * All filesystem/network/hash work runs in the worker. Poll never waits for I/O.
 * destroy cancels and joins; call only after poll reports done to avoid waiting. */
CastSpeechModelInstall *cast_speech_model_install_start(size_t index, int download);
void cast_speech_model_install_poll(CastSpeechModelInstall *, CastSpeechModelProgress *);
void cast_speech_model_install_cancel(CastSpeechModelInstall *);
void cast_speech_model_install_destroy(CastSpeechModelInstall *);
/* Cancel and release ownership without waiting. The worker cleans up on completion.
 * The handle must not be used after this call. Suitable for panel shutdown. */
void cast_speech_model_install_abandon(CastSpeechModelInstall *);
#endif
