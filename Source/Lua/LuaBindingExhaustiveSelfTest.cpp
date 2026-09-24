// Make sure this file is always set to NOT use pre-compiled headers and conformance mode (/permissive) otherwise everything will be on fire!

#include "LuaBindingExhaustiveSelfTest.h"

#include "LuabindDefinitions.h"
#include "luabind/detail/class_registry.hpp"
#include "luabind/detail/class_rep.hpp"
#include "luabind/detail/construct_rep.hpp"
#include "luabind/detail/implicit_cast.hpp"
#include "luabind/detail/method_rep.hpp"
#include "luabind/detail/object_rep.hpp"
#include "luabind/detail/overload_rep.hpp"
#include "luabind/detail/ref.hpp"

#include "Activity.h"
#include "ActivityMan.h"
#include "AEmitter.h"
#include "Actor.h"
#include "Attachable.h"
#include "AudioMan.h"
#include "Base64/base64.h"
#include "Box.h"
#include "Gib.h"
#include "HeldDevice.h"
#include "LuaMan.h"
#include "MOSRotating.h"
#include "MovableMan.h"
#include "PostProcessMan.h"
#include "PresetMan.h"
#include "PreviewEventLedger.h"
#include "RTEError.h"
#include "RTETools.h"
#include "Scene.h"
#include "SceneMan.h"
#include "SimChecksum.h"
#include "SoundSet.h"
#include "TerrainLayerSnapshot.h"
#include "TimerMan.h"
#include "Vector.h"
#include "Writer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>

#ifdef _WIN32
#include <excpt.h>
#endif

using namespace RTE;

namespace {
	using luabind::detail::class_registry;
	using luabind::detail::class_rep;
	using luabind::detail::method_rep;
	using luabind::detail::object_rep;
	using luabind::detail::overload_rep;
	using luabind::detail::overload_rep_base;

	constexpr const char* c_Tag = "[preview-binding-exhaustive-selftest]";
	constexpr int c_Crashed = -1000;

	// The registry and the class keep these private; an explicit instantiation may name a private member, so the walk reads them through one.
	template <typename Tag, typename Tag::type Member> struct PrivateMember {
		friend typename Tag::type Get(Tag) { return Member; }
	};
	struct RegistryClasses {
		using type = std::map<const std::type_info*, class_rep*, class_registry::cmp> class_registry::*;
		friend type Get(RegistryClasses);
	};
	template struct PrivateMember<RegistryClasses, &class_registry::m_classes>;
	struct ClassSetters {
		using type = std::map<const char*, class_rep::callback, luabind::detail::ltstr> class_rep::*;
		friend type Get(ClassSetters);
	};
	template struct PrivateMember<ClassSetters, &class_rep::m_setters>;
	struct ClassConstructor {
		using type = luabind::detail::construct_rep class_rep::*;
		friend type Get(ClassConstructor);
	};
	template struct PrivateMember<ClassConstructor, &class_rep::m_constructor>;

	struct OverloadArity : overload_rep_base {
		static int Of(const overload_rep_base& overload) { return overload.*(&OverloadArity::m_arity); }
	};

#ifdef _WIN32
	int CrashFilter(unsigned long exceptionCode, unsigned long* code) {
		// Lua errors and C++ exceptions unwind to the pcall; only a fault lands here.
		if (exceptionCode == 0xE06D7363UL || (exceptionCode & 0xFFFFFF00UL) == 0xE24C4A00UL) {
			return EXCEPTION_CONTINUE_SEARCH;
		}
		*code = exceptionCode;
		return EXCEPTION_EXECUTE_HANDLER;
	}

	int GuardedCall(lua_State* L, int argumentCount, int resultCount, unsigned long* code) {
		__try {
			return lua_pcall(L, argumentCount, resultCount, 0);
		} __except (CrashFilter(GetExceptionCode(), code)) {
			return c_Crashed;
		}
	}
#else
	int GuardedCall(lua_State* L, int argumentCount, int resultCount, unsigned long*) {
		return lua_pcall(L, argumentCount, resultCount, 0);
	}
#endif

	// The drops the fence decides during one call, counted around its own hooks.
	bool (*s_FenceRuns)(const char*, const char*, bool) = nullptr;
	bool (*s_FenceArgument)(lua_State*, int, bool, bool, const char*, const char*) = nullptr;
	int s_Drops = 0;

	bool CountingRuns(const char* className, const char* methodName, bool isConst) {
		const bool runs = s_FenceRuns && s_FenceRuns(className, methodName, isConst);
		s_Drops += runs ? 0 : 1;
		return runs;
	}

	bool CountingArgument(lua_State* L, int index, bool mutablePointer, bool mutableReference, const char* className, const char* methodName) {
		const bool takes = s_FenceArgument(L, index, mutablePointer, mutableReference, className, methodName);
		s_Drops += takes ? 0 : 1;
		return takes;
	}

	// The LocalPrediction preview's fences, in its order, around one call instead of a stepped clone.
	struct PreviewWindow {
		Activity* activity = nullptr;
		long long simCount = 0;
		long long simTicks = 0;
		std::mt19937 rng;
		uint64_t draws = 0;
		long uidCounter = 0;
		uint64_t soundCursor = 0;
		Activity::RollbackState activityState;
		TerrainLayerSnapshot terrain;
		bool terrainCaptured = false;

		void Open() {
			activity = g_ActivityMan.GetActivity();
			simCount = g_TimerMan.GetSimUpdateCount();
			simTicks = g_TimerMan.GetSimTimeTicks();
			rng = g_SimRNG.GetEngineState();
			draws = g_SimRNG.GetDrawCount();
			uidCounter = MovableObject::GetUniqueIDCounter();
			soundCursor = g_AudioMan.GetCheckpointSoundContainerCursor();
			if (activity) {
				activity->CaptureRollbackState(activityState);
			}
			terrainCaptured = terrain.Capture();
			LuaMan::SetScriptsFrozen(true);
			AudioMan::SetPlaybackSuppressed(true);
			PostProcessMan::SetRegistrationSuppressed(true);
			PreviewEventLedger::Arm(static_cast<uint64_t>(simCount), soundCursor, {});
			LuaMan::CapturePreviewSelfCopies({}, false);
			g_MovableMan.BeginSpeculation();
			MovableObject::PinUniqueIDCounter(uidCounter);
			LuaMan::BeginPreviewScripts({}, false);
			g_TimerMan.AdvanceSimTickForPreview();
			s_FenceRuns = luabind::detail::preview_fence::runs;
			s_FenceArgument = luabind::detail::preview_fence::argument;
			luabind::detail::preview_fence::runs = &CountingRuns;
			if (s_FenceArgument) {
				luabind::detail::preview_fence::argument = &CountingArgument;
			}
		}

