#pragma once

#include "CheckpointLuaHeap.h"

#include <bit>
#include <initializer_list>

namespace RTE::CheckpointLua {
	class ScratchImage {
	public:
		using Field = std::pair<std::string_view, TValue>;
		explicit ScratchImage(uint64_t horizon) : m_Serial(horizon), m_Data(std::make_shared<ScratchData>()) {}
		static TValue Nil() { TValue value; setnilV(&value); return value; }
		static TValue Number(double number) { TValue value; setnumV(&value, number); return value; }
		static TValue Boolean(bool flag) { TValue value; setboolV(&value, flag); return value; }
		TValue String(std::string_view text) {
			CheckOpen();
			const auto found = m_Strings.find(std::string(text));
			if (found != m_Strings.end()) return found->second;
			if (text.size() > (std::numeric_limits<MSize>::max)() || text.size() > (std::numeric_limits<size_t>::max)() - sizeof(GCstr) - 1)
				throw std::length_error("frozen descriptor string is too large");
			auto* string = reinterpret_cast<GCstr*>(Allocate(sizeof(GCstr) + text.size() + 1));
			string->gct = static_cast<uint8_t>(~LJ_TSTR);
			string->len = static_cast<MSize>(text.size());
			if (!text.empty()) std::memcpy(string + 1, text.data(), text.size());
			TValue value; setgcVraw(&value, obj2gco(string), LJ_TSTR);
			m_Strings.emplace(std::string(text), value);
			return value;
		}
		TValue Object(std::span<const Field> fields) {
			if (fields.size() > (std::numeric_limits<MSize>::max)()) throw std::length_error("frozen descriptor table is too large");
			const size_t nodes = std::bit_ceil((std::max)(size_t{1}, fields.size()));
			if (nodes > (std::numeric_limits<MSize>::max)() || nodes > (std::numeric_limits<size_t>::max)() / sizeof(Node))
				throw std::length_error("frozen descriptor table is too large");
			auto* values = reinterpret_cast<Node*>(Allocate(nodes * sizeof(Node)));
			for (size_t index = 0; index < nodes; ++index) { setnilV(&values[index].key); setnilV(&values[index].val); }
			std::unordered_set<std::string_view> names;
			for (size_t index = 0; index < fields.size(); ++index) {
				if (!names.insert(fields[index].first).second) throw std::logic_error("duplicate frozen descriptor field");
				values[index].key = String(fields[index].first);
				values[index].val = fields[index].second;
			}
			auto* table = NewTable();
			setmref(table->node, values);
			table->hmask = static_cast<MSize>(nodes - 1);
			TValue value; setgcVraw(&value, obj2gco(table), LJ_TTAB); return value;
		}
		TValue Object(std::initializer_list<Field> fields) { return Object(std::span<const Field>(fields.begin(), fields.size())); }
		TValue Array(std::span<const TValue> elements) {
			if (elements.size() >= (std::numeric_limits<MSize>::max)() || elements.size() >= (std::numeric_limits<size_t>::max)() / sizeof(TValue))
				throw std::length_error("frozen descriptor array is too large");
			auto* values = reinterpret_cast<TValue*>(Allocate((elements.size() + 1) * sizeof(TValue)));
			setnilV(values);
			if (!elements.empty()) std::memcpy(values + 1, elements.data(), elements.size_bytes());
			auto* empty = reinterpret_cast<Node*>(Allocate(sizeof(Node)));
			setnilV(&empty->key); setnilV(&empty->val);
			auto* table = NewTable();
			setmref(table->array, values);
			table->asize = static_cast<MSize>(elements.size() + 1);
			setmref(table->node, empty);
			TValue value; setgcVraw(&value, obj2gco(table), LJ_TTAB); return value;
		}
		TValue Opaque() {
			auto* object = reinterpret_cast<GCudata*>(Allocate(sizeof(GCudata)));
			object->gct = static_cast<uint8_t>(~LJ_TUDATA);
			object->serial = NextSerial();
			m_Data->objects.push_back(obj2gco(object));
			TValue value; setgcVraw(&value, obj2gco(object), LJ_TUDATA); return value;
		}
		Snapshot Finish(const Snapshot& source) {
			CheckOpen();
			m_Data->sealed = true;
			return source.WithScratch(std::exchange(m_Data, nullptr));
		}
	private:
		uint64_t m_Serial;
		std::shared_ptr<ScratchData> m_Data;
		std::unordered_map<std::string, TValue> m_Strings;
		void CheckOpen() const { if (!m_Data || m_Data->sealed) throw std::logic_error("frozen descriptor storage is sealed"); }
		uint64_t NextSerial() {
			if (m_Serial == (std::numeric_limits<uint64_t>::max)()) throw std::overflow_error("frozen descriptor serial overflow");
			return ++m_Serial;
		}
		std::byte* Allocate(size_t bytes) {
			CheckOpen();
			constexpr size_t alignment = alignof(std::max_align_t);
			if (bytes > (std::numeric_limits<size_t>::max)() - alignment) throw std::bad_array_new_length();
			bytes = (bytes + alignment - 1) & ~(alignment - 1);
			if (m_Data->blocks.empty() || bytes > m_Data->blocks.back().size - m_Data->blocks.back().used) {
				CheckpointFailure::Check(CheckpointFailure::Point::LuaAllocation);
				const size_t size = (std::max)(bytes, size_t{64 * 1024});
				m_Data->blocks.push_back({std::unique_ptr<std::byte[]>(new std::byte[size]), size, 0});
			}
			auto& block = m_Data->blocks.back();
			std::byte* result = block.bytes.get() + block.used;
			block.used += bytes;
			std::memset(result, 0, bytes);
			return result;
		}
		GCtab* NewTable() {
			auto* table = reinterpret_cast<GCtab*>(Allocate(sizeof(GCtab)));
			table->gct = static_cast<uint8_t>(~LJ_TTAB);
			table->serial = NextSerial();
			m_Data->objects.push_back(obj2gco(table));
			return table;
		}
	};
}
