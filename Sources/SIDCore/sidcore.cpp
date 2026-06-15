#include "sidcore.h"

#include <cmath>
#include <cstring>
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
constexpr uint8_t kFilterResonanceRoute = 0x17;
constexpr uint8_t kFilterCutoff = 0x16;
constexpr uint8_t kFilterModeVolume = 0x18;
constexpr uint8_t kFilterCutoffHiUnused = 0x15;
constexpr uint8_t kMasterVolume = 0x18;
constexpr int kTableCount = 4;
constexpr int kWTBL = 0;
constexpr int kPTBL = 1;
constexpr int kFTBL = 2;
constexpr uint8_t kWaveDelayLast = 0x0f;
constexpr uint8_t kWaveSilent = 0xe0;
constexpr uint8_t kWaveSilentLast = 0xef;
constexpr uint8_t kWaveCommand = 0xf0;
constexpr uint8_t kWaveCommandLast = 0xfe;
constexpr uint8_t kTableJump = 0xff;
constexpr uint32_t kPlayerTicksPerSecond = 50;

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
SIDCoreInstrument sidcore_current_instrument = {0.01f, 0.20f, 0.80f, 0.30f, 0x00, 0x03, 0x20};
char sidcore_event_buffer[160] = "Idle";
float sidcore_sample_rate = 48000.0f;
uint8_t sidcore_last_note = 60;
uint8_t sidcore_last_ad = 0x52;
uint8_t sidcore_last_sr = 0xc5;
bool sidcore_gate_on = false;
uint16_t sidcore_current_freq = 0;
uint8_t sidcore_current_wave = 0x20;
uint8_t sidcore_wavetable_ptr = 0;
uint8_t sidcore_pulsetable_ptr = 0;
uint8_t sidcore_filtertable_ptr = 0;
uint8_t sidcore_wavetable_delay = 0;
uint8_t sidcore_pulse_time = 0;
uint8_t sidcore_filter_time = 0;
uint16_t sidcore_pulse_width = 0x0800;
uint8_t sidcore_filter_cutoff = 0;
uint8_t sidcore_filter_ctrl = 0;
uint8_t sidcore_filter_type = 0;
uint32_t sidcore_tick_samples = 960;
uint32_t sidcore_samples_until_tick = 960;

struct SIDCoreInstrumentBlob
{
  uint8_t pointers[kTableCount] = {0, 0, 0, 0};
  char name[16] = {'S', 'I', 'D', 'L', 'o', 'r', 'd', 0};
  uint8_t table_len[kTableCount] = {0, 0, 0, 0};
  std::vector<uint8_t> left[kTableCount];
  std::vector<uint8_t> right[kTableCount];
} sidcore_blob;

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

uint8_t sid_waveform_from_firstwave(uint8_t firstwave)
{
  // GoatTracker firstwave can carry non-wave bits; playback needs waveform bits only.
  uint8_t waveform = firstwave & 0xf0;
  if (!waveform) waveform = 0x20;
  return waveform;
}

uint32_t sid_samples_per_tick(float sample_rate)
{
  if (sample_rate < 1000.0f) return 1;
  uint32_t samples = static_cast<uint32_t>(std::lround(sample_rate / static_cast<float>(kPlayerTicksPerSecond)));
  if (samples == 0) samples = 1;
  return samples;
}

uint8_t sid_table_left(int table, uint8_t ptr)
{
  if (ptr == 0 || table < 0 || table >= kTableCount) return 0;
  size_t index = static_cast<size_t>(ptr - 1);
  if (index >= sidcore_blob.left[table].size()) return 0;
  return sidcore_blob.left[table][index];
}

uint8_t sid_table_right(int table, uint8_t ptr)
{
  if (ptr == 0 || table < 0 || table >= kTableCount) return 0;
  size_t index = static_cast<size_t>(ptr - 1);
  if (index >= sidcore_blob.right[table].size()) return 0;
  return sidcore_blob.right[table][index];
}