		void Close() {
			luabind::detail::preview_fence::runs = s_FenceRuns;
			luabind::detail::preview_fence::argument = s_FenceArgument;
			g_MovableMan.HarvestSpeculativeSpawns();
			std::vector<MovableObject*> taken;
			g_MovableMan.EndSpeculation(&taken);
			if (terrainCaptured) {
				terrain.Restore();
			}
			if (activity) {
				activity->RestoreRollbackState(activityState);
			}
			g_SimRNG.SetEngineState(rng);
			g_SimRNG.SetDrawCount(draws);
			g_TimerMan.RestoreSimTickAfterPreview(simCount, simTicks);
			MovableObject::PinUniqueIDCounter(uidCounter);
			g_AudioMan.SetCheckpointSoundContainerCursor(soundCursor);
			PreviewEventLedger::Disarm();
			PostProcessMan::SetRegistrationSuppressed(false);
			AudioMan::SetPlaybackSuppressed(false);
			LuaMan::EndPreviewScripts();
			LuaMan::SetScriptsFrozen(false);
		}
	};

	struct Instance {
		class_rep* crep = nullptr;
		void* object = nullptr;
		int handle = LUA_NOREF;
		std::string origin;
		const Entity* entity = nullptr;
		const Serializable* serializable = nullptr;
		bool sceneObject = false;
		std::vector<std::string> lines;
		std::set<size_t> maskedLines;
		bool unstableBytes = false;
		std::vector<std::pair<std::string, std::string>> fingerprint;
		std::set<std::string> unstableProperties;
	};

	struct Finding {
		std::string call;
		std::string where;
		std::string detail;
	};

	struct WorldHash {
		std::string total;
		std::map<std::string, std::string> parts;
	};

	uint64_t s_FixtureTick = 0;
	uint64_t s_WalkTick = 0;
	bool s_Armed = false;
	int s_Verdict = -1;

	std::map<std::string, class_rep*> CppClasses(lua_State* L) {
		std::map<std::string, class_rep*> classes;
		const class_registry* registry = class_registry::get_registry(L);
		for (const auto& [type, crep]: registry->*Get(RegistryClasses{})) {
			if (crep && crep->get_class_type() == class_rep::cpp_class) {
				classes.emplace(crep->name(), crep);
			}
		}
		return classes;
	}

	bool Derives(const class_rep* crep, const std::type_info& base) {
		int offset = 0;
		return luabind::detail::implicit_cast(crep, &base, offset) >= 0;
	}

	class_rep* RegisteredClassOf(const Entity* entity, const std::map<std::string, class_rep*>& classes) {
		for (const Entity::ClassInfo* info = &entity->GetClass(); info; info = info->GetParent()) {
			if (const auto found = classes.find(info->GetName()); found != classes.end()) {
				return found->second;
			}
		}
		return nullptr;
	}

	void* EntityPointerAs(const Entity* entity, const class_rep* crep) {
		int offset = 0;
		if (luabind::detail::implicit_cast(crep, &typeid(Entity), offset) < 0) {
			return nullptr;
		}
		return const_cast<char*>(reinterpret_cast<const char*>(entity)) - offset;
	}

	void PushHandle(lua_State* L, void* object, class_rep* crep) {
		void* storage = lua_newuserdata(L, sizeof(object_rep));
		new (storage) object_rep(object, crep, 0, nullptr);
		luabind::detail::getref(L, crep->metatable_ref());
		lua_setmetatable(L, -2);
	}

	// Picks the checkpoint serializer for the object behind a handle.
	void Describe(Instance& instance) {
		int offset = 0;
		if (luabind::detail::implicit_cast(instance.crep, &typeid(Entity), offset) >= 0) {
			instance.entity = reinterpret_cast<const Entity*>(static_cast<char*>(instance.object) + offset);
			instance.sceneObject = dynamic_cast<const SceneObject*>(instance.entity) != nullptr;
			return;
		}
		const std::type_info& type = *instance.crep->type();
		if (type == typeid(Vector)) {
			instance.serializable = static_cast<const Vector*>(instance.object);
		} else if (type == typeid(Box)) {
			instance.serializable = static_cast<const Box*>(instance.object);
		} else if (type == typeid(SoundSet)) {
			instance.serializable = static_cast<const SoundSet*>(instance.object);
		} else if (type == typeid(Gib)) {
			instance.serializable = static_cast<const Gib*>(instance.object);
		}
	}

	bool HasSerializer(const Instance& instance) {
		return instance.entity || instance.serializable;
	}

	std::string Serialize(const Instance& instance) {
		auto stream = std::make_unique<std::ostringstream>();
		std::ostringstream* text = stream.get();
		Writer writer(std::move(stream));
		if (instance.sceneObject) {
			Scene::SaveSceneObject(writer, dynamic_cast<const SceneObject*>(instance.entity), false, true);
		} else if (instance.entity) {
			instance.entity->Save(writer);
		} else if (instance.serializable) {
			instance.serializable->Save(writer);
		}
		return text->str();
	}

	void SerializeInto(const Instance* instance, std::string* out) {
		try {
			*out = Serialize(*instance);
		} catch (const std::exception& error) {
			*out = std::string("<the serializer threw: ") + error.what() + ">";
		}
	}

#ifdef _WIN32
	bool GuardedSerialize(const Instance* instance, std::string* out, unsigned long* code) {
		__try {
			SerializeInto(instance, out);
			return true;
		} __except (CrashFilter(GetExceptionCode(), code)) {
			return false;
		}
	}
#else
	bool GuardedSerialize(const Instance* instance, std::string* out, unsigned long*) {
		SerializeInto(instance, out);
		return true;
	}
#endif

	std::vector<std::string> Lines(const std::string& text) {
		std::vector<std::string> lines;
		std::istringstream stream(text);
		for (std::string line; std::getline(stream, line);) {
			lines.push_back(line);
		}
		return lines;
	}

	std::vector<std::string> Tokens(const std::string& text) {
		std::vector<std::string> tokens;
		std::istringstream stream(text);
		for (std::string token; stream >> token;) {
			tokens.push_back(token);
		}
		return tokens;
	}

