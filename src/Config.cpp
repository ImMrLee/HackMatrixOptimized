#include "Config.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

std::optional<fs::path>
envPath(const char* name)
{
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return std::nullopt;
  }
  return fs::path(value);
}

} // namespace

Config::Config()
{
  reload();
}

void
Config::reload()
{
  configPath = resolveConfigPath();
  std::ifstream ifs(configPath);
  if (!ifs.is_open()) {
    throw std::runtime_error("Unable to open HackMatrix config: " + configPath);
  }
  config = fkyaml::node::deserialize(ifs);
}

std::shared_ptr<Config> Config::_singleton = nullptr;

std::string
Config::resolveConfigPath()
{
  std::vector<fs::path> candidates;

  if (auto explicitPath = envPath("HACKMATRIX_CONFIG_FILE")) {
    return explicitPath->string();
  }

  if (auto configHome = envPath("HACKMATRIX_CONFIG_HOME")) {
    candidates.push_back(*configHome / "config.yaml");
  }

  if (auto xdgConfigHome = envPath("XDG_CONFIG_HOME")) {
    candidates.push_back(*xdgConfigHome / "HackMatrix" / "config.yaml");
  }

  if (auto home = envPath("HOME")) {
    candidates.push_back(*home / ".config" / "HackMatrix" / "config.yaml");
  }

  for (const auto& candidate : candidates) {
    if (fs::exists(candidate)) {
      return candidate.string();
    }
  }

  if (!candidates.empty()) {
    return candidates.front().string();
  }

  return "config.yaml";
}

std::shared_ptr<Config>
Config::singleton()
{
  if (_singleton == nullptr) {
    _singleton = std::make_shared<Config>();
  }
  return _singleton;
}

const std::string&
Config::getConfigPath() const
{
  return configPath;
}

std::vector<std::string>
split_path(const std::string& path)
{
  std::vector<std::string> parts;
  size_t start = 0, end;

  while ((end = path.find('.', start)) != std::string::npos) {
    parts.push_back(path.substr(start, end - start));
    start = end + 1;
  }
  parts.push_back(path.substr(start));
  return parts;
}

std::vector<std::string>
Config::get_keys(const std::string& key_path)
{
  std::vector<std::string> keys;
  auto parts = split_path(key_path);
  fkyaml::node* current = &config;

  // Navigate to the requested node
  for (const auto& part : parts) {
    current = &(*current)[part];
  }

  if(current->is_mapping()) {
  auto keyPairs = current->get_value<std::map<std::string, std::string>>();

    // Collect all keys in the node
    for (const auto& pair : keyPairs) {
        keys.push_back(pair.first);
    }
  }

  return keys;
}

void
Config::setIntAndSave(const std::string& key, int value)
{
  if (configPath.empty()) {
    throw std::runtime_error("HackMatrix config path is empty");
  }

  const fs::path path(configPath);
  std::ifstream input(path);
  if (!input.is_open()) {
    throw std::runtime_error("Unable to open HackMatrix config: " +
                             configPath);
  }

  std::vector<std::string> lines;
  std::string line;
  bool replaced = false;
  const std::string prefix = key + ":";
  while (std::getline(input, line)) {
    std::string trimmed = line;
    const auto first = trimmed.find_first_not_of(" \t");
    if (first != std::string::npos) {
      trimmed.erase(0, first);
    }
    if (!replaced && trimmed.rfind(prefix, 0) == 0) {
      line = key + ": " + std::to_string(value);
      replaced = true;
    }
    lines.push_back(line);
  }
  if (!replaced) {
    lines.push_back(key + ": " + std::to_string(value));
  }

  fs::path temporary = path;
  temporary += ".tmp";
  std::ofstream output(temporary, std::ios::trunc);
  if (!output.is_open()) {
    throw std::runtime_error("Unable to write HackMatrix config: " +
                             temporary.string());
  }
  for (const auto& entry : lines) {
    output << entry << '\n';
  }
  output.close();

  std::error_code ec;
  fs::rename(temporary, path, ec);
  if (ec) {
    fs::remove(temporary, ec);
    throw std::runtime_error("Unable to replace HackMatrix config: " +
                             path.string());
  }

  config[key] = value;
}