void sid_write_voice1_locked()
{
  if (!sid_instance) return;
  sid_instance->write(kVoice1FreqLo, static_cast<uint8_t>(sidcore_current_freq & 0xff));
  sid_instance->write(kVoice1FreqHi, static_cast<uint8_t>((sidcore_current_freq >> 8) & 0xff));
  sid_instance->write(kVoice1PulseLo, static_cast<uint8_t>(sidcore_pulse_width & 0xff));
  sid_instance->write(kVoice1PulseHi, static_cast<uint8_t>((sidcore_pulse_width >> 8) & 0x0f));
  uint8_t control = sidcore_current_wave & 0xfe;
  if (sidcore_gate_on) control |= 0x01;
  sid_instance->write(kVoice1Control, control);
}

void sid_write_filter_locked()
{
  if (!sid_instance) return;
  sid_instance->write(kFilterCutoffHiUnused, 0x00);
  sid_instance->write(kFilterCutoff, sidcore_filter_cutoff);
  sid_instance->write(kFilterResonanceRoute, sidcore_filter_ctrl);
  sid_instance->write(kFilterModeVolume, static_cast<uint8_t>(sidcore_filter_type | 0x0f));
}

void sid_step_filter_table_locked()
{
  if (sidcore_filtertable_ptr == 0) return;

  uint8_t left = sid_table_left(kFTBL, sidcore_filtertable_ptr);
  uint8_t right = sid_table_right(kFTBL, sidcore_filtertable_ptr);
  if (left == 0 && right == 0)
  {
    sidcore_filtertable_ptr = 0;
    return;
  }

  if (left == kTableJump)
  {
    sidcore_filtertable_ptr = right;
    if (sidcore_filtertable_ptr == 0) return;
    left = sid_table_left(kFTBL, sidcore_filtertable_ptr);
    right = sid_table_right(kFTBL, sidcore_filtertable_ptr);
  }

  if (sidcore_filter_time == 0)
  {
    if (left >= 0x80)
    {
      sidcore_filter_type = left & 0x70;
      sidcore_filter_ctrl = right;
      sidcore_filtertable_ptr++;
      left = sid_table_left(kFTBL, sidcore_filtertable_ptr);
      right = sid_table_right(kFTBL, sidcore_filtertable_ptr);
      if (left == 0x00)
      {
        sidcore_filter_cutoff = right;
        sidcore_filtertable_ptr++;
      }
      sid_write_filter_locked();
      return;
    }

    if (left != 0x00)
    {
      sidcore_filter_time = left;
    }
    else
    {
      sidcore_filter_cutoff = right;
      sidcore_filtertable_ptr++;
      sid_write_filter_locked();
      return;
    }
  }

  if (sidcore_filter_time > 0)
  {
    sidcore_filter_cutoff = static_cast<uint8_t>(sidcore_filter_cutoff + right);
    sidcore_filter_time--;
    if (sidcore_filter_time == 0) sidcore_filtertable_ptr++;
  }
  sid_write_filter_locked();
}

void sid_step_pulse_table_locked()
{
  if (sidcore_pulsetable_ptr == 0) return;

  uint8_t left = sid_table_left(kPTBL, sidcore_pulsetable_ptr);
  uint8_t right = sid_table_right(kPTBL, sidcore_pulsetable_ptr);
  if (left == 0 && right == 0)
  {
    sidcore_pulsetable_ptr = 0;
    return;
  }

  if (left == kTableJump)
  {
    sidcore_pulsetable_ptr = right;
    if (sidcore_pulsetable_ptr == 0) return;
    left = sid_table_left(kPTBL, sidcore_pulsetable_ptr);
    right = sid_table_right(kPTBL, sidcore_pulsetable_ptr);
  }

  if (sidcore_pulse_time == 0)
  {
    if (left >= 0x80)
    {
      sidcore_pulse_width = static_cast<uint16_t>(((left & 0x0f) << 8) | right);
      sidcore_pulsetable_ptr++;
      sid_write_voice1_locked();
      return;
    }
    sidcore_pulse_time = left;
  }

  if (sidcore_pulse_time > 0)
  {
    int delta = (right < 0x80) ? static_cast<int>(right) : static_cast<int>(right) - 0x100;
    sidcore_pulse_width = static_cast<uint16_t>((sidcore_pulse_width + delta) & 0x0fff);
    sidcore_pulse_time--;
    if (sidcore_pulse_time == 0) sidcore_pulsetable_ptr++;
  }
  sid_write_voice1_locked();
}

