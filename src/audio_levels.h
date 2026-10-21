/* Bounded input-meter arithmetic shared by capture backends. No PCM retention. */
#ifndef CAST_AUDIO_LEVELS_INTERNAL_H
#define CAST_AUDIO_LEVELS_INTERNAL_H
#include "cast.h"
#include <math.h>
#include <string.h>
#define AUDIO_LEVEL_STALE_NS UINT64_C(250000000)
typedef struct {double squares,peak;unsigned samples;bool clipped;} AudioLevelAccumulator;
static inline float audio_level_sample(AudioLevelAccumulator *level,float input,double gain)
{
    float clean=isfinite(input)?input:0;
    double sample=(double)clean*gain;
    if(!isfinite(sample))sample=0;
    double magnitude=fabs(sample);
    if(magnitude>level->peak)level->peak=magnitude;
    level->clipped|=magnitude>=1;
    level->squares+=sample*sample;++level->samples;
    return clean;
}
static inline uint64_t audio_level_capture_end(uint64_t first,unsigned frames)
{
    uint64_t last=first+frames;
    return (last/48000)*UINT64_C(1000000000)+(last%48000)*UINT64_C(1000000000)/48000;
}
static inline void audio_level_finish(AudioLaneLevel *out,const AudioLevelAccumulator *level,uint64_t timestamp)
{
    if(!level->samples)return;
    out->peak=(float)fmin(1,level->peak);
    out->rms=(float)fmin(1,sqrt(level->squares/level->samples));
    out->clipped=level->clipped;out->timestamp_ns=timestamp;
}
static inline void audio_level_clear(AudioLaneLevel *out,uint64_t epoch)
{
    memset(out,0,sizeof *out);out->epoch=epoch;
}
static inline void audio_level_gate(AudioLaneLevel *out,bool enabled,bool available,uint64_t now)
{
    out->enabled=enabled;out->available=enabled&&available;
    bool plausible=out->timestamp_ns && (now>=out->timestamp_ns || out->timestamp_ns-now<=UINT64_C(50000000));
    out->fresh=out->available&&plausible&&(now<out->timestamp_ns || now-out->timestamp_ns<=AUDIO_LEVEL_STALE_NS);
    if(!out->fresh){out->peak=out->rms=0;out->clipped=false;}
}
#endif
