#pragma once

#include "CheckpointLuaHeap.h"

extern "C" {
#include "lj_bc.h"
#include "lj_ctype.h"
#include "lj_dispatch.h"
}

#include <optional>
#include <unordered_set>

// Include at global scope after the script prototype records and their bytecode writer.
namespace RTE::CheckpointLua {

	class PrototypeCapture {
	public:
		explicit PrototypeCapture(Snapshot snapshot) : m_Heap(std::move(snapshot)) {}

		std::shared_ptr<const ScriptGraphPrototype> Capture(const GCproto* source) {
			if (m_Active.contains(source)) throw std::runtime_error("cyclic script prototype");
			if (const auto found = m_Prototypes.find(source); found != m_Prototypes.end()) return found->second;
			const GCproto prototype = m_Heap.Read(source);
			if (prototype.gct != static_cast<uint8_t>(~LJ_TPROTO)) throw std::runtime_error("a frozen script prototype has the wrong type");
			if (prototype.sizebc == 0) throw std::runtime_error("script prototype has no function header");
			m_Heap.ReadBytes(source, prototype.sizept);
			m_Active.insert(source);
			struct ActiveScope {
				std::unordered_set<const GCproto*>& active;
				const GCproto* source;
				~ActiveScope() { active.erase(source); }
			} active{m_Active, source};
			auto captured = std::make_shared<ScriptGraphPrototype>();
			captured->flags = prototype.flags & (PROTO_CHILD | PROTO_VARARG | PROTO_FFI);
			captured->parameters = prototype.numparams;
			captured->frameSize = prototype.framesize;
			captured->firstLine = prototype.firstline;
			captured->lineCount = prototype.numline;
			captured->chunkName = ReadString(reinterpret_cast<const GCstr*>(gcref(prototype.chunkname)));
			captured->bytecode = ReadArray(After<BCIns>(source, sizeof(GCproto)), prototype.sizebc);
#if LJ_HASJIT
			if ((prototype.flags & PROTO_ILOOP) || prototype.trace) {
				for (size_t index = 1; index < captured->bytecode.size(); ++index) {
					BCIns& instruction = captured->bytecode[index];
					const BCOp op = bc_op(instruction);
					if (op == BC_IFORL || op == BC_IITERL || op == BC_ILOOP || op == BC_JFORI) {
						setbc_op(&instruction, static_cast<BCOp>(op - BC_IFORL + BC_FORL));
					} else if (op == BC_JFORL || op == BC_JITERL || op == BC_JLOOP) {
						instruction = TraceStartInstruction(bc_d(instruction));
					}
				}
			}
#endif
			if (prototype.sizeuv) captured->upvalues = ReadArray(mref(prototype.uv, uint16_t), prototype.sizeuv);
			const auto numbers = ReadArray(mref(prototype.k, TValue), prototype.sizekn);
			captured->numbers.reserve(numbers.size());
			for (const TValue& number: numbers) captured->numbers.push_back(number.u64);
			if (const void* debug = mref(prototype.lineinfo, const void)) {
				const uintptr_t start = reinterpret_cast<uintptr_t>(source), address = reinterpret_cast<uintptr_t>(debug);
				if (address < start || address - start > prototype.sizept) throw std::runtime_error("script prototype debug data is out of bounds");
				captured->debug = m_Heap.ReadString(static_cast<const char*>(debug), prototype.sizept - static_cast<size_t>(address - start));
			}
			const auto constants = ReadArray(Before<GCRef>(mref(prototype.k, GCRef), ByteSize<GCRef>(prototype.sizekgc)), prototype.sizekgc);
			captured->constants.reserve(constants.size());
			for (const GCRef& reference: constants) {
				const GCobj* value = gcref(reference);
				const uint8_t type = m_Heap.Read(After<uint8_t>(value, offsetof(GCproto, gct)));
				auto& constant = captured->constants.emplace_back();
				if (type == static_cast<uint8_t>(~LJ_TSTR)) {
					constant.text = ReadString(reinterpret_cast<const GCstr*>(value));
				} else if (type == static_cast<uint8_t>(~LJ_TPROTO)) {
					constant.kind = ScriptGraphPrototype::Constant::Kind::Prototype;
					constant.prototype = Capture(reinterpret_cast<const GCproto*>(value));
				} else if (type == static_cast<uint8_t>(~LJ_TTAB)) {
					constant.kind = ScriptGraphPrototype::Constant::Kind::Table;
					constant.table = CaptureTable(reinterpret_cast<const GCtab*>(value));
#if LJ_HASFFI
				} else if (type == static_cast<uint8_t>(~LJ_TCDATA)) {
					const auto* address = reinterpret_cast<const GCcdata*>(value);
					const GCcdata data = m_Heap.Read(address);
					if (data.ctypeid != CTID_INT64 && data.ctypeid != CTID_UINT64 && data.ctypeid != CTID_COMPLEX_DOUBLE)
						throw std::runtime_error("unsupported script prototype cdata constant");
					constant.kind = ScriptGraphPrototype::Constant::Kind::CData;
					constant.ctype = data.ctypeid;
					constant.cdata = ReadArray(After<uint64_t>(address, sizeof(GCcdata)), data.ctypeid == CTID_COMPLEX_DOUBLE ? 2 : 1);
#endif
				} else throw std::runtime_error("unsupported script prototype constant");
			}
			m_Prototypes.emplace(source, captured);
			return captured;
		}

