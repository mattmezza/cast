#ifndef CAST_AUDIO_EXTRA_H
#define CAST_AUDIO_EXTRA_H
#include <stdint.h>
/* Immutable borrowed callback/context, alive until every consuming worker stops.
 * Mix interleaved stereo float PCM at 48 kHz without waiting, I/O or allocation.
 * Readers may invoke concurrently. Replacing the pointer does not retire users. */
typedef struct CastAudioExtra {
    void (*mix)(void *, uint64_t, float *, unsigned);
    void *user;
} CastAudioExtra;
/* Apply only the accepted suffix; never ask an extra source for historic PCM
 * that the route's existing privacy window has already rejected. */
static inline void cast_audio_extra_mix(const CastAudioExtra *extra, uint64_t ns,
                                        uint64_t accepted_ns, float *pcm, unsigned frames)
{
    if (!extra || !extra->mix || !frames) return;
    uint64_t skip = 0;
    if (accepted_ns > ns) {
        uint64_t delta = accepted_ns - ns;
        if (delta / UINT64_C(1000000000) > frames / 48000u) return;
        skip = (delta / UINT64_C(1000000000)) * 48000u +
               ((delta % UINT64_C(1000000000)) * 48000u + UINT64_C(999999999)) /
                   UINT64_C(1000000000);
        if (skip >= frames) return;
    }
    uint64_t offset = skip * UINT64_C(1000000000) / 48000u;
    if (ns > UINT64_MAX - offset) return;
    extra->mix(extra->user, ns + offset, pcm + skip * 2, frames - (unsigned)skip);
}
#endif
