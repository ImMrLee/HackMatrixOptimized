#include <gtest/gtest.h>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <signal.h>

namespace fs = std::filesystem;

static bool file_nonempty(const std::string& path)
{
  std::ifstream in(path);
  return in.good() && in.peek() != std::ifstream::traits_type::eof();
}

static bool wait_for_log_line_contains(const std::string& path,
                                       const std::string& needle,
                                       std::string* out_line = nullptr,
                                       int attempts = 150,
                                       int sleep_ms = 100)
{
  for (int i = 0; i < attempts; ++i) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
      if (line.find(needle) != std::string::npos) {
        if (out_line) {
          *out_line = line;
        }
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  return false;
}

static std::string rstrip(const std::string& in)
{
  size_t end = in.find_last_not_of(" \t\r\n");
  if (end == std::string::npos) {
    return "";
  }
  return in.substr(0, end + 1);
}

class WaylandBasicTest : public ::testing::Test
{
protected:
  std::string logFile = "/tmp/matrix-wlroots-output.log";
  pid_t compPid = -1;

  void SetUp() override
  {
    // Kill any existing compositor to avoid conflicts.
    system("pkill -f matrix-wlroots >/dev/null 2>&1");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    // Clear log
    std::ofstream(logFile, std::ios::trunc).close();

    // Ensure runtime dir exists
    const char* user = std::getenv("USER");
    std::string xdg = "/tmp/xdg-runtime-";
    xdg += (user ? user : "user");
    std::filesystem::create_directories(xdg);
    std::error_code ec;
    std::filesystem::permissions(xdg,
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace,
                                 ec);
    setenv("XDG_RUNTIME_DIR", xdg.c_str(), 1);
    unsetenv("WAYLAND_DISPLAY");
    // engine.cpp only writes its bootstrap line when this is set; it is the
    // file the assertions below read.
    setenv("MATRIX_WLROOTS_OUTPUT", logFile.c_str(), 1);

    // Verify launch exists in current dir
    ASSERT_TRUE(fs::exists("./launch")) << "launch script not found";

    // Start compositor
    compPid = fork();
    if (compPid == 0) {
#ifdef MATRIX_TEST_DEFAULT_COMPOSITOR_BIN
      // Point launch at the binary this test was built against, unless the
      // caller already chose one explicitly.
      if (!std::getenv("MATRIX_WLROOTS_BIN")) {
        setenv("MATRIX_WLROOTS_BIN", MATRIX_TEST_DEFAULT_COMPOSITOR_BIN, 1);
      }
#endif
      execl("/bin/bash", "bash", "-lc", "./launch --headless --in-wm", (char*)nullptr);
      std::exit(127);
    }
    ASSERT_GT(compPid, 0);
  }

  void TearDown() override
  {
    if (compPid > 0) {
      kill(compPid, SIGTERM);
      std::this_thread::sleep_for(std::chrono::seconds(2));
      kill(compPid, SIGKILL);
      std::this_thread::sleep_for(std::chrono::seconds(2));
      compPid = -1;
    }
    system("pkill -f matrix-wlroots >/dev/null 2>&1");
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
};

TEST_F(WaylandBasicTest, LogsAreWrittenAfterStartup)
{
  // Wait up to ~6s for the compositor to write something.
  bool wrote = false;
  for (int i = 0; i < 120; ++i) { // up to ~12s
    if (file_nonempty(logFile)) {
      wrote = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  EXPECT_TRUE(wrote) << "Compositor log is empty after startup";
}

TEST_F(WaylandBasicTest, LogsEngineBootstrapLine)
{
  // engine.cpp writes "engine: VOXEL_API_BIND=..." to MATRIX_WLROOTS_OUTPUT when
  // it initialises. The older "startup: ..." instrumentation these tests used
  // to assert on has been removed from the engine.
  std::string line;
  bool found = wait_for_log_line_contains(
    logFile, "engine: VOXEL_API_BIND=", &line, 200, 100);
  EXPECT_TRUE(found) << "Expected engine bootstrap log line";
  if (found) {
    auto pos = line.find("VOXEL_API_BIND=");
    ASSERT_NE(pos, std::string::npos);
    std::string value =
      rstrip(line.substr(pos + std::string("VOXEL_API_BIND=").size()));
    EXPECT_FALSE(value.empty());
    EXPECT_NE(value, "(null)");
  }
}

TEST_F(WaylandBasicTest, CompositorStaysAliveAfterStartup)
{
  // A crash-on-startup regression would show up as the child exiting quickly.
  std::this_thread::sleep_for(std::chrono::seconds(3));
  ASSERT_GT(compPid, 0);
  EXPECT_EQ(kill(compPid, 0), 0)
    << "Compositor exited within a few seconds of startup";
}

int main(int argc, char** argv)
{
  // Allow focusing on a single test by default; use --all to run everything or
  // set WAYLAND_TEST_FILTER to override the default filter. Falls back to
  // WaylandBasicTest.LogsEngineBootstrapLine to keep runs short.
  std::string defaultFilter = "WaylandBasicTest.LogsEngineBootstrapLine";
  const char* envFilter = std::getenv("WAYLAND_TEST_FILTER");
  bool runAll = false;

  // Strip --all from argv so gtest doesn't complain.
  std::vector<char*> args;
  args.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--all") {
      runAll = true;
      continue;
    }
    args.push_back(argv[i]);
  }
  args.push_back(nullptr);
  int newArgc = static_cast<int>(args.size()) - 1;

  ::testing::InitGoogleTest(&newArgc, args.data());

  if (!runAll) {
    if (envFilter && *envFilter) {
      ::testing::GTEST_FLAG(filter) = envFilter;
    } else {
      ::testing::GTEST_FLAG(filter) = defaultFilter;
    }
  }

  return RUN_ALL_TESTS();
}
