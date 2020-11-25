/******************************************************************************
    Copyright (C) 2016-2019 by Streamlabs (General Workings Inc)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

******************************************************************************/

#include <chrono>
#include <inttypes.h>
#include <iostream>
#include <ipc-class.hpp>
#include <ipc-function.hpp>
#include <ipc-server.hpp>
#include <memory>
#include <thread>
#include <vector>
#include "error.hpp"
#include "nodeobs_api.h"
#include "nodeobs_autoconfig.h"
#include "nodeobs_content.h"
#include "nodeobs_service.h"
#include "nodeobs_settings.h"
#include "osn-fader.hpp"
#include "osn-filter.hpp"
#include "osn-global.hpp"
#include "osn-input.hpp"
#include "osn-module.hpp"
#include "osn-properties.hpp"
#include "osn-scene.hpp"
#include "osn-sceneitem.hpp"
#include "osn-source.hpp"
#include "osn-transition.hpp"
#include "osn-video.hpp"
#include "osn-volmeter.hpp"
#include "callback-manager.h"

#include "util-crashmanager.h"

#include "shared.hpp"

#ifdef __APPLE__
#include <unistd.h>
#endif

#if defined(_WIN32)
#include "Shlobj.h"

// Checks ForceGPUAsRenderDevice setting
extern "C" __declspec(dllexport) DWORD NvOptimusEnablement = [] {
	LPWSTR       roamingPath;
	std::wstring filePath;
	std::string  line;
	std::fstream file;
	bool         settingValue = true; // Default value (NvOptimusEnablement = 1)

	if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roamingPath))) {
		// Couldn't find roaming app data folder path, assume default value
		return settingValue;
	} else {
		filePath.assign(roamingPath);
		filePath.append(L"\\slobs-client\\basic.ini");
		CoTaskMemFree(roamingPath);
	}

	file.open(filePath);

	if (file.is_open()) {
		while (std::getline(file, line)) {
			if (line.find("ForceGPUAsRenderDevice", 0) != std::string::npos) {
				if (line.substr(line.find('=') + 1) == "false") {
					settingValue = false;
					file.close();
					break;
				}
			}
		}
	} else {
		//Couldn't open config file, assume default value
		return settingValue;
	}

	// Return setting value
	return settingValue;
}();
#endif

#define BUFFSIZE 512

struct ServerData
{
	std::mutex                                     mtx;
	std::chrono::high_resolution_clock::time_point last_connect, last_disconnect;
	size_t                                         count_connected = 0;
};

bool ServerConnectHandler(void* data, int64_t)
{
	ServerData*                  sd = reinterpret_cast<ServerData*>(data);
	std::unique_lock<std::mutex> ulock(sd->mtx);
	sd->last_connect = std::chrono::high_resolution_clock::now();
	sd->count_connected++;
	return true;
}

void ServerDisconnectHandler(void* data, int64_t)
{
	ServerData*                  sd = reinterpret_cast<ServerData*>(data);
	std::unique_lock<std::mutex> ulock(sd->mtx);
	sd->last_disconnect = std::chrono::high_resolution_clock::now();
	sd->count_connected--;
}

namespace System
{
	static void
	    Shutdown(void* data, const int64_t id, const std::vector<ipc::value>& args, std::vector<ipc::value>& rval)
	{
		bool* shutdown = (bool*)data;
		*shutdown      = true;
#ifdef __APPLE__
		g_util_osx->stopApplication();
#endif
		rval.push_back(ipc::value((uint64_t)ErrorCode::Ok));
		return;
	}
} // namespace System

int main(int argc, char* argv[])
{
#ifdef __APPLE__
	g_util_osx = new UtilInt();
	g_util_osx->init();
#endif
	std::string socketPath      = "";
	std::string receivedVersion = "";
#ifdef __APPLE__
	socketPath = "/tmp/";
#endif
	if (argc != 3) {
		std::cerr << "Version mismatch. Expected <socketpath> <version> params";
		return -3;
	}

	socketPath += argv[1];
	receivedVersion = argv[2];
	// Check versions
	std::string myVersion = OSN_VERSION;
	if (receivedVersion != myVersion) {
		std::cerr << "Versions mismatch. Server version: " << myVersion << "but received client version: " << receivedVersion;
		return -3;
	}

	// Usage:
	// argv[0] = Path to this application. (Usually given by default if run via path-based command!)
	// argv[1] = Path to a named socket.
	// argv[2] = version from client ; must match the server version

	// Instance
	ipc::server myServer;
	bool        doShutdown = false;
	ServerData  sd;
	sd.last_disconnect = sd.last_connect = std::chrono::high_resolution_clock::now();
	sd.count_connected                   = 0;

	// Classes
	/// System
	{
		std::shared_ptr<ipc::collection> cls = std::make_shared<ipc::collection>("System");
		cls->register_function(
		    std::make_shared<ipc::function>("Shutdown", std::vector<ipc::type>{}, System::Shutdown, &doShutdown));
		myServer.register_collection(cls);
	};

	/// OBS Studio Node
	osn::Global::Register(myServer);
	osn::Source::Register(myServer);
	osn::Input::Register(myServer);
	osn::Filter::Register(myServer);
	osn::Transition::Register(myServer);
	osn::Scene::Register(myServer);
	osn::SceneItem::Register(myServer);
	osn::Fader::Register(myServer);
	osn::Volmeter::Register(myServer);
	osn::Properties::Register(myServer);
	osn::Video::Register(myServer);
	osn::Module::Register(myServer);
	CallbackManager::Register(myServer);
	OBS_API::Register(myServer);
	OBS_content::Register(myServer);
	OBS_service::Register(myServer);
	OBS_settings::Register(myServer);
	OBS_settings::Register(myServer);
	autoConfig::Register(myServer);

	OBS_API::CreateCrashHandlerExitPipe();

	// Register Connect/Disconnect Handlers
	myServer.set_connect_handler(ServerConnectHandler, &sd);
	myServer.set_disconnect_handler(ServerDisconnectHandler, &sd);

	// Initialize Server
	try {
		myServer.initialize(socketPath.c_str());
	} catch (std::exception& e) {
		std::cerr << "Initialization failed with error " << e.what() << "." << std::endl;
		return -2;
	} catch (...) {
		std::cerr << "Failed to initialize server" << std::endl;
		return -2;
	}

	// Reset Connect/Disconnect time.
	sd.last_disconnect = sd.last_connect = std::chrono::high_resolution_clock::now();

#ifdef __APPLE__
	// WARNING: Blocking function -> this won't return until the application
	// receives a stop or terminate event
	g_util_osx->runApplication();
#endif
#ifdef WIN32
	bool waitBeforeClosing = false;
	while (!doShutdown) {
		if (sd.count_connected == 0) {
			auto tp    = std::chrono::high_resolution_clock::now();
			auto delta = tp - sd.last_disconnect;
			if (std::chrono::duration_cast<std::chrono::milliseconds>(delta).count() > 5000) {
				doShutdown = true;
				waitBeforeClosing = true;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	// Wait for crash handler listening thread to finish.
	// flag waitBeforeClosing: server process expect to receive the exit message from the crash-handler 
	// before going further with shutdown. It needed for usecase where obs64 process stay alive and 
	// continue streaming till user confirms exit in crash-handler.
	OBS_API::WaitCrashHandlerClose(waitBeforeClosing);
#endif
	osn::Source::finalize_global_signals();
	OBS_API::destroyOBS_API();

	// Finalize Server
	myServer.finalize();

	return 0;
}