	// The first difference between two serialized lines; a base64 runtime block is decoded and compared token by token.
	std::string DescribeLineChange(const std::string& before, const std::string& after) {
		const size_t equals = before.find(" = ");
		const auto encoded = [equals](const std::string& line) {
			return line.size() >= equals + 3 + 24 && line.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/-_=.", equals + 3) == std::string::npos;
		};
		if (equals != std::string::npos && after.compare(0, equals + 3, before, 0, equals + 3) == 0 && encoded(before) && encoded(after)) {
			const std::string field = before.substr(0, equals);
			std::vector<std::string> was;
			std::vector<std::string> now;
			// A runtime block pads with '.', or not at all.
			const auto decode = [equals](const std::string& line) {
				std::string payload = line.substr(equals + 3);
				while (!payload.empty() && (payload.back() == '.' || payload.back() == '=')) {
					payload.pop_back();
				}
				payload.append((4 - payload.size() % 4) % 4, '=');
				return Tokens(base64_decode(payload));
			};
			try {
				was = decode(before);
				now = decode(after);
			} catch (const std::exception&) {
				was.clear();
			}
			if (!was.empty() && !now.empty()) {
				size_t at = 0;
				while (at < was.size() && at < now.size() && was[at] == now[at]) {
					++at;
				}
				const auto window = [at](const std::vector<std::string>& tokens) {
					std::string text;
					for (size_t i = at >= 4 ? at - 4 : 0; i < tokens.size() && i < at + 6; ++i) {
						text += (text.empty() ? "" : " ") + (i == at ? "[" + tokens[i].substr(0, 40) + "]" : tokens[i].substr(0, 40));
					}
					// A runtime block can carry pixels; only text is printed.
					for (char& c: text) {
						c = (c >= 32 && c < 127) ? c : '?';
					}
					return text;
				};
				size_t differing = 0;
				for (size_t i = 0; i < was.size() && i < now.size(); ++i) {
					differing += was[i] != now[i] ? 1 : 0;
				}
				return field + " decoded token " + std::to_string(at) + " of " + std::to_string(was.size()) + " (" + std::to_string(differing) + " tokens differ, " +
				       std::to_string(was.size()) + " -> " + std::to_string(now.size()) + " tokens): ..." + window(was) + "... -> ..." + window(now) + "...";
			}
		}
		const auto clip = [](const std::string& text) { return text.size() > 300 ? text.substr(0, 300) + "..." : text; };
		return clip(before) + " -> " + clip(after);
	}

