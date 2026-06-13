#include "sidcore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

#include "resid/sid.h"

namespace {
constexpr int kPALClockRate = 985248;
constexpr uint8_t kVoice1FreqLo = 0x00;
constexpr uint8_t kVoice1FreqHi = 0x01;
constexpr uint8_t kVoice1PulseLo = 0x02;
constexpr uint8_t kVoice1PulseHi = 0x03;
constexpr uint8_t kVoice1Control = 0x04;
constexpr uint8_t kVoice1AttackDecay = 0x05;
constexpr uint8_t kVoice1SustainRelease = 0x06;
constexpr uint8_t kMasterVolume = 0x18;
constexpr uint8_t kSawWaveGate = 0x21;
constexpr uint8_t kSawWaveNoGate = 0x20;

const float kAttackTimes[16] = {
  0.002f, 0.008f, 0.016f, 0.024f, 0.038f, 0.056f, 0.068f, 0.080f,
  0.100f, 0.250f, 0.500f, 0.800f, 1.000f, 3.000f, 5.000f, 8.000f
};

const float kDecayReleaseTimes[16] = {
  0.006f, 0.024f, 0.048f, 0.072f, 0.114f, 0.168f, 0.204f, 0.240f,
  0.300f, 0.750f, 1.500f, 2.400f, 3.000f, 9.000f, 15.000f, 24.000f
};

std::mutex sidcore_mutex;
SID *sid_instance = nullptr;
SIDCoreInstrument sidcore_current_instrument = {0.01f, 0.20f, 0.80f, 0.30f};
char sidcore_event_buffer[160] = "Idle";
float sidcore_sample_rate = 48000.0f;
uint8_t sidcore_last_note = 60;

int closest_time_index(const float *table, float value)
{
  int best_index = 0;
  float best_diff = std::fabs(table[0] - value);
  for (int i = 1; i < 16; i++)
  {
    float diff = std::fabs(table[i] - value);
    if (diff < best_diff)
    {
      best_diff = diff;
      best_index = i;
    }
  }
  return best_index;
}

uint16_t sid_frequency_from_midi(uint8_t note)
{
  double hz = 440.0 * std::pow(2.0, (static_cast<int>(note) - 69) / 12.0);
  double freq = hz * 16777216.0 / static_cast<double>(kPALClockRate);
  if (freq < 0.0) freq = 0.0;
  if (freq > 65535.0) freq = 65535.0;
  return static_cast<uint16_t>(freq);
}

void sidcore_init_engine_locked()
{
  if (sid_instance) return;

  sid_instance = new SID();
  sid_instance->set_chip_model(MOS6581);
  sid_instance->set_sampling_parameters(kPALClockRate, SAMPLE_FAST, sidcore_sample_rate);
  sid_instance->reset();
  sid_instance->write(kMasterVolume, 0x0f);
  sid_instance->write(kVoice1PulseLo, 0x00);
  sid_instance->write(kVoice1PulseHi, 0x08);
}

void sidcore_apply_adsr_locked()
{
  if (!sid_instance) return;

  int attack = closest_time_index(kAttackTimes, std::max(0.001f, sidcore_current_instrument.attack));
  int decay = closest_time_index(kDecayReleaseTimes, std::max(0.001f, sidcore_current_instrument.decay));
  float sustain_level = sidcore_current_instrument.sustain;
  if (sustain_level < 0.0f) sustain_level = 0.0f;
  if (sustain_level > 1.0f) sustain_level = 1.0f;
  int sustain = static_cast<int>(std::round(sustain_level * 15.0f));
  int release = closest_time_index(kDecayReleaseTimes, std::max(0.001f, sidcore_current_instrument.release));

  sid_instance->write(kVoice1AttackDecay, static_cast<uint8_t>((attack << 4) | decay));
  sid_instance->write(kVoice1SustainRelease, static_cast<uint8_t>((sustain << 4) | release));
}
}

