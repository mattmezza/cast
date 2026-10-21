/* Synthetic capture only: no PipeWire server connection, mic or playback. */
#include "media_internal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static uint64_t clock_ns=UINT64_C(1000000000);
uint64_t cast_now_ns(void){return clock_ns;}
static void fill(float *samples,float left,float right)
{for(unsigned i=0;i<480;++i){samples[2*i]=left;samples[2*i+1]=right;}}
static void zero(const AudioLaneLevel *level)
{assert(level->peak==0&&level->rms==0&&!level->clipped);}
int main(void)
{
    Config cfg={.mic=true,.desktop=true,.mic_gain=1,.desktop_gain=.5};
    CastAudio *audio=audio_test_open(&cfg);assert(audio);
    AudioLevels levels;audio_levels(NULL,&levels);assert(!levels.supported);
    audio_levels(audio,&levels);assert(levels.supported&&!levels.mic.fresh);zero(&levels.mic);
    float samples[960];fill(samples,.5f,-.25f);
    audio_test_push(audio,0,clock_ns-UINT64_C(10000000),samples,480);
    fill(samples,.4f,-.4f);audio_test_push(audio,1,clock_ns-UINT64_C(10000000),samples,480);
    audio_levels(audio,&levels);
    assert(levels.mic.enabled&&levels.mic.available&&levels.mic.fresh);
    assert(fabsf(levels.mic.peak-.5f)<.00001f&&fabsf(levels.mic.rms-sqrtf(.15625f))<.00001f);
    assert(fabsf(levels.desktop.peak-.2f)<.00001f&&fabsf(levels.desktop.rms-.2f)<.00001f);
    fill(samples,0,0);clock_ns+=UINT64_C(10000000);audio_test_push(audio,0,clock_ns-UINT64_C(10000000),samples,480);
    audio_levels(audio,&levels);assert(levels.mic.fresh);zero(&levels.mic);
    fill(samples,NAN,INFINITY);clock_ns+=UINT64_C(10000000);audio_test_push(audio,0,clock_ns-UINT64_C(10000000),samples,480);
    audio_levels(audio,&levels);zero(&levels.mic);assert(isfinite(levels.mic.rms));
    fill(samples,2,-3);clock_ns+=UINT64_C(10000000);audio_test_push(audio,0,clock_ns-UINT64_C(10000000),samples,480);
    audio_levels(audio,&levels);assert(levels.mic.peak==1&&levels.mic.rms==1&&levels.mic.clipped);
    uint64_t old_epoch=levels.mic.epoch;
    uint64_t old_start=clock_ns-UINT64_C(10000000);
    clock_ns+=UINT64_C(1000000);audio_barrier(audio,true);
    audio_levels(audio,&levels);assert(levels.mic.epoch>old_epoch&&!levels.mic.fresh&&!levels.mic.timestamp_ns);zero(&levels.mic);zero(&levels.desktop);
    audio_test_push(audio,0,old_start,samples,480);audio_levels(audio,&levels);zero(&levels.mic);assert(!levels.mic.timestamp_ns);
    fill(samples,.5f,.5f);audio_test_push(audio,0,clock_ns,samples,480);audio_levels(audio,&levels);
    assert(levels.mic.fresh&&levels.mic.peak==.5f); /* Input mixer survives one output's pause. */
    clock_ns+=UINT64_C(300000000);audio_levels(audio,&levels);assert(!levels.mic.fresh);zero(&levels.mic);
    /* Muting and gain changes retire stored peaks immediately. No route is
     * opened: synthetic audio has no core, and this candidate disables inputs. */
    cfg.mic=cfg.desktop=false;cfg.mic_gain=0;char error[256];
    assert(!audio_configure(audio,&cfg,error,sizeof error));audio_levels(audio,&levels);
    assert(!levels.mic.enabled&&!levels.desktop.enabled);zero(&levels.mic);zero(&levels.desktop);
    fill(samples,.8f,.8f);audio_test_push(audio,0,clock_ns,samples,480);audio_levels(audio,&levels);zero(&levels.mic);
    /* A fresh synthetic route avoids opening a real device on either OS. */
    audio_close(audio);cfg.mic=true;cfg.mic_gain=4;audio=audio_test_open(&cfg);assert(audio);
    clock_ns+=UINT64_C(10000000);fill(samples,.5f,-.5f);audio_test_push(audio,0,clock_ns,samples,480);
    audio_levels(audio,&levels);assert(levels.mic.peak==1&&levels.mic.clipped);
    audio_close(audio);puts("audio levels PASS: real batch RMS/peak, gain, silence, clipping, stale expiry and privacy epoch");
}
