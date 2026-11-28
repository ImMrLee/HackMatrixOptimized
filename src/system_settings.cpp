#include "system_settings.h"
#include "time_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::string
runCommand(const std::string& command)
{
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return {};
  }
  std::string output;
  char buffer[256];
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }
  pclose(pipe);
  return output;
}

bool
existsExecutable(const char* path)
{
  return access(path, X_OK) == 0;
}

int
parseLastPercent(const std::string& text)
{
  const auto percent = text.find('%');
  if (percent == std::string::npos) {
    return -1;
  }
  std::size_t end = percent;
  while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  std::size_t start = end;
  while (start > 0 &&
         std::isdigit(static_cast<unsigned char>(text[start - 1]))) {
    --start;
  }
  if (start == end) {
    return -1;
  }
  return std::atoi(text.substr(start, end - start).c_str());
}

int
readIntFile(const std::string& path)
{
  std::ifstream input(path);
  int value = 0;
  if (!(input >> value)) {
    return 0;
  }
  return value;
}

} // namespace

void
SystemSettings::refreshAudioIfDue(double now)
{
  if (now - lastAudioRefresh_ < 0.5) {
    return;
  }
  refreshAudio();
}

void
SystemSettings::refreshAudio()
{
  lastAudioRefresh_ = nowSeconds();
  audio_ = AudioSettings{};

  if (!backendChecked_) {
    backendChecked_ = true;
    if (existsExecutable("/usr/bin/wpctl")) {
      backend_ = 1;
    } else if (existsExecutable("/usr/bin/pactl")) {
      backend_ = 2;
    } else if (existsExecutable("/usr/bin/amixer")) {
      backend_ = 3;
    }
  }

  if (backend_ == 1) {
    audio_.backend = "pipewire";
    const std::string output = runCommand(
      "wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null");
    const auto volume = output.find("Volume:");
    if (volume != std::string::npos) {
      const float value = static_cast<float>(
        std::atof(output.c_str() + volume + std::string("Volume:").size()));
      audio_.volume = std::clamp(value, 0.0f, 1.5f);
      audio_.muted = output.find("[MUTED]") != std::string::npos;
      audio_.available = true;
      audio_.backend = "pipewire";
    }
  } else if (backend_ == 2) {
    audio_.backend = "pulseaudio";
    const std::string volume =
      runCommand("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null");
    const int percent = parseLastPercent(volume);
    if (percent >= 0) {
      audio_.volume = std::clamp(percent / 100.0f, 0.0f, 1.5f);
      const std::string mute =
        runCommand("pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null");
      audio_.muted = mute.find("yes") != std::string::npos;
      audio_.available = true;
      audio_.backend = "pulseaudio";
    }
  } else if (backend_ == 3) {
    audio_.backend = "alsa";
    const std::string output = runCommand("amixer get Master 2>/dev/null");
    const int percent = parseLastPercent(output);
    if (percent >= 0) {
      audio_.volume = std::clamp(percent / 100.0f, 0.0f, 1.5f);
      audio_.muted = output.find("[off]") != std::string::npos;
      audio_.available = true;
      audio_.backend = "alsa";
    }
  }
}

bool
SystemSettings::setVolume(float volume01)
{
  if (!audio_.available) {
    return false;
  }
  const int percent = static_cast<int>(
    std::lround(std::clamp(volume01, 0.0f, 1.0f) * 100.0f));
  if (backend_ == 1) {
    runCommand("wpctl set-volume @DEFAULT_AUDIO_SINK@ " +
               std::to_string(percent) + "%");
  } else if (backend_ == 2) {
    runCommand("pactl set-sink-volume @DEFAULT_SINK@ " +
               std::to_string(percent) + "%");
  } else if (backend_ == 3) {
    runCommand("amixer sset Master " + std::to_string(percent) + "%");
  } else {
    return false;
  }
  refreshAudio();
  return true;
}

