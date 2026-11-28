#pragma once

#include <string>

struct AudioSettings
{
  bool available = false;
  bool muted = false;
  float volume = 0.0f; // 0..1
  std::string backend;
};

struct BrightnessSettings
{
  bool available = false;
  bool writable = false;
  int current = 0;
  int max = 0;
  std::string device;
};

// Small, dependency-free wrappers around the host's audio/backlight controls.
// Values are cached and refreshed lazily by the System Config tab so we do not
// spawn a process every frame.
class SystemSettings
{
public:
  void refreshAudioIfDue(double now);
  void refreshAudio();
  bool setVolume(float volume01);
  bool toggleMute();
  const AudioSettings& audio() const { return audio_; }

  void refreshBrightnessIfDue(double now);
  void refreshBrightness();
  bool setBrightness(float percent01);
  const BrightnessSettings& brightness() const { return brightness_; }

private:
  AudioSettings audio_;
  BrightnessSettings brightness_;
  double lastAudioRefresh_ = -1.0e9;
  double lastBrightnessRefresh_ = -1.0e9;
  bool backendChecked_ = false;
  int backend_ = 0; // 1 wpctl, 2 pactl, 3 amixer
  bool brightnessDeviceChecked_ = false;
  std::string brightnessPath_;
  std::string brightnessMaxPath_;
};