void sid_execute_wave_command_locked(uint8_t command, uint8_t param)
{
  switch (command)
  {
    case 0x05: // set AD
      sidcore_last_ad = param;
      if (sid_instance) sid_instance->write(kVoice1AttackDecay, sidcore_last_ad);
      break;
    case 0x06: // set SR
      sidcore_last_sr = param;
      if (sid_instance) sid_instance->write(kVoice1SustainRelease, sidcore_last_sr);
      break;
    case 0x07: // set wave
      sidcore_current_wave = sid_waveform_from_firstwave(param);
      sid_write_voice1_locked();
      break;
    case 0x09: // set pulse table ptr
      sidcore_pulsetable_ptr = param;
      sidcore_pulse_time = 0;
      break;
    case 0x0a: // set filter table ptr
      sidcore_filtertable_ptr = param;
      sidcore_filter_time = 0;
      break;
    case 0x0b: // set filter ctrl
      sidcore_filter_ctrl = param;
      if (!sidcore_filter_ctrl) sidcore_filtertable_ptr = 0;
      sid_write_filter_locked();
      break;
    case 0x0c: // set filter cutoff
      sidcore_filter_cutoff = param;
      sid_write_filter_locked();
      break;
    case 0x0d: // set master volume
      if (param < 0x10)
      {
        if (sid_instance) sid_instance->write(kMasterVolume, static_cast<uint8_t>(sidcore_filter_type | param));
      }
      break;
    default:
      break;
  }
}

void sid_step_wave_table_locked()
{
  if (sidcore_wavetable_ptr == 0) return;

  uint8_t wave = sid_table_left(kWTBL, sidcore_wavetable_ptr);
  uint8_t note = sid_table_right(kWTBL, sidcore_wavetable_ptr);
  if (wave == 0 && note == 0)
  {
    sidcore_wavetable_ptr = 0;
    return;
  }

  if (wave == kTableJump)
  {
    sidcore_wavetable_ptr = note;
    if (sidcore_wavetable_ptr == 0) return;
    wave = sid_table_left(kWTBL, sidcore_wavetable_ptr);
    note = sid_table_right(kWTBL, sidcore_wavetable_ptr);
  }

  bool was_command = false;
  if (wave <= kWaveDelayLast)
  {
    if (sidcore_wavetable_delay != wave)
    {
      sidcore_wavetable_delay++;
      return;
    }
  }
  else
  {
    if (wave < kWaveSilent)
    {
      sidcore_current_wave = wave & 0xfe;
      sid_write_voice1_locked();
    }
    else if (wave <= kWaveSilentLast)
    {
      sidcore_current_wave = wave & 0x0f;
      sid_write_voice1_locked();
    }
    else if (wave >= kWaveCommand && wave <= kWaveCommandLast)
    {
      was_command = true;
      sid_execute_wave_command_locked(static_cast<uint8_t>(wave & 0x0f), note);
    }
  }

  sidcore_wavetable_delay = 0;
  sidcore_wavetable_ptr++;
  if (sid_table_left(kWTBL, sidcore_wavetable_ptr) == kTableJump)
  {
    sidcore_wavetable_ptr = sid_table_right(kWTBL, sidcore_wavetable_ptr);
  }

  if (!was_command && note != 0x80)
  {
    if (note < 0x80)
    {
      int target = static_cast<int>(sidcore_last_note) + static_cast<int>(note);
      if (target < 0) target = 0;
      if (target > 127) target = 127;
      sidcore_current_freq = sid_frequency_from_midi(static_cast<uint8_t>(target));
      sid_write_voice1_locked();
    }
  }
}