bool
SystemSettings::toggleMute()
{
  if (!audio_.available) {
    return false;
  }
  if (backend_ == 1) {
    runCommand("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
  } else if (backend_ == 2) {
    runCommand("pactl set-sink-mute @DEFAULT_SINK@ toggle");
  } else if (backend_ == 3) {
    runCommand("amixer sset Master toggle");
  } else {
    return false;
  }
  refreshAudio();
  return true;
}

void
SystemSettings::refreshBrightnessIfDue(double now)
{
  if (now - lastBrightnessRefresh_ < 0.5) {
    return;
  }
  refreshBrightness();
}

void
SystemSettings::refreshBrightness()
{
  lastBrightnessRefresh_ = nowSeconds();
  brightness_ = BrightnessSettings{};
  if (!brightnessPath_.empty()) {
    brightness_.device =
      std::filesystem::path(brightnessPath_).parent_path().filename().string();
  }

  if (!brightnessDeviceChecked_) {
    brightnessDeviceChecked_ = true;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root("/sys/class/backlight");
    if (fs::exists(root, ec) && fs::is_directory(root, ec)) {
      for (const auto& entry : fs::directory_iterator(root, ec)) {
        const fs::path brightness = entry.path() / "brightness";
        const fs::path maximum = entry.path() / "max_brightness";
        if (fs::exists(brightness, ec) && fs::exists(maximum, ec)) {
          brightnessPath_ = brightness.string();
          brightnessMaxPath_ = maximum.string();
          brightness_.device = entry.path().filename().string();
          break;
        }
      }
    }
  }

  if (brightnessPath_.empty() || brightnessMaxPath_.empty()) {
    return;
  }

  brightness_.current = readIntFile(brightnessPath_);
  brightness_.max = readIntFile(brightnessMaxPath_);
  if (brightness_.max <= 0) {
    return;
  }
  brightness_.available = true;
  brightness_.writable =
    access(brightnessPath_.c_str(), W_OK) == 0 ||
    existsExecutable("/usr/bin/brightnessctl") ||
    existsExecutable("/usr/bin/busctl") ||
    existsExecutable("/usr/bin/gdbus");
}

bool
SystemSettings::setBrightness(float percent01)
{
  if (!brightness_.available || !brightness_.writable) {
    return false;
  }
  const float percent = std::clamp(percent01, 0.0f, 1.0f);
  const int value = static_cast<int>(
    std::lround(percent * static_cast<float>(brightness_.max)));
  if (access(brightnessPath_.c_str(), W_OK) == 0) {
    std::ofstream output(brightnessPath_, std::ios::trunc);
    if (output.is_open()) {
      output << value;
      output.close();
    }
  } else if (existsExecutable("/usr/bin/brightnessctl")) {
    runCommand("brightnessctl set " +
               std::to_string(static_cast<int>(std::lround(percent * 100.0f))) +
               "%");
  } else {
    if (brightness_.device.empty()) {
      return false;
    }
    // Direct sysfs writes need root/udev; logind can set brightness for the
    // active local session without extra privileges.
    std::string command;
    if (existsExecutable("/usr/bin/busctl")) {
      command =
        "busctl call org.freedesktop.login1 "
        "/org/freedesktop/login1/session/auto "
        "org.freedesktop.login1.Session SetBrightness ssu backlight " +
        brightness_.device + " " + std::to_string(value) +
        " >/dev/null 2>&1";
    } else if (existsExecutable("/usr/bin/gdbus")) {
      command =
        "gdbus call --system --dest org.freedesktop.login1 "
        "--object-path /org/freedesktop/login1/session/auto "
        "--method org.freedesktop.login1.Session.SetBrightness backlight " +
        brightness_.device + " " + std::to_string(value) +
        " >/dev/null 2>&1";
    }
    if (command.empty() || std::system(command.c_str()) != 0) {
      return false;
    }
  }
  refreshBrightness();
  return true;
}
