#pragma once

#include "Base64/base64.h"
#include "Reader.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace RTE {
	// A missing owned value and an uncaptured value are distinct. Apply only after
	// native construction has finished, preserving an existing owner's address.
	template <class T> std::string CaptureOwnedCheckpoint(const T* value) {
		return value ? value->SaveCheckpoint() : "none";
	}
	template <class T> bool ValidateOwnedCheckpoint(std::string_view text) {
		if (text == "none") return true;
		T candidate;
		return candidate.LoadCheckpoint(text, true);
	}
	template <class T> void ReadOwnedCheckpoint(Reader& reader, std::string& pending) {
		pending = base64_decode(reader.ReadPropValue());
		if (!ValidateOwnedCheckpoint<T>(pending)) reader.ReportError("invalid owned native checkpoint");
	}
	template <class T> void RestoreOwnedCheckpoint(T*& value, std::string& pending) {
		if (pending.empty()) return;
		if (pending == "none") {
			delete value;
			value = nullptr;
		} else {
			if (!value) value = new T;
			if (!value->LoadCheckpoint(pending)) throw std::runtime_error("could not restore owned native checkpoint");
		}
		pending.clear();
	}
}