void sidcore_step_tables_locked()
{
  sid_step_filter_table_locked();
  sid_step_wave_table_locked();
  sid_step_pulse_table_locked();
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
  sid_write_filter_locked();
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
  sidcore_last_ad = static_cast<uint8_t>((attack << 4) | decay);
  sidcore_last_sr = static_cast<uint8_t>((sustain << 4) | release);

  sid_instance->write(kVoice1AttackDecay, sidcore_last_ad);
  sid_instance->write(kVoice1SustainRelease, sidcore_last_sr);
}

bool sidcore_read_instrument_header(FILE *handle,
                                    uint8_t *ad,
                                    uint8_t *sr,
                                    uint8_t *vibdelay,
                                    uint8_t *gatetimer,
                                    uint8_t *firstwave)
{
  uint8_t pointers[4];
  char name[16];
  uint8_t len;

  if (std::fread(ad, 1, 1, handle) != 1) return false;
  if (std::fread(sr, 1, 1, handle) != 1) return false;
  if (std::fread(pointers, 1, sizeof(pointers), handle) != sizeof(pointers)) return false;
  if (std::fread(vibdelay, 1, 1, handle) != 1) return false;
  if (std::fread(gatetimer, 1, 1, handle) != 1) return false;
  if (std::fread(firstwave, 1, 1, handle) != 1) return false;
  if (std::fread(name, 1, sizeof(name), handle) != sizeof(name)) return false;

  for (int i = 0; i < 4; i++)
  {
    if (std::fread(&len, 1, 1, handle) != 1) return false;
    if (len > 0)
    {
      if (std::fseek(handle, static_cast<long>(len) * 2L, SEEK_CUR) != 0) return false;
    }
  }
  return true;
}

bool sidcore_read_instrument_with_tables(FILE *handle,
                                         uint8_t *ad,
                                         uint8_t *sr,
                                         uint8_t *vibdelay,
                                         uint8_t *gatetimer,
                                         uint8_t *firstwave)
{
  if (std::fread(ad, 1, 1, handle) != 1) return false;
  if (std::fread(sr, 1, 1, handle) != 1) return false;
  if (std::fread(sidcore_blob.pointers, 1, kTableCount, handle) != kTableCount) return false;
  if (std::fread(vibdelay, 1, 1, handle) != 1) return false;
  if (std::fread(gatetimer, 1, 1, handle) != 1) return false;
  if (std::fread(firstwave, 1, 1, handle) != 1) return false;
  if (std::fread(sidcore_blob.name, 1, sizeof(sidcore_blob.name), handle) != sizeof(sidcore_blob.name)) return false;

  for (int i = 0; i < kTableCount; i++)
  {
    uint8_t len = 0;
    if (std::fread(&len, 1, 1, handle) != 1) return false;
    sidcore_blob.table_len[i] = len;
    sidcore_blob.left[i].assign(len, 0);
    sidcore_blob.right[i].assign(len, 0);
    if (len > 0)
    {
      if (std::fread(sidcore_blob.left[i].data(), 1, len, handle) != len) return false;
      if (std::fread(sidcore_blob.right[i].data(), 1, len, handle) != len) return false;
    }
  }
  return true;
}