	WorldHash HashWorld() {
		g_SimChecksum.BeginTick(static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount()));
		g_MovableMan.FeedTickEndChecksum();
		g_SceneMan.FeedTerrainToSimChecksum();
		g_LuaMan.HashAllLuaStatesIntoSimChecksum();
		const SimChecksum::Result result = g_SimChecksum.EndTick();
		WorldHash hash;
		hash.total = SimChecksum::HashHex(SimChecksum::SimGatedHash(result));
		for (const auto& [name, value]: result.per_subsystem) {
			hash.parts[name] = SimChecksum::HashHex(value);
		}
		return hash;
	}

	// The parameters a signature names, without the ones Lua does not pass.
	std::vector<std::string> SignatureParameters(const std::string& signature) {
		std::vector<std::string> parameters;
		const size_t open = signature.find('(');
		const size_t close = signature.rfind(')');
		if (open == std::string::npos || close == std::string::npos || close <= open) {
			return parameters;
		}
		int depth = 0;
		std::string current;
		const auto flush = [&]() {
			const size_t first = current.find_first_not_of(' ');
			const size_t last = current.find_last_not_of(' ');
			if (first != std::string::npos) {
				std::string parameter = current.substr(first, last - first + 1);
				if (parameter != "lua_State*") {
					parameters.push_back(parameter);
				}
			}
			current.clear();
		};
		for (size_t i = open + 1; i < close; ++i) {
			const char c = signature[i];
			if (c == '<' || c == '[' || c == '(') {
				++depth;
			} else if (c == '>' || c == ']' || c == ')') {
				--depth;
			}
			if (c == ',' && depth == 0) {
				flush();
			} else {
				current += c;
			}
		}
		flush();
		return parameters;
	}

	std::string BareType(std::string type) {
		if (type.rfind("const ", 0) == 0) {
			type.erase(0, 6);
		}
		while (!type.empty() && (type.back() == '&' || type.back() == '*' || type.back() == ' ')) {
			type.pop_back();
		}
		return type;
	}

	bool PrimitiveType(const std::string& type) {
		const std::string bare = BareType(type);
		return bare == "number" || bare == "boolean" || bare == "string" || bare.rfind("custom", 0) == 0;
	}

	class Walk {
	public:
		explicit Walk(lua_State* state) : L(state) {}

		bool Run();
		bool RunWalk();

	private:
		lua_State* L;
		std::map<std::string, class_rep*> m_Classes;
		std::map<std::string, Instance> m_Instances;
		PreviewWindow m_Window;
		WorldHash m_World;
		std::ofstream m_Journal;
		int m_GetRef = LUA_NOREF;
		int m_SetRef = LUA_NOREF;
		std::vector<Finding> m_Leaks;
		std::vector<Finding> m_Crashes;
		std::vector<Finding> m_Asserts;
		size_t m_Methods = 0;
		size_t m_Overloads = 0;
		size_t m_Properties = 0;
		size_t m_Calls = 0;
		size_t m_Dropped = 0;
		size_t m_Errors = 0;
		size_t m_DiscoveryCrashes = 0;

		void MakeHelpers();
		// A full cycle, as the engine runs between ticks, and the collector stopped again as the engine leaves it.
		void CollectGarbage() {
			lua_gc(L, LUA_GCCOLLECT, 0);
			lua_gc(L, LUA_GCSTOP, 0);
		}
		bool AdoptTop(const std::string& origin);
		void AddPointer(class_rep* crep, void* object, const std::string& origin);
		void AddEntity(const Entity* entity, const std::string& origin, bool exact);
		void CollectWorld();
		void Discover();
		void AddPresetClones();
		void FillBases();
		void Construct();
		void PushArgument(const std::string& type, double number, std::string& text);
		int PushArguments(const overload_rep_base& overload, double number, std::string& text);
		std::string ValueText(int index, bool nested);
		std::vector<std::pair<std::string, std::string>> Fingerprint(const Instance& instance);
		void Baseline(Instance& instance);
		bool ObjectChanged(Instance& instance, std::string& where, std::string& first);
		void Probe(Instance& target, const std::string& call, const std::function<int()>& push);
		void Check(Instance& target, const std::string& call);
		void Sweep(const std::string& after);
		bool Control();
		void WalkClass(Instance& instance);
	};

	void Walk::MakeHelpers() {
		// Property reads and writes go through Lua, so an error in either comes back to the pcall.
		if (luaL_loadstring(L, "return function(o, k) return o[k] end, function(o, k, v) o[k] = v end") == 0 && lua_pcall(L, 0, 2, 0) == 0) {
			m_SetRef = luaL_ref(L, LUA_REGISTRYINDEX);
			m_GetRef = luaL_ref(L, LUA_REGISTRYINDEX);
		} else {
			lua_pop(L, 1);
		}
	}

	// Keeps the handle on top of the stack as its class's instance when that class has none; pops it either way. An iterator
	// stands for the first object it hands out.
	bool Walk::AdoptTop(const std::string& origin) {
		if (lua_type(L, -1) == LUA_TFUNCTION) {
			unsigned long code = 0;
			const int top = lua_gettop(L);
			if (GuardedCall(L, 0, 1, &code) != 0) {
				lua_settop(L, top - 1);
				return false;
			}
			return AdoptTop(origin + " first element");
		}
		object_rep* rep = luabind::detail::is_class_object(L, -1);
		if (!rep || !rep->ptr() || !rep->crep() || rep->crep()->get_class_type() != class_rep::cpp_class || m_Instances.count(rep->crep()->name()) > 0) {
			lua_pop(L, 1);
			return false;
		}
		Instance instance;
		instance.crep = rep->crep();
		instance.object = rep->ptr();
		instance.origin = origin;
		instance.handle = luaL_ref(L, LUA_REGISTRYINDEX);
		Describe(instance);
		m_Instances.emplace(instance.crep->name(), std::move(instance));
		return true;
	}

	void Walk::AddPointer(class_rep* crep, void* object, const std::string& origin) {
		if (!crep || !object || m_Instances.count(crep->name()) > 0) {
			return;
		}
		PushHandle(L, object, crep);
		AdoptTop(origin);
	}

	// An exact instance is of the class itself; otherwise of the nearest class the bindings register.
	void Walk::AddEntity(const Entity* entity, const std::string& origin, bool exact) {
		class_rep* crep = entity ? RegisteredClassOf(entity, m_Classes) : nullptr;
		if (crep && (!exact || entity->GetClassName() == crep->name())) {
			AddPointer(crep, EntityPointerAs(entity, crep), origin);
		}
	}

	void Walk::CollectWorld() {
		// The engine's globals: the managers and whatever the scripts keep there.
		lua_pushnil(L);
		while (lua_next(L, LUA_GLOBALSINDEX) != 0) {
			if (lua_type(L, -2) == LUA_TSTRING && luabind::detail::is_class_object(L, -1)) {
				const std::string name = lua_tostring(L, -2);
				lua_pushvalue(L, -1);
				AdoptTop("global " + name);
			}
			lua_pop(L, 1);
		}
		std::vector<MovableObject*> known = g_MovableMan.SnapshotKnownObjects();
		std::sort(known.begin(), known.end(), [](const MovableObject* a, const MovableObject* b) { return a->GetUniqueID() < b->GetUniqueID(); });
		bool exact = true;
		std::function<void(const MovableObject*)> visit = [&](const MovableObject* mo) {
			AddEntity(mo, "world uid=" + std::to_string(mo->GetUniqueID()) + " " + mo->GetPresetName(), exact);
			if (const MOSRotating* rotating = dynamic_cast<const MOSRotating*>(mo)) {
				for (const Attachable* attachable: rotating->GetAttachableList()) {
					visit(attachable);
				}
				for (const AEmitter* wound: rotating->GetWoundList()) {
					visit(static_cast<const MovableObject*>(wound));
				}
			}
		};
		// The objects in the world and what they carry; exact classes first, then the nearest registered one. A known object
		// outside the world (a placeholder with no material) is not what a script walks.
		for (int pass = 0; pass < 2; ++pass) {
			exact = pass == 0;
			for (const MovableObject* mo: known) {
				if (g_MovableMan.IsResident(mo)) {
					visit(mo);
				}
			}
		}
		AddEntity(g_SceneMan.GetScene(), "the scene", false);
		AddEntity(g_ActivityMan.GetActivity(), "the activity", false);
	}

	std::string Walk::ValueText(int index, bool nested) {
		index = index < 0 ? lua_gettop(L) + index + 1 : index;
		switch (lua_type(L, index)) {
			case LUA_TNIL:
				return "nil";
			case LUA_TBOOLEAN:
				return lua_toboolean(L, index) ? "true" : "false";
			case LUA_TNUMBER: {
				char buffer[64];
				std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(lua_tonumber(L, index)));
				return buffer;
			}
			case LUA_TSTRING: {
				size_t length = 0;
				const char* text = lua_tolstring(L, index, &length);
				return "\"" + std::string(text, std::min<size_t>(length, 200)) + "\"";
			}
			case LUA_TUSERDATA: {
				object_rep* rep = luabind::detail::is_class_object(L, index);
				if (!rep) {
					return "userdata";
				}
				std::string text = std::string(rep->crep()->name()) + "{";
				if (!nested) {
					for (const auto& [name, callback]: rep->crep()->properties()) {
						lua_rawgeti(L, LUA_REGISTRYINDEX, m_GetRef);
						lua_pushvalue(L, index);
						lua_pushstring(L, name);
						unsigned long code = 0;
						if (GuardedCall(L, 2, 1, &code) == 0) {
							const int type = lua_type(L, -1);
							if (type == LUA_TNUMBER || type == LUA_TBOOLEAN || type == LUA_TSTRING) {
								text += std::string(name) + "=" + ValueText(-1, true) + ";";
							}
						}
						lua_settop(L, index);
					}
				}
				return text + "}";
			}
			default:
				return lua_typename(L, lua_type(L, index));
		}
	}

	std::vector<std::pair<std::string, std::string>> Walk::Fingerprint(const Instance& instance) {
		std::vector<std::pair<std::string, std::string>> readings;
		const int top = lua_gettop(L);
		for (const auto& [name, callback]: instance.crep->properties()) {
			lua_rawgeti(L, LUA_REGISTRYINDEX, m_GetRef);
			lua_rawgeti(L, LUA_REGISTRYINDEX, instance.handle);
			lua_pushstring(L, name);
			unsigned long code = 0;
			const int status = GuardedCall(L, 2, 1, &code);
			readings.emplace_back(name, status == 0 ? ValueText(-1, false) : std::string("error"));
			lua_settop(L, top);
		}
		return readings;
	}

	// Two readings with nothing between them: what differs is the reading's own noise, listed and left out of the compare.
	void Walk::Baseline(Instance& instance) {
		instance.maskedLines.clear();
		instance.unstableProperties.clear();
		instance.unstableBytes = false;
		std::string first;
		std::string second;
		unsigned long code = 0;
		if (HasSerializer(instance) && !(GuardedSerialize(&instance, &first, &code) && GuardedSerialize(&instance, &second, &code))) {
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "0x%08lX", code);
			m_Crashes.push_back({std::string(instance.crep->name()) + " serializer", "fault", buffer});
			std::cout << "[bindx] CRASH the " << instance.crep->name() << " serializer faulted (" << buffer << "); its properties stand in for its bytes" << std::endl;
			instance.entity = nullptr;
			instance.serializable = nullptr;
			instance.sceneObject = false;
		}
		if (HasSerializer(instance)) {
			instance.lines = Lines(first);
			const std::vector<std::string> again = Lines(second);
			if (again.size() != instance.lines.size()) {
				instance.unstableBytes = true;
			} else {
				for (size_t i = 0; i < again.size(); ++i) {
					if (again[i] != instance.lines[i]) {
						instance.maskedLines.insert(i);
					}
				}
			}
		} else {
			instance.fingerprint = Fingerprint(instance);
			const auto again = Fingerprint(instance);
			for (size_t i = 0; i < again.size() && i < instance.fingerprint.size(); ++i) {
				if (again[i].second != instance.fingerprint[i].second) {
					instance.unstableProperties.insert(again[i].first);
				}
			}
		}
	}

	bool Walk::ObjectChanged(Instance& instance, std::string& where, std::string& first) {
		if (HasSerializer(instance)) {
			if (instance.unstableBytes) {
				return false;
			}
			std::string text;
			unsigned long code = 0;
			where = "object bytes";
			if (!GuardedSerialize(&instance, &text, &code)) {
				first = "the serializer faulted after the call";
				return true;
			}
			const std::vector<std::string> now = Lines(text);
			if (now.size() != instance.lines.size()) {
				size_t at = 0;
				while (at < now.size() && at < instance.lines.size() && now[at] == instance.lines[at]) {
					++at;
				}
				first = "line " + std::to_string(at + 1) + " (" + std::to_string(instance.lines.size()) + " -> " + std::to_string(now.size()) + " lines): " +
				        (at < instance.lines.size() ? instance.lines[at] : std::string("<end>")) + " -> " + (at < now.size() ? now[at] : std::string("<end>"));
				return true;
			}
			for (size_t i = 0; i < now.size(); ++i) {
				if (now[i] != instance.lines[i] && instance.maskedLines.count(i) == 0) {
					first = "line " + std::to_string(i + 1) + ": " + DescribeLineChange(instance.lines[i], now[i]);
					// Both whole lines, for the report to decode.
					static std::ofstream changedLines("preview_binding_exhaustive.lines", std::ios::trunc);
					changedLines << instance.crep->name() << " line " << (i + 1) << "\n" << instance.lines[i] << "\n" << now[i] << "\n" << std::flush;
					return true;
				}
			}
			return false;
		}
		const auto now = Fingerprint(instance);
		where = "object properties";
		for (size_t i = 0; i < now.size() && i < instance.fingerprint.size(); ++i) {
			if (now[i].second != instance.fingerprint[i].second && instance.unstableProperties.count(now[i].first) == 0) {
				first = now[i].first + ": " + instance.fingerprint[i].second + " -> " + now[i].second;
				return true;
			}
		}
		return false;
	}

	void Walk::PushArgument(const std::string& type, double number, std::string& text) {
		const std::string bare = BareType(type);
		if (bare == "number" || bare.rfind("custom", 0) == 0) {
			lua_pushnumber(L, number);
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%g", number);
			text += buffer;
		} else if (bare == "boolean") {
			lua_pushboolean(L, 1);
			text += "true";
		} else if (bare == "string") {
			lua_pushstring(L, "a");
			text += "\"a\"";
		} else if (const auto found = m_Instances.find(bare); found != m_Instances.end()) {
			lua_rawgeti(L, LUA_REGISTRYINDEX, found->second.handle);
			text += bare;
		} else {
			lua_pushnil(L);
			text += "nil";
		}
	}

	int Walk::PushArguments(const overload_rep_base& overload, double number, std::string& text) {
		std::string signature;
		overload.get_signature(L, signature);
		std::vector<std::string> parameters = SignatureParameters(signature);
		const int count = std::max(0, OverloadArity::Of(overload) - 1);
		// A method bound from a free function names its self first; Lua passes self apart.
		if (static_cast<int>(parameters.size()) > count) {
			parameters.erase(parameters.begin(), parameters.begin() + (parameters.size() - count));
		}
		text = "(";
		for (int i = 0; i < count; ++i) {
			if (i > 0) {
				text += ", ";
			}
			PushArgument(i < static_cast<int>(parameters.size()) ? parameters[i] : std::string("nil"), number, text);
		}
		text += ")";
		return count;
	}

	void Walk::Discover() {
		for (int round = 0; round < 4; ++round) {
			const size_t before = m_Instances.size();
			std::vector<std::string> names;
			for (const auto& [name, instance]: m_Instances) {
				names.push_back(name);
			}
			for (const std::string& name: names) {
				const Instance source = m_Instances.at(name);
				for (const auto& [property, callback]: source.crep->properties()) {
					const int top = lua_gettop(L);
					lua_rawgeti(L, LUA_REGISTRYINDEX, m_GetRef);
					lua_rawgeti(L, LUA_REGISTRYINDEX, source.handle);
					lua_pushstring(L, property);
					unsigned long code = 0;
					const int status = GuardedCall(L, 2, 1, &code);
					if (status == 0) {
						AdoptTop("discovered " + name + "." + property);
					} else if (status == c_Crashed) {
						++m_DiscoveryCrashes;
						std::cout << "[bindx] CRASH during discovery " << name << "." << property << " (get) fault 0x" << std::hex << code << std::dec << std::endl;
					}
					lua_settop(L, top);
				}
				source.crep->get_table(L);
				const int table = lua_gettop(L);
				std::vector<std::pair<std::string, const method_rep*>> getters;
				lua_pushnil(L);
				while (lua_next(L, table) != 0) {
					if (lua_type(L, -2) == LUA_TSTRING && lua_tocfunction(L, -1) == &class_rep::function_dispatcher && lua_getupvalue(L, -1, 1)) {
						const std::string method = lua_tostring(L, -3);
						if (method.rfind("Get", 0) == 0) {
							getters.emplace_back(method, static_cast<const method_rep*>(lua_touserdata(L, -1)));
						}
						lua_pop(L, 1);
					}
					lua_pop(L, 1);
				}
				std::sort(getters.begin(), getters.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
				for (const auto& [method, rep]: getters) {
					for (const overload_rep& overload: rep->overloads()) {
						std::string signature;
						overload.get_signature(L, signature);
						const std::vector<std::string> parameters = SignatureParameters(signature);
						if (!std::all_of(parameters.begin(), parameters.end(), [&](const std::string& parameter) { return PrimitiveType(parameter) || m_Instances.count(BareType(parameter)) > 0; })) {
							continue;
						}
						const int top = lua_gettop(L);
						lua_pushstring(L, method.c_str());
						lua_rawget(L, table);
						lua_rawgeti(L, LUA_REGISTRYINDEX, source.handle);
						std::string args;
						const int count = PushArguments(overload, 0, args);
						unsigned long code = 0;
						const int status = GuardedCall(L, count + 1, 1, &code);
						if (status == 0) {
							AdoptTop("discovered " + name + ":" + method + args);
						} else if (status == c_Crashed) {
							++m_DiscoveryCrashes;
							std::cout << "[bindx] CRASH during discovery " << name << ":" << method << args << " fault 0x" << std::hex << code << std::dec << std::endl;
						}
						lua_settop(L, top);
					}
				}
				lua_settop(L, table - 1);
			}
			if (m_Instances.size() == before) {
				break;
			}
		}
	}

	void Walk::AddPresetClones() {
		for (const auto& [name, crep]: m_Classes) {
			if (m_Instances.count(name) > 0 || !Derives(crep, typeid(Entity))) {
				continue;
			}
			std::list<Entity*> presets;
			g_PresetMan.GetAllOfType(presets, name);
			for (const Entity* preset: presets) {
				if (preset && preset->GetClassName() == name) {
					// A script holds a copy it made before the preview; the preset itself stays the library's.
					const Entity* copy = preset->Clone();
					AddEntity(copy, "clone of preset " + preset->GetModuleAndPresetName(), true);
					break;
				}
			}
		}
	}

	// A class the engine only makes through a subclass is walked on an object of that subclass.
	void Walk::FillBases() {
		std::vector<std::string> names;
		for (const auto& [name, instance]: m_Instances) {
			names.push_back(name);
		}
		for (const std::string& name: names) {
			const std::string origin = "the " + name + " instance (" + m_Instances.at(name).origin + ")";
			std::function<void(class_rep*, char*)> climb = [&](class_rep* crep, char* object) {
				for (const class_rep::base_info& base: crep->bases()) {
					char* basePointer = object + base.pointer_offset;
					if (base.base && m_Instances.count(base.base->name()) == 0) {
						AddPointer(base.base, basePointer, origin);
					}
					if (base.base) {
						climb(base.base, basePointer);
					}
				}
			};
			climb(m_Instances.at(name).crep, static_cast<char*>(m_Instances.at(name).object));
		}
	}

	void Walk::Construct() {
		for (const auto& [name, crep]: m_Classes) {
			if (m_Instances.count(name) > 0 || !crep->has_lua_constructor()) {
				continue;
			}
			for (const auto& overload: (crep->*Get(ClassConstructor{})).overloads) {
				const int top = lua_gettop(L);
				lua_getglobal(L, name.c_str());
				if (lua_isnil(L, -1)) {
					lua_settop(L, top);
					break;
				}
				std::string args;
				std::string signature;
				overload.get_signature(L, signature);
				std::vector<std::string> parameters = SignatureParameters(signature);
				args = "(";
				for (size_t i = 0; i < parameters.size(); ++i) {
					args += i > 0 ? ", " : "";
					PushArgument(parameters[i], 0, args);
				}
				args += ")";
				unsigned long code = 0;
				const int status = GuardedCall(L, static_cast<int>(parameters.size()), 1, &code);
				const bool adopted = status == 0 && AdoptTop("constructed " + name + args);
				lua_settop(L, top);
				if (adopted) {
					break;
				}
			}
		}
	}

	void Walk::Probe(Instance& target, const std::string& call, const std::function<int()>& push) {
		m_Journal << call << '\n' << std::flush;
		const int base = lua_gettop(L);
		const int count = push();
		RTEError::s_LastIgnoredAssertDescription.clear();
		unsigned long code = 0;
		m_Window.Open();
		s_Drops = 0;
		const int status = GuardedCall(L, count, LUA_MULTRET, &code);
		const int drops = s_Drops;
		m_Window.Close();
		lua_settop(L, base);
		++m_Calls;
		m_Dropped += drops > 0 ? 1 : 0;
		m_Errors += status != 0 && status != c_Crashed ? 1 : 0;
		if (status == c_Crashed) {
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "0x%08lX", code);
			m_Crashes.push_back({call, "fault", buffer});
			std::cout << "[bindx] CRASH " << call << " fault " << buffer << std::endl;
		}
		if (!RTEError::s_LastIgnoredAssertDescription.empty()) {
			m_Asserts.push_back({call, "assert", RTEError::s_LastIgnoredAssertDescription});
			std::cout << "[bindx] ASSERT " << call << ": " << RTEError::s_LastIgnoredAssertDescription << std::endl;
		}
		CollectGarbage();
		Check(target, call);
	}

	void Walk::Check(Instance& target, const std::string& call) {
		std::string where;
		std::string first;
		if (ObjectChanged(target, where, first)) {
			m_Leaks.push_back({call, where, first});
			std::cout << "[bindx] LEAK " << call << " " << where << " first=" << first << std::endl;
			Baseline(target);
		}
		const WorldHash now = HashWorld();
		if (now.total != m_World.total || now.parts != m_World.parts) {
			std::string parts;
			for (const auto& [name, value]: now.parts) {
				const auto was = m_World.parts.find(name);
				if (was == m_World.parts.end() || was->second != value) {
					parts += (parts.empty() ? "" : ",") + name;
				}
			}
			for (const auto& [name, value]: m_World.parts) {
				if (now.parts.count(name) == 0) {
					parts += (parts.empty() ? "" : ",") + name + "(gone)";
				}
			}
			m_Leaks.push_back({call, "world hash", parts});
			std::cout << "[bindx] LEAK " << call << " world hash subsystems=" << parts << " gated=" << (now.total != m_World.total ? "changed" : "same") << std::endl;
			m_World = now;
		}
	}

	// Every fixture after a class: a call that wrote an object other than its own shows here.
	void Walk::Sweep(const std::string& after) {
		for (auto& [name, instance]: m_Instances) {
			std::string where;
			std::string first;
			if (ObjectChanged(instance, where, first)) {
				m_Leaks.push_back({"after " + after + " on " + name, where, first});
				std::cout << "[bindx] LEAK after the " << after << " calls, on the " << name << " fixture: " << where << " first=" << first << std::endl;
				Baseline(instance);
			}
		}
	}

	// The oracle's own detecting run: a write to a fixture's health must show in its bytes and in the world hash, and its undo in neither.
	bool Walk::Control() {
		const auto found = m_Instances.find("AHuman");
		Actor* actor = found == m_Instances.end() ? nullptr : const_cast<Actor*>(dynamic_cast<const Actor*>(found->second.entity));
		if (!actor) {
			std::cout << "[bindx] control: no AHuman fixture to write" << std::endl;
			return false;
		}
		const float health = actor->GetHealth();
		actor->SetHealth(health - 1.0F);
		std::string where;
		std::string first;
		const bool seenBytes = ObjectChanged(found->second, where, first);
		const bool seenWorld = HashWorld().total != m_World.total;
		actor->SetHealth(health);
		std::string firstAfter;
		const bool bytesUndone = !ObjectChanged(found->second, where, firstAfter);
		const bool worldUndone = HashWorld().total == m_World.total;
		std::cout << "[bindx] control: AHuman health written one lower: object bytes " << (seenBytes ? "changed (" + first + ")" : std::string("unchanged")) << ", world hash "
		          << (seenWorld ? "changed" : "unchanged") << "; written back: object bytes " << (bytesUndone ? "as before" : "still changed (" + firstAfter + ")") << ", world hash "
		          << (worldUndone ? "as before" : "still changed") << std::endl;
		return seenBytes && seenWorld && bytesUndone && worldUndone;
	}

	void Walk::WalkClass(Instance& instance) {
		const auto started = std::chrono::steady_clock::now();
		const std::string className = instance.crep->name();
		const size_t callsBefore = m_Calls;
		const size_t leaksBefore = m_Leaks.size();
		instance.crep->get_table(L);
		const int table = lua_gettop(L);
		std::vector<std::pair<std::string, const method_rep*>> methods;
		lua_pushnil(L);
		while (lua_next(L, table) != 0) {
			if (lua_type(L, -2) == LUA_TSTRING && lua_tocfunction(L, -1) == &class_rep::function_dispatcher && lua_getupvalue(L, -1, 1)) {
				methods.emplace_back(lua_tostring(L, -3), static_cast<const method_rep*>(lua_touserdata(L, -1)));
				lua_pop(L, 1);
			}
			lua_pop(L, 1);
		}
		std::sort(methods.begin(), methods.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		size_t overloads = 0;
		for (const auto& [method, rep]: methods) {
			++m_Methods;
			for (const overload_rep& overload: rep->overloads()) {
				++overloads;
				std::string args;
				// The arguments are made before the window opens, so none of them is the window's own.
				const std::function<int()> push = [&, method = method]() {
					lua_pushstring(L, method.c_str());
					lua_rawget(L, table);
					lua_rawgeti(L, LUA_REGISTRYINDEX, instance.handle);
					return PushArguments(overload, 1, args) + 1;
				};
				std::string preview;
				{
					const int top = lua_gettop(L);
					PushArguments(overload, 1, preview);
					lua_settop(L, top);
				}
				Probe(instance, className + ":" + method + preview, push);
			}
		}
		m_Overloads += overloads;
		lua_settop(L, table - 1);

		const auto& getters = instance.crep->properties();
		const auto& setters = instance.crep->*Get(ClassSetters{});
		std::set<std::string> properties;
		for (const auto& [name, callback]: getters) {
			properties.insert(name);
		}
		for (const auto& [name, callback]: setters) {
			properties.insert(name);
		}
		for (const std::string& property: properties) {
			++m_Properties;
			if (getters.count(property.c_str()) > 0) {
				Probe(instance, className + "." + property + " (get)", [&]() {
					lua_rawgeti(L, LUA_REGISTRYINDEX, m_GetRef);
					lua_rawgeti(L, LUA_REGISTRYINDEX, instance.handle);
					lua_pushstring(L, property.c_str());
					return 2;
				});
			}
			const auto setter = setters.find(property.c_str());
			if (setter == setters.end()) {
				continue;
			}
			std::string setterType;
			setter->second.sig(L, setterType);
			const std::vector<std::string> parameters = SignatureParameters(setterType);
			const std::string type = parameters.empty() ? std::string("nil") : parameters.front();
			// The value written differs from the one read, so a write that lands shows.
			std::string written;
			const auto pushValue = [&]() {
				const int top = lua_gettop(L);
				double number = 1;
				int current = LUA_TNIL;
				if (getters.count(property.c_str()) > 0) {
					lua_rawgeti(L, LUA_REGISTRYINDEX, m_GetRef);
					lua_rawgeti(L, LUA_REGISTRYINDEX, instance.handle);
					lua_pushstring(L, property.c_str());
					unsigned long code = 0;
					if (GuardedCall(L, 2, 1, &code) == 0) {
						current = lua_type(L, -1);
						if (current == LUA_TNUMBER) {
							number = static_cast<double>(lua_tonumber(L, -1)) + 1;
						} else if (current == LUA_TBOOLEAN) {
							const bool flipped = !lua_toboolean(L, -1);
							lua_settop(L, top);
							lua_pushboolean(L, flipped);
							written = flipped ? "true" : "false";
							return;
						} else if (current == LUA_TSTRING) {
							const std::string text = std::string(lua_tostring(L, -1)) + "x";
							lua_settop(L, top);
							lua_pushstring(L, text.c_str());
							written = "\"" + text + "\"";
							return;
						}
					}
					lua_settop(L, top);
				}
				written.clear();
				PushArgument(type, number, written);
			};
			Probe(instance, className + "." + property + " (set)", [&]() {
				lua_rawgeti(L, LUA_REGISTRYINDEX, m_SetRef);
				lua_rawgeti(L, LUA_REGISTRYINDEX, instance.handle);
				lua_pushstring(L, property.c_str());
				pushValue();
				return 3;
			});
		}
		Sweep(className);
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		std::cout << "[bindx] class " << className << " methods=" << methods.size() << " overloads=" << overloads << " properties=" << properties.size()
		          << " calls=" << (m_Calls - callsBefore) << " leaks=" << (m_Leaks.size() - leaksBefore) << " seconds=" << seconds << " instance: " << instance.origin << std::endl;
	}

	bool Walk::Run() {
		m_Journal.open("preview_binding_exhaustive.journal", std::ios::trunc);
		g_MovableMan.WaitForActorsSeeTask();
		g_MovableMan.CompleteQueuedMOIDDrawings();
		if (g_SimChecksum.IsActive()) {
			std::cout << c_Tag << " FAIL the tick hash is being taken this tick; walk on another tick" << std::endl;
			return false;
		}
		// Every binding runs under dummy arguments; an assert they raise is listed by the walk, not counted against the run.
		const bool ignoredAsserts = RTEError::s_IgnoreAllAsserts;
		const bool assertFired = RTEError::s_AssertFired;
		const std::string lastAssert = RTEError::s_LastIgnoredAssertDescription;
		RTEError::s_IgnoreAllAsserts = true;
		RTEError::s_LastIgnoredAssertDescription.clear();
		const bool passed = RunWalk();
		RTEError::s_IgnoreAllAsserts = ignoredAsserts;
		RTEError::s_AssertFired = assertFired;
		RTEError::s_LastIgnoredAssertDescription = lastAssert;
		return passed;
	}

	bool Walk::RunWalk() {
		const auto started = std::chrono::steady_clock::now();
		m_Classes = CppClasses(L);
		MakeHelpers();
		if (m_GetRef == LUA_NOREF) {
			std::cout << c_Tag << " FAIL could not make the property helpers" << std::endl;
			return false;
		}
		CollectWorld();
		Discover();
		AddPresetClones();
		FillBases();
		Construct();
		Discover();
		FillBases();

		size_t without = 0;
		for (const auto& [name, crep]: m_Classes) {
			if (const auto found = m_Instances.find(name); found != m_Instances.end()) {
				std::cout << "[bindx] instance " << name << ": " << found->second.origin << " serializer=" << (found->second.sceneObject ? "SaveSceneObject" : found->second.entity ? "Entity::Save" : found->second.serializable ? "Serializable::Save" : "properties") << std::endl;
			} else {
				++without;
				std::cout << "[bindx] NOINSTANCE " << name << std::endl;
			}
		}

		CollectGarbage();
		m_World = HashWorld();
		const WorldHash again = HashWorld();
		if (again.total != m_World.total || again.parts != m_World.parts) {
			std::cout << c_Tag << " FAIL the world hash differs between two readings with nothing between them" << std::endl;
			return false;
		}
		for (auto& [name, instance]: m_Instances) {
			Baseline(instance);
			if (instance.unstableBytes || !instance.maskedLines.empty() || !instance.unstableProperties.empty()) {
				std::string list;
				for (size_t line: instance.maskedLines) {
					list += (list.empty() ? "" : "; ") + std::to_string(line + 1) + ":" + instance.lines[line];
				}
				for (const std::string& property: instance.unstableProperties) {
					list += (list.empty() ? "" : "; ") + property;
				}
				std::cout << "[bindx] UNSTABLE " << name << (instance.unstableBytes ? " bytes (line count moves; bytes not compared)" : "") << " left out: " << list << std::endl;
			}
		}

		if (!Control()) {
			std::cout << c_Tag << " FAIL the oracle did not see a native write to a fixture, or did not see it undone" << std::endl;
			return false;
		}
		const std::string setupAssert = RTEError::s_LastIgnoredAssertDescription;
		if (!setupAssert.empty()) {
			std::cout << "[bindx] ASSERT during the fixture discovery (the last one): " << setupAssert << std::endl;
		}
		for (auto& [name, instance]: m_Instances) {
			WalkClass(instance);
		}

		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		std::cout << c_Tag << " totals classes=" << m_Classes.size() << " with_instance=" << m_Instances.size() << " without_instance=" << without << " methods=" << m_Methods
		          << " overloads=" << m_Overloads << " properties=" << m_Properties << " calls=" << m_Calls << " dropped=" << m_Dropped << " errors=" << m_Errors
		          << " leaks=" << m_Leaks.size() << " crashes=" << m_Crashes.size() << " asserts=" << m_Asserts.size() << " discovery_crashes=" << m_DiscoveryCrashes
		          << " seconds=" << seconds << std::endl;
		if (m_Leaks.empty()) {
			std::cout << c_Tag << " PASS" << std::endl;
			return true;
		}
		std::cout << c_Tag << " FAIL leaks=" << m_Leaks.size() << std::endl;
		return false;
	}

	// The classes a scene can hold, cloned from their presets into the world a few ticks before the walk.
	void AddFixtures(lua_State* L) {
		const std::map<std::string, class_rep*> classes = CppClasses(L);
		std::set<std::string> present;
		std::function<void(const MovableObject*)> note = [&](const MovableObject* mo) {
			present.insert(mo->GetClassName());
			if (const MOSRotating* rotating = dynamic_cast<const MOSRotating*>(mo)) {
				for (const Attachable* attachable: rotating->GetAttachableList()) {
					note(attachable);
				}
				for (const AEmitter* wound: rotating->GetWoundList()) {
					note(static_cast<const MovableObject*>(wound));
				}
			}
		};
		for (const MovableObject* mo: g_MovableMan.SnapshotKnownObjects()) {
			if (g_MovableMan.IsResident(mo)) {
				note(mo);
			}
		}
		std::vector<std::pair<int, std::string>> order;
		for (const auto& [name, crep]: classes) {
			if (Derives(crep, typeid(MovableObject))) {
				order.emplace_back(Derives(crep, typeid(Actor)) ? 0 : Derives(crep, typeid(HeldDevice)) ? 1 : 2, name);
			}
		}
		std::sort(order.begin(), order.end());
		const int width = std::max(200, g_SceneMan.GetSceneWidth());
		int placed = 0;
		for (const auto& [rank, name]: order) {
			if (present.count(name) > 0) {
				continue;
			}
			std::list<Entity*> presets;
			g_PresetMan.GetAllOfType(presets, name);
			const Entity* preset = nullptr;
			for (const Entity* candidate: presets) {
				if (candidate && candidate->GetClassName() == name) {
					preset = candidate;
					break;
				}
			}
			MovableObject* mo = preset ? dynamic_cast<MovableObject*>(preset->Clone()) : nullptr;
			if (!mo) {
				std::cout << "[bindx] fixture " << name << ": no preset" << std::endl;
				continue;
			}
			mo->SetPos(Vector(static_cast<float>(100 + (placed * 173) % (width - 200)), 60.0F));
			++placed;
			note(mo);
			g_MovableMan.AddMO(mo);
			std::cout << "[bindx] fixture " << name << ": " << preset->GetModuleAndPresetName() << " uid=" << mo->GetUniqueID() << std::endl;
		}
	}
} // namespace

