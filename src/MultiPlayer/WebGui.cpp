#include "engine.h"
#include "MultiPlayer/WebGui.h"
#include "MultiPlayer/Server.h"
#include "MultiPlayer/Client.h"
#include "imgui.h"
#include <cerrno>
#include <chrono>
#include <cstdlib>

#include <thread>
#include <fstream>
#include <sstream>
#include <enet/enet.h>
#include <spdlog/spdlog.h>

std::string read_file(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) return "<html><body><p>Hello error, could not find file</p></body></html>";
	std::ostringstream ss;
	ss << file.rdbuf();
	return ss.str();
}


namespace MultiPlayer {
	namespace {
		int readWebGuiPort()
		{
			const char* portEnv = std::getenv("MULTIPLAYER_WEBGUI_PORT");
			if (!portEnv || !*portEnv) {
				return 7775;
			}

			errno = 0;
			char* end = nullptr;
			long parsedPort = std::strtol(portEnv, &end, 10);
			if (errno != 0 || end == portEnv || *end != '\0' || parsedPort <= 0 || parsedPort > 65535) {
				return 7775;
			}

			return static_cast<int>(parsedPort);
		}

		bool parsePort(const std::string& value, int& port)
		{
			errno = 0;
			char* end = nullptr;
			const long parsed = std::strtol(value.c_str(), &end, 10);
			if (errno != 0 || end == value.c_str() || *end != '\0' ||
			    parsed <= 0 || parsed > 65535) {
				return false;
			}
			port = static_cast<int>(parsed);
			return true;
		}
	}

	WebGui::WebGui(Engine* engine)
		: port(readWebGuiPort())
		, engine(engine)
	{
		CROW_ROUTE(app, "/")([](){
				return read_file("src/MultiPlayer/WebGui.html");
				});

		CROW_ROUTE(app, "/multiplayer/connect").methods(crow::HTTPMethod::POST)
			([this, engine](const crow::request& req) {
			 auto params = req.get_body_params();

			 const char* hostValue = params.get("host");
			 const char* portValue = params.get("port");

			 if (!hostValue || !portValue || !*hostValue) {
				 return crow::response(400, "missing host or port");
			 }

			 int requestedPort = 0;
			 if (!parsePort(portValue, requestedPort)) {
				 return crow::response(400, "invalid port");
			 }

			 // The request is queued; the render thread owns ENet and all
			 // Client lifetime/GL cleanup.
			 engine->requestMultiplayerConnect(hostValue, requestedPort);
			 return crow::response(202, "connection queued");
			 });

		serverThread = std::thread([this]() {
			started.store(true);
			try {
				app.port(port).multithreaded().run();
			} catch (const std::exception& error) {
				spdlog::error("WebGui: server stopped: {}", error.what());
			} catch (...) {
				spdlog::error("WebGui: server stopped with unknown error");
			}
		});
	}

	WebGui::~WebGui()
	{
		// Give the thread a chance to enter run() so stop() is observed.
		for (int i = 0; i < 100 && !started.load(); ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		app.stop();
		if (serverThread.joinable()) {
			serverThread.join();
		}
	}

	void WebGui::Render() {}
}
