// Unit tests for persisting scalar settings in config.yaml.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "Config.h"
#include "MultiPlayer/Client.h"

namespace fs = std::filesystem;

namespace {

class ConfigPersistenceTest : public ::testing::Test
{
protected:
  fs::path dir;
  fs::path file;
  std::shared_ptr<Config> previousSingleton;
  std::string previousConfigFile;
  bool hadConfigFile = false;

  void SetUp() override
  {
    dir = fs::temp_directory_path() / "hackmatrix-config-persistence-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    file = dir / "config.yaml";

    std::ofstream output(file);
    output << "foo: 1\n"
           << "multiplayer_window_stream_fps: 5\n"
           << "multiplayer_window_stream_quality: 65\n"
           << "nested:\n"
           << "  keep: true\n";
    output.close();

    if (const char* current = std::getenv("HACKMATRIX_CONFIG_FILE")) {
      previousConfigFile = current;
      hadConfigFile = true;
    }

    previousSingleton = Config::_singleton;
    Config::_singleton = nullptr;
    setenv("HACKMATRIX_CONFIG_FILE", file.string().c_str(), 1);
  }

  void TearDown() override
  {
    Config::_singleton = previousSingleton;
    if (hadConfigFile) {
      setenv("HACKMATRIX_CONFIG_FILE", previousConfigFile.c_str(), 1);
    } else {
      unsetenv("HACKMATRIX_CONFIG_FILE");
    }
    fs::remove_all(dir);
  }
};

} // namespace

TEST_F(ConfigPersistenceTest, ReadsExistingAndFallbackValues)
{
  auto config = Config::singleton();
  EXPECT_EQ(config->get_or<int>("multiplayer_window_stream_fps", 0), 5);
  EXPECT_EQ(config->get_or<int>("multiplayer_window_stream_quality", 0), 65);
  EXPECT_EQ(config->get_or<int>("missing_key", 42), 42);
}

TEST_F(ConfigPersistenceTest, RewritesExistingKeyAndPreservesRestOfFile)
{
  auto config = Config::singleton();
  config->setIntAndSave("multiplayer_window_stream_fps", 12);

  std::ifstream input(file);
  std::string contents((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
  EXPECT_NE(contents.find("multiplayer_window_stream_fps: 12"),
            std::string::npos);
  EXPECT_NE(contents.find("multiplayer_window_stream_quality: 65"),
            std::string::npos);
  EXPECT_NE(contents.find("nested:\n  keep: true"), std::string::npos);

  config->reload();
  EXPECT_EQ(config->get_or<int>("multiplayer_window_stream_fps", 0), 12);
}

TEST_F(ConfigPersistenceTest, AppendsNewKey)
{
  auto config = Config::singleton();
  config->setIntAndSave("brand_new_setting", 77);
  config->reload();
  EXPECT_EQ(config->get_or<int>("brand_new_setting", 0), 77);
}

TEST_F(ConfigPersistenceTest, ClientSettersPersist)
{
  auto client = std::make_shared<MultiPlayer::Client>(nullptr, true);
  client->setWindowStreamFps(9);
  client->setWindowStreamQuality(72);

  Config::_singleton = nullptr;
  auto config = Config::singleton();
  EXPECT_EQ(config->get_or<int>("multiplayer_window_stream_fps", 0), 9);
  EXPECT_EQ(config->get_or<int>("multiplayer_window_stream_quality", 0), 72);
  client.reset();
}
