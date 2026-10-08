#pragma once

#include "CheckpointLuaView.h"
#include "CheckpointArchive.h"
#include "System.h"

#include <memory_resource>
#include <memory>
#include <string>
#include <string_view>
#include <cstddef>
#include <functional>
#include <format>
#include <limits>
#include <vector>
#include <chrono>
#include <thread>

namespace RTE::CheckpointLua {
	// The callback descriptor is capture-owned. Keep its live function tokens and
	// scalar fields here; its Lua tables are made only by the saver VM.
	struct CallbackImage {
		struct Function {
			TValue value;
			std::pmr::string path;
			bool enabled;
			Function(const TValue& token, std::string_view file, bool on, std::pmr::memory_resource* resource) : value(token), path(file.substr(0, file.find('\0')), resource), enabled(on) {}
		};
		struct Group {
			std::pmr::string name;
			std::pmr::vector<Function> functions;
			Group(std::string_view key, std::pmr::memory_resource* resource) : name(key, resource), functions(resource) {}
		};
		struct Object {
			std::pmr::string uid;
			std::pmr::vector<Group> groups;
			Object(std::string_view key, std::pmr::memory_resource* resource) : uid(key, resource), groups(resource) {}
		};
		std::shared_ptr<std::pmr::memory_resource> backing = CheckpointBuffer::LeaseCaptureStorage();
		std::pmr::monotonic_buffer_resource storage{backing ? backing.get() : std::pmr::get_default_resource()};
		std::pmr::vector<Object> objects{&storage};
		std::pmr::vector<TValue> roots{&storage};
		std::thread::id captureThread = std::this_thread::get_id();
		size_t stateIndex = 0;
		bool report = false;

		void Push(lua_State* state, View& view) const {
			const auto started = report ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			const auto table = [&](size_t array, size_t fields) {
				const auto hint = [](size_t count) { return count <= static_cast<size_t>(std::numeric_limits<int>::max()) ? static_cast<int>(count) : 0; };
				lua_createtable(state, hint(array), hint(fields));
				view.MarkLocalScratch(state, -1);
			};
			table(0, objects.size());
			for (const Object& object: objects) {
				table(0, object.groups.size());
				for (const Group& group: object.groups) {
					table(group.functions.size(), 0);
					int index = 1;
					for (const Function& function: group.functions) {
						table(0, 3);
						view.Push(state, function.value); lua_setfield(state, -2, "function");
						lua_pushlstring(state, function.path.data(), function.path.size()); lua_setfield(state, -2, "path");
						lua_pushboolean(state, function.enabled); lua_setfield(state, -2, "enabled");
						lua_rawseti(state, -2, index++);
					}
					lua_setfield(state, -2, group.name.c_str());
				}
				lua_setfield(state, -2, object.uid.c_str());
			}
			if (report) System::PrintDiagnosticLine(std::format("[checkpoint-callback-format] state={} objects={} functions={} capture_thread={} worker_thread={} off_capture_thread={} us={}",
				stateIndex, objects.size(), roots.size(), std::hash<std::thread::id>{}(captureThread), std::hash<std::thread::id>{}(std::this_thread::get_id()), captureThread != std::this_thread::get_id(),
				std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count()));
		}
	};
}