extern "C" {
void sidcore_init_default_instrument(SIDCoreInstrument *instrument)
{
  if (!instrument) return;
  std::lock_guard<std::mutex> lock(sidcore_mutex);
  instrument->attack = 0.01f;
  instrument->decay = 0.20f;
  instrument->sustain = 0.80f;
  instrument->release = 0.30f;
  sidcore_current_instrument = *instrument;
}

void sidcore_set_adsr(SIDCoreInstrument *instrument, float attack, float decay, float sustain, float release)
{
  if (!instrument) return;
  std::lock_guard<std::mutex> lock(sidcore_mutex);
  instrument->attack = attack;
  instrument->decay = decay;
  instrument->sustain = sustain;
  instrument->release = release;
  sidcore_current_instrument = *instrument;
  sidcore_init_engine_locked();
  sidcore_apply_adsr_locked();
  std::snprintf(sidcore_event_buffer,
                sizeof(sidcore_event_buffer),
                "ADSR updated: %.2f/%.2f/%.2f/%.2f",
                instrument->attack,
                instrument->decay,
                instrument->sustain,
                instrument->release);
}

void sidcore_note_on(uint8_t note, uint8_t velocity, const SIDCoreInstrument *instrument)
{
  if (!instrument) return;
  std::lock_guard<std::mutex> lock(sidcore_mutex);
  sidcore_init_engine_locked();
  sidcore_current_instrument = *instrument;
  sidcore_apply_adsr_locked();

  sidcore_last_note = note;
  uint16_t freq = sid_frequency_from_midi(note);
  sid_instance->write(kVoice1FreqLo, static_cast<uint8_t>(freq & 0xff));
  sid_instance->write(kVoice1FreqHi, static_cast<uint8_t>((freq >> 8) & 0xff));
  sid_instance->write(kVoice1Control, kSawWaveGate);
  sid_instance->write(kMasterVolume, 0x0f);

  std::snprintf(sidcore_event_buffer,
                sizeof(sidcore_event_buffer),
                "reSID note on: %u vel=%u ADSR %.2f/%.2f/%.2f/%.2f",
                note,
                velocity,
                instrument->attack,
                instrument->decay,
                instrument->sustain,
                instrument->release);
}

void sidcore_note_off(uint8_t note)
{
  (void)note;
  std::lock_guard<std::mutex> lock(sidcore_mutex);
  if (sid_instance)
    sid_instance->write(kVoice1Control, kSawWaveNoGate);
  std::snprintf(sidcore_event_buffer, sizeof(sidcore_event_buffer), "reSID note off: %u", sidcore_last_note);
}

void sidcore_set_sample_rate(float sample_rate)
{
  std::lock_guard<std::mutex> lock(sidcore_mutex);
  if (sample_rate < 8000.0f) return;
  sidcore_sample_rate = sample_rate;
  sidcore_init_engine_locked();
  sid_instance->set_sampling_parameters(kPALClockRate, SAMPLE_FAST, sidcore_sample_rate);
  sid_instance->write(kMasterVolume, 0x0f);
  sidcore_apply_adsr_locked();
}

void sidcore_render(float *buffer, uint32_t frame_count)
{
  if (!buffer) return;
  std::lock_guard<std::mutex> lock(sidcore_mutex);
  sidcore_init_engine_locked();

  std::vector<short> temp(frame_count, 0);
  int produced = 0;
  int remaining = static_cast<int>(frame_count);

  while (remaining > 0)
  {
    cycle_count delta = static_cast<cycle_count>(kPALClockRate * remaining / sidcore_sample_rate);
    if (delta <= 0) delta = 1;
    int rendered = sid_instance->clock(delta, temp.data() + produced, remaining);
    if (rendered <= 0) break;
    produced += rendered;
    remaining -= rendered;
  }

  for (uint32_t i = 0; i < frame_count; i++)
  {
    short sample = (i < static_cast<uint32_t>(produced)) ? temp[i] : 0;
    buffer[i] = static_cast<float>(sample) / 32768.0f;
  }
}

const char *sidcore_last_event(void)
{
  return sidcore_event_buffer;
}
}
