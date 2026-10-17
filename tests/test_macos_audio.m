/* Offline capture batches: no AVAudioEngine, devices, permissions or playback. */
#include "media_internal.h"
#include <assert.h>
#include <stdio.h>
static uint64_t clock_ns;
uint64_t cast_now_ns(void) { return clock_ns; }
int main(void)
{
    Config cfg = {0}; cfg.mic = true; cfg.mic_gain = 1;
    CastAudio *a = audio_test_open(&cfg); assert(a);
    float input[4800*2], output[480*2];
    for (unsigned i = 0; i < 4800*2; ++i) input[i] = 0.25f;
    uint64_t origin = 1000000000ULL;
    unsigned produced = 0, old_missing = 0;
    /* A 10 ms recorder tick and 30 ms safety horizon must retain every sample
     * when the microphone delivers 100 ms batches (not requested 1024 frames). */
    for (unsigned ms = 10; ms <= 1100; ms += 10) {
        clock_ns = origin + (uint64_t)ms*1000000;
        if (ms <= 1000 && ms % 100 == 0)
            audio_test_push(a, 0, clock_ns - 100000000, input, 4800);
        unsigned goal = ms > 30 ? (ms-30)*48 : 0;
        if (goal > 48000) goal = 48000;
        while (produced + 480 <= goal) {
            uint64_t ns = origin + (uint64_t)produced*1000000000/48000;
            if (audio_record_pending(a, ns, 480)) break;
            audio_read(a, ns, output, 480);
            for (unsigned i = 0; i < 480*2; ++i) assert(output[i] == 0.25f);
            produced += 480;
        }
        /* The previous 30 ms policy has consumed unavailable samples by 40ms. */
        if (ms == 40) {
            audio_read(a, origin, output, 480);
            for (unsigned i = 0; i < 480*2; ++i) old_missing += output[i] == 0;
        }
    }
    assert(produced == 48000 && old_missing == 960);
    /* Stop's final batch waits for delivery; stalled capture has a deadline. */
    clock_ns = origin + 1110000000;
    uint64_t tail = origin + 1000000000;
    assert(audio_record_pending(a, tail, 480));
    audio_test_push(a, 0, tail, input, 4800);
    assert(!audio_record_pending(a, tail, 480));
    clock_ns = tail + 350000000;
    assert(!audio_record_pending(a, tail + 100000000, 480));
    /* Privacy retires previous data immediately, even while a batch is pending. */
    audio_record_privacy(a);
    assert(!audio_record_pending(a, tail, 480));
    audio_read(a, tail, output, 480);
    for (unsigned i = 0; i < 480*2; ++i) assert(output[i] == 0);
    audio_barrier(a, false);
    assert(!audio_record_pending(a, tail, 480));
    cfg.mic = false;
    CastAudio *disabled = audio_test_open(&cfg); assert(disabled);
    assert(!audio_record_pending(disabled, clock_ns, 480));
    /* Mixing waits for both sources, and privacy keeps old callbacks retired. */
    cfg.mic = true; cfg.desktop = true; cfg.desktop_gain = 1;
    CastAudio *mixed = audio_test_open(&cfg); assert(mixed);
    audio_test_push(mixed, 0, clock_ns, input, 4800);
    assert(audio_record_pending(mixed, clock_ns, 480));
    audio_test_push(mixed, 1, clock_ns, input, 4800);
    assert(!audio_record_pending(mixed, clock_ns, 480));
    audio_read(mixed, clock_ns, output, 480);
    for (unsigned i = 0; i < 480*2; ++i) assert(output[i] == 0.5f);
    audio_barrier(mixed, false);
    audio_test_push(mixed, 0, clock_ns - 100000000, input, 4800);
    audio_read(mixed, clock_ns - 100000000, output, 480);
    for (unsigned i = 0; i < 480*2; ++i) assert(output[i] == 0);
    audio_close(mixed); audio_close(disabled); audio_close(a);
    puts("macOS audio: 100 ms capture batches, bounded stall, stop tail, privacy PASS");
    return 0;
}
