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
constexpr uint8_t kHardRestartAD = 0x0f;
constexpr uint8_t kHardRestartSR = 0x00;
constexpr int kTableCount = 4;
constexpr int kWTBL = 0;
constexpr int kPTBL = 1;
constexpr int kFTBL = 2;
constexpr int kSTBL = 3;
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
char sidcore_event_buffer[512] = "Idle";
float sidcore_sample_rate = 48000.0f;
int sidcore_chip_model = 8580;
uint8_t sidcore_last_note = 60;
uint8_t sidcore_base_note = 60;
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
uint8_t sidcore_master_volume = 0x0f;
uint8_t sidcore_vib_time = 0;
uint8_t sidcore_vib_delay = 0;
uint8_t sidcore_speedtable_ptr = 0;
uint8_t sidcore_table_base[kTableCount] = {0, 0, 0, 0};
uint32_t sidcore_tick_samples = 960;
uint32_t sidcore_samples_until_tick = 960;

bool sid_table_valid_index(int table)
{
  return table >= 0 && table < kTableCount;
}

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

uint8_t sid_table_left(int table, uint8_t ptr);
uint8_t sid_table_right(int table, uint8_t ptr);
void sid_write_voice1_locked();

size_t sid_table_index(int table, uint8_t ptr, bool *ok)
{
  if (ok) *ok = false;
  if (ptr == 0 || table < 0 || table >= kTableCount) return 0;

  size_t index = static_cast<size_t>(ptr - 1);
  uint8_t base = sidcore_table_base[table];
  if (base != 0 && ptr > base)
  {
    index = static_cast<size_t>(ptr - base - 1);
  }

  if (index >= sidcore_blob.left[table].size()) return 0;
  if (ok) *ok = true;
  return index;
}

uint16_t sid_frequency_from_midi(uint8_t note)
{
  double hz = 440.0 * std::pow(2.0, (static_cast<int>(note) - 69) / 12.0);
  double freq = hz * 16777216.0 / static_cast<double>(kPALClockRate);
  if (freq < 0.0) freq = 0.0;
  if (freq > 65535.0) freq = 65535.0;
  return static_cast<uint16_t>(freq);
}

uint16_t sid_note_step_from_current_note()
{
  int note = static_cast<int>(sidcore_last_note);
  if (note < 0) note = 0;
  if (note > 126) note = 126;
  uint16_t current = sid_frequency_from_midi(static_cast<uint8_t>(note));
  uint16_t next = sid_frequency_from_midi(static_cast<uint8_t>(note + 1));
  if (next >= current) return static_cast<uint16_t>(next - current);
  return 0;
}

bool sid_speedtable_entry(uint8_t param, uint8_t *left, uint8_t *right)
{
  if (param == 0) return false;
  bool ok = false;
  size_t index = sid_table_index(kSTBL, param, &ok);
  if (!ok) return false;
  if (left) *left = sidcore_blob.left[kSTBL][index];
  if (right) *right = sidcore_blob.right[kSTBL][index];
  return true;
}

uint16_t sid_speed_from_speedtable(uint8_t param)
{
  uint8_t left = 0;
  uint8_t right = 0;
  if (!sid_speedtable_entry(param, &left, &right)) return 0;
  uint16_t speed = static_cast<uint16_t>((static_cast<uint16_t>(left) << 8) | right);
  if (speed >= 0x8000)
  {
    uint16_t note_step = sid_note_step_from_current_note();
    uint8_t shift = right & 0x1f;
    speed = static_cast<uint16_t>(note_step >> shift);
  }
  return speed;
}

bool sid_wave_command_uses_speedtable(uint8_t command)
{
  return command == 0x01 || command == 0x02 || command == 0x03 || command == 0x04;
}

uint8_t sid_count_invalid_speed_refs_locked()
{
  uint8_t invalid = 0;
  int len = static_cast<int>(sidcore_blob.table_len[kWTBL]);
  for (int i = 0; i < len; i++)
  {
    uint8_t wave = sidcore_blob.left[kWTBL][i];
    uint8_t param = sidcore_blob.right[kWTBL][i];
    if (wave >= kWaveCommand && wave <= kWaveCommandLast)
    {
      uint8_t command = static_cast<uint8_t>(wave & 0x0f);
      if (sid_wave_command_uses_speedtable(command) && param != 0 && !sid_speedtable_entry(param, nullptr, nullptr))
      {
        invalid++;
      }
    }
  }
  return invalid;
}

bool sid_invalid_wave_command(uint8_t command)
{
  return command == 0x00 || command == 0x08 || command == 0x0e;
}