void LuaBindingExhaustiveSelfTest::Arm(uint64_t fixtureTick, uint64_t walkTick) {
	s_Armed = true;
	s_FixtureTick = fixtureTick;
	s_WalkTick = walkTick;
	s_Verdict = -1;
}

int LuaBindingExhaustiveSelfTest::OnTick(uint64_t simTick) {
	if (!s_Armed) {
		return -1;
	}
	LuaStateWrapper& master = g_LuaMan.GetMasterScriptState();
	if (simTick == s_FixtureTick) {
		std::lock_guard<std::recursive_mutex> lock(master.GetMutex());
		AddFixtures(master.GetLuaState());
		return -1;
	}
	if (simTick != s_WalkTick) {
		return -1;
	}
	s_Armed = false;
	if (!g_ActivityMan.GetActivity() || !g_SceneMan.GetScene()) {
		std::cout << c_Tag << " FAIL no activity and scene to walk in" << std::endl;
		s_Verdict = 1;
		return s_Verdict;
	}
	std::lock_guard<std::recursive_mutex> lock(master.GetMutex());
	lua_State* L = master.GetLuaState();
	const int top = lua_gettop(L);
	Walk walk(L);
	s_Verdict = walk.Run() ? 0 : 1;
	lua_settop(L, top);
	return s_Verdict;
}