void sidcore_apply_ad_sr_to_instrument(uint8_t ad, uint8_t sr, SIDCoreInstrument *instrument)
{
  int attack = (ad >> 4) & 0x0f;
  int decay = ad & 0x0f;
  int sustain = (sr >> 4) & 0x0f;
  int release = sr & 0x0f;

  instrument->attack = kAttackTimes[attack];
  instrument->decay = kDecayReleaseTimes[decay];
  instrument->sustain = static_cast<float>(sustain) / 15.0f;
  instrument->release = kDecayReleaseTimes[release];
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
  instrument->vibDelay = 0x00;
  instrument->gateTimer = 0x03;
  instrument->firstWave = 0x20;
  sidcore_current_instrument = *instrument;
  sidcore_apply_adsr_locked();
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
  sidcore_current_freq = sid_frequency_from_midi(note);
  sidcore_current_wave = sid_waveform_from_firstwave(sidcore_current_instrument.firstWave);
  sidcore_gate_on = true;
  sidcore_wavetable_ptr = sidcore_blob.pointers[kWTBL];
  sidcore_pulsetable_ptr = sidcore_blob.pointers[kPTBL];
  sidcore_filtertable_ptr = sidcore_blob.pointers[kFTBL];
  sidcore_wavetable_delay = 0;
  sidcore_pulse_time = 0;
  sidcore_filter_time = 0;
  sidcore_tick_samples = sid_samples_per_tick(sidcore_sample_rate);
  sidcore_samples_until_tick = sidcore_tick_samples;
  sid_write_voice1_locked();
  sid_write_filter_locked();
  sid_instance->write(kMasterVolume, 0x0f);

  std::snprintf(sidcore_event_buffer,
                sizeof(sidcore_event_buffer),
                "reSID note on: %u vel=%u wave=%02X wt=%02X pt=%02X ft=%02X",
                note,
                velocity,
                sidcore_current_instrument.firstWave,
                sidcore_wavetable_ptr,
                sidcore_pulsetable_ptr,
                sidcore_filtertable_ptr);
}

void sidcore_note_off(uint8_t note)
{
  (void)note;
  std::lock_guard<std::mutex> lock(sidcore_mutex);
  sidcore_gate_on = false;
  sid_write_voice1_locked();
  std::snprintf(sidcore_event_buffer, sizeof(sidcore_event_buffer), "reSID note off: %u", sidcore_last_note);
}

