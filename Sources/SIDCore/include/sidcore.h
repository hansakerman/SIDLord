#ifndef SIDCORE_H
#define SIDCORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  float attack;
  float decay;
  float sustain;
  float release;
  uint8_t vibDelay;
  uint8_t gateTimer;
  uint8_t firstWave;
} SIDCoreInstrument;

void sidcore_init_default_instrument(SIDCoreInstrument *instrument);
void sidcore_set_adsr(SIDCoreInstrument *instrument, float attack, float decay, float sustain, float release);
void sidcore_note_on(uint8_t note, uint8_t velocity, const SIDCoreInstrument *instrument);
void sidcore_note_off(uint8_t note);
void sidcore_set_sample_rate(float sample_rate);
void sidcore_render(float *buffer, uint32_t frame_count);
int sidcore_save_ins(const char *path, const SIDCoreInstrument *instrument);
int sidcore_load_ins(const char *path, SIDCoreInstrument *instrument);
const char *sidcore_last_event(void);

#ifdef __cplusplus
}
#endif

#endif