		std::string Dump(const GCproto* source) {
			if (const auto found = m_Dumps.find(source); found != m_Dumps.end()) return found->second;
			std::string text = SerializeScriptGraphPrototype(*Capture(source));
			m_Dumps.emplace(source, text);
			return text;
		}

	private:
		Snapshot m_Heap;
		std::unordered_map<const GCproto*, std::shared_ptr<const ScriptGraphPrototype>> m_Prototypes;
		std::unordered_map<const GCproto*, std::string> m_Dumps;
		std::unordered_set<const GCproto*> m_Active;
#if LJ_HASJIT
		std::optional<jit_State> m_Jit;
#endif

		template<class T> static size_t ByteSize(size_t count) {
			if (count > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::runtime_error("a frozen script array is too large");
			return count * sizeof(T);
		}

		template<class T> static const T* After(const void* source, size_t bytes) {
			const uintptr_t address = reinterpret_cast<uintptr_t>(source);
			if (!address || bytes > std::numeric_limits<uintptr_t>::max() - address) throw std::runtime_error("a frozen script address overflows");
			return reinterpret_cast<const T*>(address + bytes);
		}

		template<class T> static const T* Before(const void* source, size_t bytes) {
			const uintptr_t address = reinterpret_cast<uintptr_t>(source);
			if (!address || bytes > address) throw std::runtime_error("a frozen script address underflows");
			return reinterpret_cast<const T*>(address - bytes);
		}

		template<class T> std::vector<T> ReadArray(const T* source, size_t count) const {
			static_assert(std::is_trivially_copyable_v<T>);
			const auto bytes = m_Heap.ReadBytes(source, ByteSize<T>(count));
			std::vector<T> values(count);
			if (!bytes.empty()) std::memcpy(values.data(), bytes.data(), bytes.size());
			return values;
		}

		std::string ReadString(const GCstr* source) const {
			const GCstr text = m_Heap.Read(source);
			if (text.gct != static_cast<uint8_t>(~LJ_TSTR)) throw std::runtime_error("a frozen script string has the wrong type");
			return m_Heap.ReadString(After<char>(source, sizeof(GCstr)), text.len);
		}

		ScriptGraphPrototypeValue CaptureValue(const TValue& value) const {
			ScriptGraphPrototypeValue captured;
			if (tvisstr(&value)) {
				captured.kind = ScriptGraphPrototypeValue::Kind::String;
				captured.text = ReadString(reinterpret_cast<const GCstr*>(gcval(&value)));
			} else if (tvistab(&value)) {
				captured.kind = ScriptGraphPrototypeValue::Kind::Table;
			} else if (tvisnumber(&value) || tvispri(&value)) captured.bits = value.u64;
			else throw std::runtime_error("unsupported script prototype table value");
			return captured;
		}

		ScriptGraphPrototypeTable CaptureTable(const GCtab* source) const {
			const GCtab table = m_Heap.Read(source);
			if (table.gct != static_cast<uint8_t>(~LJ_TTAB)) throw std::runtime_error("a frozen prototype table has the wrong type");
			const auto array = ReadArray(tvref(table.array), table.asize);
			size_t count = array.size();
			while (count && tvisnil(&array[count - 1])) --count;
			ScriptGraphPrototypeTable captured;
			captured.array.reserve(count);
			for (size_t index = 0; index < count; ++index) captured.array.push_back(CaptureValue(array[index]));
			struct Entry { TValue key, value; std::string text; };
			std::vector<Entry> entries;
			if (table.hmask) {
				const auto nodes = ReadArray(noderef(table.node), static_cast<size_t>(table.hmask) + 1);
				for (const Node& node: nodes) {
					if (tvisnil(&node.val)) continue;
					auto& entry = entries.emplace_back(Entry{node.key, node.val, {}});
					if (tvisstr(&entry.key)) entry.text = ReadString(reinterpret_cast<const GCstr*>(gcval(&entry.key)));
				}
			}
			std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
				const uint32_t leftType = itype(&left.key), rightType = itype(&right.key);
				if (leftType != rightType) return leftType < rightType;
				if (leftType != LJ_TSTR) return left.key.u64 < right.key.u64;
				const int order = std::memcmp(left.text.data(), right.text.data(), std::min(left.text.size(), right.text.size()));
				return order ? order < 0 : left.text.size() < right.text.size();
			});
			captured.hash.reserve(entries.size());
			for (const Entry& entry: entries) captured.hash.emplace_back(CaptureValue(entry.key), CaptureValue(entry.value));
			return captured;
		}

#if LJ_HASJIT
		BCIns TraceStartInstruction(BCReg number) {
			if (!m_Jit) {
				const lua_State state = m_Heap.Read(m_Heap.State());
				const auto* global = mref(state.glref, global_State);
				m_Jit = m_Heap.Read(After<jit_State>(global, offsetof(GG_State, J) - offsetof(GG_State, g)));
			}
			if (number == 0 || number >= m_Jit->sizetrace) throw std::runtime_error("a frozen script trace number is out of bounds");
			const GCRef reference = m_Heap.Read(After<GCRef>(m_Jit->trace, ByteSize<GCRef>(number)));
			const auto* source = reinterpret_cast<const GCtrace*>(gcref(reference));
			const GCtrace trace = m_Heap.Read(source);
			if (trace.gct != static_cast<uint8_t>(~LJ_TTRACE)) throw std::runtime_error("a frozen script trace has the wrong type");
			return trace.startins;
		}
#endif
	};
}