void sidcore_set_sample_rate(float sample_rate)
{
  std::lock_guard<std::mutex> lock(sidcore_mutex);
  if (sample_rate < 8000.0f) return;
  sidcore_sample_rate = sample_rate;
  sidcore_init_engine_locked();
  sid_instance->set_sampling_parameters(kPALClockRate, SAMPLE_FAST, sidcore_sample_rate);
  sidcore_tick_samples = sid_samples_per_tick(sidcore_sample_rate);
  sidcore_samples_until_tick = sidcore_tick_samples;
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
    uint32_t chunk = remaining;
    if (sidcore_tick_samples == 0) sidcore_tick_samples = sid_samples_per_tick(sidcore_sample_rate);
    if (sidcore_samples_until_tick == 0) sidcore_samples_until_tick = sidcore_tick_samples;
    if (chunk > sidcore_samples_until_tick) chunk = sidcore_samples_until_tick;

    cycle_count delta = static_cast<cycle_count>(kPALClockRate * chunk / sidcore_sample_rate);
    if (delta <= 0) delta = 1;
    int rendered = sid_instance->clock(delta, temp.data() + produced, static_cast<int>(chunk));
    if (rendered <= 0) break;
    produced += rendered;
    remaining -= rendered;

    if (rendered >= static_cast<int>(sidcore_samples_until_tick))
    {
      sidcore_samples_until_tick = sidcore_tick_samples;
      sidcore_step_tables_locked();
    }
    else
    {
      sidcore_samples_until_tick -= static_cast<uint32_t>(rendered);
    }
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

int sidcore_save_ins(const char *path, const SIDCoreInstrument *instrument)
{
  uint8_t vibdelay;
  uint8_t gatetimer;
  uint8_t firstwave;
  uint8_t ad;
  uint8_t sr;
  FILE *handle;

  if ((!path) || (!instrument)) return 0;

  std::lock_guard<std::mutex> lock(sidcore_mutex);
  sidcore_current_instrument = *instrument;
  sidcore_apply_adsr_locked();
  ad = sidcore_last_ad;
  sr = sidcore_last_sr;
  vibdelay = sidcore_current_instrument.vibDelay;
  gatetimer = sidcore_current_instrument.gateTimer;
  firstwave = sidcore_current_instrument.firstWave;

  handle = std::fopen(path, "wb");
  if (!handle)
  {
    std::snprintf(sidcore_event_buffer, sizeof(sidcore_event_buffer), "Save failed: could not open file");
    return 0;
  }

  if (std::fwrite("GTI5", 1, 4, handle) != 4 ||
      std::fwrite(&ad, 1, 1, handle) != 1 ||
      std::fwrite(&sr, 1, 1, handle) != 1 ||
      std::fwrite(sidcore_blob.pointers, 1, kTableCount, handle) != kTableCount ||
      std::fwrite(&vibdelay, 1, 1, handle) != 1 ||
      std::fwrite(&gatetimer, 1, 1, handle) != 1 ||
      std::fwrite(&firstwave, 1, 1, handle) != 1 ||
      std::fwrite(sidcore_blob.name, 1, sizeof(sidcore_blob.name), handle) != sizeof(sidcore_blob.name))
  {
    std::fclose(handle);
    std::snprintf(sidcore_event_buffer, sizeof(sidcore_event_buffer), "Save failed: write error");
    return 0;
  }

  for (int i = 0; i < kTableCount; i++)
  {
    uint8_t len = sidcore_blob.table_len[i];
    if (std::fwrite(&len, 1, 1, handle) != 1)
    {
      std::fclose(handle);
      std::snprintf(sidcore_event_buffer, sizeof(sidcore_event_buffer), "Save failed: write error");
      return 0;
    }
    if (len > 0)
    {
      if (sidcore_blob.left[i].size() < len || sidcore_blob.right[i].size() < len ||
          std::fwrite(sidcore_blob.left[i].data(), 1, len, handle) != len ||
          std::fwrite(sidcore_blob.right[i].data(), 1, len, handle) != len)
      {
        std::fclose(handle);
        std::snprintf(sidcore_event_buffer, sizeof(sidcore_event_buffer), "Save failed: write error");
        return 0;
      }
    }
  }

  std::fclose(handle);
  std::snprintf(sidcore_event_buffer, sizeof(sidcore_event_buffer), "Saved GTI5 instrument");
  return 1;
}

int sidcore_load_ins(const char *path, SIDCoreInstrument *instrument)
{
  char ident[4];
  uint8_t ad;
  uint8_t sr;
  uint8_t vibdelay;
  uint8_t gatetimer;
  uint8_t firstwave;
  FILE *handle;

  if ((!path) || (!instrument)) return 0;

  handle = std::fopen(path, "rb");
  if (!handle)
  {
    std::snprintf(sidcore_event_buffer, sizeof(sidcore_event_buffer), "Load failed: could not open file");
    return 0;
  }

  if (std::fread(ident, 1, 4, handle) != 4)
  {
    std::fclose(handle);
    std::snprintf(sidcore_event_buffer, sizeof(sidcore_event_buffer), "Load failed: short file");
    return 0;
  }

  if (std::memcmp(ident, "GTI5", 4) && std::memcmp(ident, "GTI4", 4) && std::memcmp(ident, "GTI3", 4))
  {
    std::fclose(handle);
    std::snprintf(sidcore_event_buffer, sizeof(sidcore_event_buffer), "Load failed: unsupported instrument format");
    return 0;
  }

  if (!sidcore_read_instrument_with_tables(handle, &ad, &sr, &vibdelay, &gatetimer, &firstwave))
  {
    std::fclose(handle);
    std::snprintf(sidcore_event_buffer, sizeof(sidcore_event_buffer), "Load failed: corrupt instrument data");
    return 0;
  }

  std::fclose(handle);

  std::lock_guard<std::mutex> lock(sidcore_mutex);
  sidcore_apply_ad_sr_to_instrument(ad, sr, instrument);
  instrument->vibDelay = vibdelay;
  instrument->gateTimer = gatetimer;
  instrument->firstWave = firstwave;
  sidcore_current_instrument = *instrument;
  sidcore_last_ad = ad;
  sidcore_last_sr = sr;
  sidcore_init_engine_locked();
  sidcore_apply_adsr_locked();
  std::snprintf(sidcore_event_buffer,
                sizeof(sidcore_event_buffer),
                "Loaded GTI AD=%02X SR=%02X FW=%02X GT=%02X VD=%02X",
                ad,
                sr,
                firstwave,
                gatetimer,
                vibdelay);
  return 1;
}
}
