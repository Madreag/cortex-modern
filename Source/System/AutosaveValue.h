#pragma once

#include "CheckpointFailure.h"

#include <memory>
#include <string>
#include <utility>

namespace RTE {

	// A copied identity retains its values while the live match changes its next identity.
	template<class T> class AutosaveValue {
	public:
		AutosaveValue() = default;
		AutosaveValue(const AutosaveValue&) noexcept = default;
		AutosaveValue(AutosaveValue&&) noexcept = default;
		AutosaveValue& operator=(const AutosaveValue&) noexcept = default;
		AutosaveValue& operator=(AutosaveValue&&) noexcept = default;
		AutosaveValue(const T& value) { Assign(value); }
		AutosaveValue(T&& value) { Assign(std::move(value)); }
		AutosaveValue& operator=(const T& value) { Assign(value); return *this; }
		AutosaveValue& operator=(T&& value) { Assign(std::move(value)); return *this; }
		const T& Value() const {
			static const T empty;
			return m_Value ? *m_Value : empty;
		}
		operator const T&() const { return Value(); }
		bool empty() const { return Value().empty(); }
		size_t size() const { return Value().size(); }
		void clear() noexcept { m_Value.reset(); }
		template<class Item> void push_back(Item&& item) {
			if (m_Value && m_Value.use_count() == 1) { m_Value->push_back(std::forward<Item>(item)); return; }
			CheckpointFailure::Check(CheckpointFailure::Point::NativeObjects);
			auto value = std::make_shared<T>(Value());
			value->push_back(std::forward<Item>(item));
			m_Value = std::move(value);
		}
		friend bool operator==(const AutosaveValue& left, const AutosaveValue& right) { return left.Value() == right.Value(); }
		friend bool operator==(const AutosaveValue& left, const T& right) { return left.Value() == right; }
		template<class Json> friend void to_json(Json& into, const AutosaveValue& value) { into = value.Value(); }
	private:
		template<class Value> void Assign(Value&& value) {
			CheckpointFailure::Check(CheckpointFailure::Point::NativeObjects);
			auto owned = std::make_shared<T>(std::forward<Value>(value));
			m_Value = std::move(owned);
		}
		std::shared_ptr<T> m_Value;
	};
}
