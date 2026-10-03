#pragma once

#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace RTE {

	/// The sim dump's values in the order its lines insert them, each with its own type, so the simulation thread only records them and
	/// another thread writes the text exactly as inserting them into a stream would have.
	class SimDumpTape {

	public:
		using Manipulator = std::ios_base& (*)(std::ios_base&);

		SimDumpTape& operator<<(bool value) { return Record(value); }
		SimDumpTape& operator<<(char value) { return Record(value); }
		SimDumpTape& operator<<(signed char value) { return Record(value); }
		SimDumpTape& operator<<(unsigned char value) { return Record(value); }
		SimDumpTape& operator<<(short value) { return Record(value); }
		SimDumpTape& operator<<(unsigned short value) { return Record(value); }
		SimDumpTape& operator<<(int value) { return Record(value); }
		SimDumpTape& operator<<(unsigned value) { return Record(value); }
		SimDumpTape& operator<<(long value) { return Record(value); }
		SimDumpTape& operator<<(unsigned long value) { return Record(value); }
		SimDumpTape& operator<<(long long value) { return Record(value); }
		SimDumpTape& operator<<(unsigned long long value) { return Record(value); }
		SimDumpTape& operator<<(float value) { return Record(value); }
		SimDumpTape& operator<<(double value) { return Record(value); }
		SimDumpTape& operator<<(const char* value) { return Record(std::string(value)); }
		SimDumpTape& operator<<(const std::string& value) { return Record(value); }
		SimDumpTape& operator<<(Manipulator value) { return Record(value); }
		/// Any other type would be written through a conversion the stream does not make; it must be named above.
		template <class T> SimDumpTape& operator<<(const T&) = delete;

		/// Writes the recorded values to a stream, exactly as inserting them there would have.
		/// @param out The stream.
		void Replay(std::ostream& out) const {
			for (const Item& item: m_Items) {
				std::visit([&out](const auto& value) { out << value; }, item);
			}
		}

		/// Nothing is written until the tape is replayed.
		void flush() {}

	private:
		using Item = std::variant<bool, char, signed char, unsigned char, short, unsigned short, int, unsigned, long, unsigned long, long long, unsigned long long, float, double,
		                          std::string, Manipulator>;

		template <class T> SimDumpTape& Record(T value) {
			m_Items.emplace_back(std::in_place_type<T>, std::move(value));
			return *this;
		}

		std::vector<Item> m_Items; //!< The values in insertion order.
	};
} // namespace RTE
