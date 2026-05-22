// Make sure that binding definition files are always set to NOT use pre-compiled headers and conformance mode (/permissive) otherwise everything will be on fire!

#include "LuaBindingRegisterDefinitions.h"

#include "AIDebugOverlay.h"
#include "AIDecisionChannel.h"
#include "MetricsCollector.h"
#include "NetworkSimulator.h"
#include "SimChecksum.h"

using namespace RTE;

namespace {
	// Lua-side helper: AIDecisionChannel:Emit(actor_id, layer_name, type, chosen, reason)
	void AIDecisionChannelEmit(AIDecisionChannel* self,
	                           int actor_id,
	                           const std::string& layer,
	                           const std::string& type,
	                           const std::string& chosen,
	                           const std::string& reason) {
		self->EmitS(actor_id,
		            AIDecisionChannel::LayerFromName(layer),
		            type,
		            chosen,
		            reason,
		            /*target*/ -1,
		            0.0f,
		            0.0f);
	}

	// Lua-side helper: AIDecisionChannel:EmitWithTarget(actor_id, layer_name, type, chosen, reason, target_id, x, y)
	void AIDecisionChannelEmitWithTarget(AIDecisionChannel* self,
	                                     int actor_id,
	                                     const std::string& layer,
	                                     const std::string& type,
	                                     const std::string& chosen,
	                                     const std::string& reason,
	                                     int target_actor_id,
	                                     double target_x,
	                                     double target_y) {
		self->EmitS(actor_id,
		            AIDecisionChannel::LayerFromName(layer),
		            type,
		            chosen,
		            reason,
		            target_actor_id,
		            static_cast<float>(target_x),
		            static_cast<float>(target_y));
	}

	double SimChecksumGetTick(SimChecksum* self) {
		return static_cast<double>(self->GetLastResult().tick);
	}

	std::string SimChecksumGetLastTotalHex(SimChecksum* self) {
		return SimChecksum::HashHex(self->GetLastResult().total);
	}

	std::string SimChecksumGetSubsystemHex(SimChecksum* self, const std::string& name) {
		const auto last = self->GetLastResult();
		const auto it = last.per_subsystem.find(name);
		if (it == last.per_subsystem.end()) {
			return std::string();
		}
		return SimChecksum::HashHex(it->second);
	}

	void MetricsCollectorBeginRun(MetricsCollector* self, const std::string& scenario, double seed) {
		self->BeginRun(scenario, static_cast<uint64_t>(seed));
	}

	bool MetricsCollectorWriteReport(MetricsCollector* self, const std::string& path) {
		return self->WriteReport(path);
	}
} // namespace

LuaBindingRegisterFunctionDefinitionForType(ManagerLuaBindings, AIDecisionChannel) {
	return luabind::class_<AIDecisionChannel>("AIDecisionChannelManager")

	    .def("Emit", &AIDecisionChannelEmit)
	    .def("EmitWithTarget", &AIDecisionChannelEmitWithTarget)
	    .def("Reset", &AIDecisionChannel::Reset);
}

LuaBindingRegisterFunctionDefinitionForType(ManagerLuaBindings, AIDebugOverlay) {
	return luabind::class_<AIDebugOverlay>("AIDebugOverlayManager")

	    .property("Enabled", &AIDebugOverlay::IsEnabled, &AIDebugOverlay::SetEnabled)
	    .property("WatchedActorId", &AIDebugOverlay::GetWatchedActorId, &AIDebugOverlay::SetWatchedActorId)
	    .property("HistoryLength", &AIDebugOverlay::GetHistoryLength, &AIDebugOverlay::SetHistoryLength)
	    .def("Toggle", &AIDebugOverlay::Toggle);
}

LuaBindingRegisterFunctionDefinitionForType(ManagerLuaBindings, MetricsCollector) {
	return luabind::class_<MetricsCollector>("MetricsCollectorManager")

	    .def("BeginRun", &MetricsCollectorBeginRun)
	    .def("EndRun", &MetricsCollector::EndRun)
	    .def("Record", &MetricsCollector::Record)
	    .def("RecordString", &MetricsCollector::RecordString)
	    .def("SetResult", &MetricsCollector::SetResult)
	    .def("IsSelfTest", &MetricsCollector::IsSelfTest)
	    .def("WriteReport", &MetricsCollectorWriteReport);
}

LuaBindingRegisterFunctionDefinitionForType(ManagerLuaBindings, SimChecksum) {
	return luabind::class_<SimChecksum>("SimChecksumManager")

	    .def("GetTick", &SimChecksumGetTick)
	    .def("GetLastTotalHex", &SimChecksumGetLastTotalHex)
	    .def("GetSubsystemHex", &SimChecksumGetSubsystemHex);
}

LuaBindingRegisterFunctionDefinitionForType(ManagerLuaBindings, NetworkSimulator) {
	return luabind::class_<NetworkSimulator>("NetworkSimulatorManager")

	    .property("LatencyMs", &NetworkSimulator::GetLatencyMs, &NetworkSimulator::SetLatencyMs)
	    .property("LossPct", &NetworkSimulator::GetLossPct, &NetworkSimulator::SetLossPct)
	    .property("JitterMs", &NetworkSimulator::GetJitterMs, &NetworkSimulator::SetJitterMs)
	    .property("IsActive", &NetworkSimulator::IsActive);
}