uint8_t sid_count_invalid_wave_commands_locked()
{
  uint8_t invalid = 0;
  int len = static_cast<int>(sidcore_blob.table_len[kWTBL]);
  for (int i = 0; i < len; i++)
  {
    uint8_t wave = sidcore_blob.left[kWTBL][i];
    if (wave >= kWaveCommand && wave <= kWaveCommandLast)
    {
      uint8_t command = static_cast<uint8_t>(wave & 0x0f);
      if (sid_invalid_wave_command(command))
      {
        invalid++;
      }
    }
  }
  return invalid;
}

uint8_t sid_count_invalid_jump_targets_locked(int table)
{
  uint8_t invalid = 0;
  int len = static_cast<int>(sidcore_blob.table_len[table]);
  for (int i = 0; i < len; i++)
  {
    if (sidcore_blob.left[table][i] != kTableJump) continue;
    uint8_t target = sidcore_blob.right[table][i];
    if (target == 0) continue;

    bool ok = false;
    size_t target_index = sid_table_index(table, target, &ok);
    if (!ok || sidcore_blob.left[table][target_index] == kTableJump)
    {
      invalid++;
    }
  }
  return invalid;
}

bool sid_resolve_table_jump_locked(int table, uint8_t *ptr)
{
  if (!ptr || *ptr == 0) return false;

  bool ok = false;
  size_t index = sid_table_index(table, *ptr, &ok);
  if (!ok)
  {
    *ptr = 0;
    return false;
  }

  uint8_t left = sidcore_blob.left[table][index];
  if (left != kTableJump) return true;

  uint8_t target = sidcore_blob.right[table][index];
  if (target == 0)
  {
    *ptr = 0;
    return false;
  }

  bool target_ok = false;
  size_t target_index = sid_table_index(table, target, &target_ok);
  if (!target_ok || sidcore_blob.left[table][target_index] == kTableJump)
  {
    *ptr = 0;
    return false;
  }

  *ptr = target;
  return true;
}

void sid_apply_vibrato_step_locked(uint8_t cmp, uint8_t right)
{
  uint16_t speed = right;
  if (cmp >= 0x80)
  {
    cmp &= 0x7f;
    uint16_t note_step = sid_note_step_from_current_note();
    uint8_t shift = right & 0x1f;
    speed = static_cast<uint16_t>(note_step >> shift);
  }

  if ((sidcore_vib_time < 0x80) && (sidcore_vib_time > cmp))
  {
    sidcore_vib_time ^= 0xff;
  }
  sidcore_vib_time = static_cast<uint8_t>(sidcore_vib_time + 0x02);
  if (sidcore_vib_time & 0x01)
  {
    sidcore_current_freq = static_cast<uint16_t>(sidcore_current_freq - speed);
  }
  else
  {
    sidcore_current_freq = static_cast<uint16_t>(sidcore_current_freq + speed);
  }
  sid_write_voice1_locked();
}

uint8_t sid_waveform_bits(uint8_t value)
{
  return static_cast<uint8_t>(value & 0xf0);
}

uint8_t sid_initial_waveform_locked()
{
  // Prefer explicit firstwave waveform bits.
  uint8_t waveform = sid_waveform_bits(sidcore_current_instrument.firstWave);
  if (waveform) return waveform;

  // If firstwave has no waveform bits, try deriving an initial waveform from wavetable.
  uint8_t ptr = sidcore_blob.pointers[kWTBL];
  for (int i = 0; i < 128 && ptr != 0; i++)
  {
    uint8_t wave = sid_table_left(kWTBL, ptr);
    uint8_t note = sid_table_right(kWTBL, ptr);
    if (wave == 0 && note == 0) break;

    if (wave == kTableJump)
    {
      ptr = note;
      continue;
    }

    if (wave <= kWaveDelayLast)
    {
      ptr++;
      continue;
    }

    if (wave < kWaveSilent)
    {
      return static_cast<uint8_t>(wave & 0xfe);
    }

    if (wave >= kWaveCommand && wave <= kWaveCommandLast && (wave & 0x0f) == 0x07)
    {
      uint8_t command_wave = sid_waveform_bits(note);
      if (command_wave) return command_wave;
    }
    break;
  }

  // Final safety fallback to audible default.
  return 0x20;
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
  bool ok = false;
  size_t index = sid_table_index(table, ptr, &ok);
  if (!ok) return 0;
  return sidcore_blob.left[table][index];
}

uint8_t sid_table_right(int table, uint8_t ptr)
{
  bool ok = false;
  size_t index = sid_table_index(table, ptr, &ok);
  if (!ok) return 0;
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
  sid_instance->write(kFilterModeVolume, static_cast<uint8_t>(sidcore_filter_type | (sidcore_master_volume & 0x0f)));
}

void sid_step_filter_table_locked()
{
  if (sidcore_filtertable_ptr == 0) return;
  if (!sid_resolve_table_jump_locked(kFTBL, &sidcore_filtertable_ptr)) return;

  uint8_t left = sid_table_left(kFTBL, sidcore_filtertable_ptr);
  uint8_t right = sid_table_right(kFTBL, sidcore_filtertable_ptr);
  if (left == 0 && right == 0)
  {
    sidcore_filtertable_ptr = 0;
    return;
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
  if (!sid_resolve_table_jump_locked(kPTBL, &sidcore_pulsetable_ptr)) return;

  uint8_t left = sid_table_left(kPTBL, sidcore_pulsetable_ptr);
  uint8_t right = sid_table_right(kPTBL, sidcore_pulsetable_ptr);
  if (left == 0 && right == 0)
  {
    sidcore_pulsetable_ptr = 0;
    return;
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

void sid_step_tick_effects_locked()
{
  if (sidcore_speedtable_ptr == 0) return;
  if (sidcore_vib_delay > 1)
  {
    sidcore_vib_delay--;
    return;
  }

  uint8_t cmp = 0;
  uint8_t right = 0;
  if (!sid_speedtable_entry(sidcore_speedtable_ptr, &cmp, &right)) return;
  sid_apply_vibrato_step_locked(cmp, right);
}

void sid_execute_wave_command_locked(uint8_t command, uint8_t param)
{
  if (sid_invalid_wave_command(command))
  {
    // In GoatTracker these command nibbles are invalid in wavetable context.
    // We stop wavetable progression to avoid undefined behavior.
    sidcore_wavetable_ptr = 0;
    sidcore_wavetable_delay = 0;
    return;
  }

  switch (command)
  {
    case 0x01: // porta up (speedtable param)
    {
      uint16_t speed = sid_speed_from_speedtable(param);
      sidcore_current_freq = static_cast<uint16_t>(sidcore_current_freq + speed);
      sid_write_voice1_locked();
      break;
    }
    case 0x02: // porta down (speedtable param)
    {
      uint16_t speed = sid_speed_from_speedtable(param);
      sidcore_current_freq = static_cast<uint16_t>(sidcore_current_freq - speed);
      sid_write_voice1_locked();
      break;
    }
    case 0x03: // tone porta (speedtable param)
    {
      uint16_t target = sid_frequency_from_midi(sidcore_base_note);
      if (!param)
      {
        sidcore_current_freq = target;
        sidcore_last_note = sidcore_base_note;
        sidcore_vib_time = 0;
      }
      else
      {
        uint16_t speed = sid_speed_from_speedtable(param);
        if (sidcore_current_freq < target)
        {
          uint32_t next = static_cast<uint32_t>(sidcore_current_freq) + speed;
          if (next > target)
          {
            sidcore_current_freq = target;
            sidcore_last_note = sidcore_base_note;
            sidcore_vib_time = 0;
          }
          else
          {
            sidcore_current_freq = static_cast<uint16_t>(next);
          }
        }
        else if (sidcore_current_freq > target)
        {
          if (sidcore_current_freq - target <= speed)
          {
            sidcore_current_freq = target;
            sidcore_last_note = sidcore_base_note;
            sidcore_vib_time = 0;
          }
          else
          {
            sidcore_current_freq = static_cast<uint16_t>(sidcore_current_freq - speed);
          }
        }
      }
      sid_write_voice1_locked();
      break;
    }
    case 0x04: // vibrato (speedtable param)
    {
      uint8_t cmp = 0;
      uint8_t right = 0;
      if (!sid_speedtable_entry(param, &cmp, &right)) break;
      sid_apply_vibrato_step_locked(cmp, right);
      break;
    }
    case 0x05: // set AD
      sidcore_last_ad = param;
      if (sid_instance) sid_instance->write(kVoice1AttackDecay, sidcore_last_ad);
      break;
    case 0x06: // set SR
      sidcore_last_sr = param;
      if (sid_instance) sid_instance->write(kVoice1SustainRelease, sidcore_last_sr);
      break;
    case 0x07: // set wave
      // In GoatTracker this writes the control value directly (gate handled separately).
      sidcore_current_wave = static_cast<uint8_t>(param & 0xfe);
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
        sidcore_master_volume = param;
        sid_write_filter_locked();
      }
      break;
    default:
      break;
  }
}

bool sid_step_wave_table_locked()
{
  if (sidcore_wavetable_ptr == 0) return false;
  if (!sid_resolve_table_jump_locked(kWTBL, &sidcore_wavetable_ptr)) return false;

  uint8_t wave = sid_table_left(kWTBL, sidcore_wavetable_ptr);
  uint8_t note = sid_table_right(kWTBL, sidcore_wavetable_ptr);
  if (wave == 0 && note == 0)
  {
    sidcore_wavetable_ptr = 0;
    return false;
  }

  bool was_command = false;
  if (wave <= kWaveDelayLast)
  {
    if (sidcore_wavetable_delay != wave)
    {
      sidcore_wavetable_delay++;
      return false;
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
  (void)sid_resolve_table_jump_locked(kWTBL, &sidcore_wavetable_ptr);

  if (!was_command && note != 0x80)
  {
    int target = 0;
    if (note < 0x80)
    {
      target = static_cast<int>(sidcore_base_note) + static_cast<int>(note);
      if (target < 0) target = 0;
      if (target > 127) target = 127;
    }
    else
    {
      target = static_cast<int>(note & 0x7f);
    }
    sidcore_last_note = static_cast<uint8_t>(target);
    sidcore_vib_time = 0;
    sidcore_current_freq = sid_frequency_from_midi(static_cast<uint8_t>(target));
    sid_write_voice1_locked();
  }
  return was_command;
}

void sidcore_step_tables_locked()
{
  sid_step_filter_table_locked();
  bool wave_command = sid_step_wave_table_locked();
  if (!wave_command)
  {
    sid_step_tick_effects_locked();
  }
  sid_step_pulse_table_locked();
}

void sidcore_init_engine_locked()
{
  if (sid_instance) return;

  sid_instance = new SID();
  sid_instance->set_chip_model(sidcore_chip_model == 8580 ? MOS8580 : MOS6581);
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

void sidcore_rebase_table_pointers_locked()
{
  for (int i = 0; i < kTableCount; i++)
  {
    sidcore_table_base[i] = 0;
    uint8_t len = sidcore_blob.table_len[i];
    uint8_t ptr = sidcore_blob.pointers[i];
    if (len > 0 && ptr > len)
    {
      sidcore_table_base[i] = static_cast<uint8_t>(ptr - 1);
    }
  }
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
  sidcore_chip_model = 8580;
  sidcore_init_engine_locked();
  sid_instance->set_chip_model(MOS8580);
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

  sidcore_base_note = note;
  sidcore_last_note = note;
  sidcore_current_freq = sid_frequency_from_midi(note);
  sidcore_current_wave = sid_initial_waveform_locked();
  uint8_t gateflags = static_cast<uint8_t>(sidcore_current_instrument.gateTimer & 0xc0);
  if ((gateflags & 0x40) == 0)
  {
    sidcore_gate_on = false;
    sid_write_voice1_locked();
    if ((gateflags & 0x80) == 0 && sid_instance)
    {
      sid_instance->write(kVoice1AttackDecay, kHardRestartAD);
      sid_instance->write(kVoice1SustainRelease, kHardRestartSR);
    }
  }
  sidcore_apply_adsr_locked();
  sidcore_gate_on = true;
  sidcore_wavetable_ptr = sidcore_blob.pointers[kWTBL];
  sidcore_pulsetable_ptr = sidcore_blob.pointers[kPTBL];
  sidcore_filtertable_ptr = sidcore_blob.pointers[kFTBL];
  sidcore_wavetable_delay = 0;
  sidcore_pulse_time = 0;
  sidcore_filter_time = 0;
  sidcore_vib_time = 0;
  sidcore_vib_delay = sidcore_current_instrument.vibDelay;
  sidcore_speedtable_ptr = sidcore_blob.pointers[kSTBL];
  sidcore_tick_samples = sid_samples_per_tick(sidcore_sample_rate);
  sidcore_samples_until_tick = sidcore_tick_samples;
  sid_write_voice1_locked();
  sid_write_filter_locked();

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
  sidcore_master_volume = 0x0f;
  sidcore_apply_adsr_locked();
}

void sidcore_set_chip_model(int model)
{
  std::lock_guard<std::mutex> lock(sidcore_mutex);
  sidcore_chip_model = (model == 8580) ? 8580 : 6581;
  sidcore_init_engine_locked();
  sid_instance->set_chip_model(sidcore_chip_model == 8580 ? MOS8580 : MOS6581);
  sid_write_voice1_locked();
  sid_write_filter_locked();
  std::snprintf(sidcore_event_buffer, sizeof(sidcore_event_buffer), "SID chip model set: MOS%d", sidcore_chip_model);
}

int sidcore_get_chip_model(void)
{
  std::lock_guard<std::mutex> lock(sidcore_mutex);
  return sidcore_chip_model;
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

int sidcore_table_count(void)
{
  return kTableCount;
}

uint8_t sidcore_table_pointer(int table)
{
  if (!sid_table_valid_index(table)) return 0;
  std::lock_guard<std::mutex> lock(sidcore_mutex);
  return sidcore_blob.pointers[table];
}

int sidcore_set_table_pointer(int table, uint8_t pointer)
{
  if (!sid_table_valid_index(table)) return 0;
  std::lock_guard<std::mutex> lock(sidcore_mutex);
  sidcore_blob.pointers[table] = pointer;
  sidcore_rebase_table_pointers_locked();
  return 1;
}

uint8_t sidcore_table_length(int table)
{
  if (!sid_table_valid_index(table)) return 0;
  std::lock_guard<std::mutex> lock(sidcore_mutex);
  return sidcore_blob.table_len[table];
}

int sidcore_set_table_length(int table, uint8_t length)
{
  if (!sid_table_valid_index(table)) return 0;
  std::lock_guard<std::mutex> lock(sidcore_mutex);
  sidcore_blob.table_len[table] = length;
  sidcore_blob.left[table].resize(length, 0);
  sidcore_blob.right[table].resize(length, 0);
  sidcore_rebase_table_pointers_locked();
  return 1;
}

int sidcore_get_table_row(int table, uint8_t row_index, uint8_t *left, uint8_t *right)
{
  if (!sid_table_valid_index(table) || !left || !right) return 0;
  std::lock_guard<std::mutex> lock(sidcore_mutex);
  if (row_index >= sidcore_blob.table_len[table]) return 0;
  *left = sidcore_blob.left[table][row_index];
  *right = sidcore_blob.right[table][row_index];
  return 1;
}

int sidcore_set_table_row(int table, uint8_t row_index, uint8_t left, uint8_t right)
{
  if (!sid_table_valid_index(table)) return 0;
  std::lock_guard<std::mutex> lock(sidcore_mutex);
  if (row_index >= sidcore_blob.table_len[table]) return 0;
  sidcore_blob.left[table][row_index] = left;
  sidcore_blob.right[table][row_index] = right;
  return 1;
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
  uint8_t invalid_speed_refs;
  uint8_t invalid_wave_commands;
  uint8_t invalid_wtbl_jumps;
  uint8_t invalid_ptbl_jumps;
  uint8_t invalid_ftbl_jumps;
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
  sidcore_rebase_table_pointers_locked();
  invalid_speed_refs = sid_count_invalid_speed_refs_locked();
  invalid_wave_commands = sid_count_invalid_wave_commands_locked();
  invalid_wtbl_jumps = sid_count_invalid_jump_targets_locked(kWTBL);
  invalid_ptbl_jumps = sid_count_invalid_jump_targets_locked(kPTBL);
  invalid_ftbl_jumps = sid_count_invalid_jump_targets_locked(kFTBL);
  sidcore_init_engine_locked();
  sidcore_apply_adsr_locked();
  std::snprintf(sidcore_event_buffer,
                sizeof(sidcore_event_buffer),
                "Loaded GTI AD=%02X SR=%02X FW=%02X GT=%02X VD=%02X PTR[W=%02X P=%02X F=%02X S=%02X] LEN[W=%02X P=%02X F=%02X S=%02X] BASE[W=%02X P=%02X F=%02X S=%02X] WARN_ST=%02X WARN_CMD=%02X WARN_JW=%02X WARN_JP=%02X WARN_JF=%02X",
                ad,
                sr,
                firstwave,
                gatetimer,
                vibdelay,
                sidcore_blob.pointers[0],
                sidcore_blob.pointers[1],
                sidcore_blob.pointers[2],
                sidcore_blob.pointers[3],
                sidcore_blob.table_len[0],
                sidcore_blob.table_len[1],
                sidcore_blob.table_len[2],
                sidcore_blob.table_len[3],
                sidcore_table_base[0],
                sidcore_table_base[1],
                sidcore_table_base[2],
                sidcore_table_base[3],
                invalid_speed_refs,
                invalid_wave_commands,
                invalid_wtbl_jumps,
                invalid_ptbl_jumps,
                invalid_ftbl_jumps);
  return 1;
}
}
