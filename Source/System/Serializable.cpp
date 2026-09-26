#include "Serializable.h"
#include "CaptureSentinel.h"
#include "Entity.h"

#include <optional>

namespace RTE {

	int Serializable::CreateSerializable(Reader& reader, bool checkType, bool doCreate, bool skipStartingObject) {
		if (checkType && reader.ReadPropValue() != GetClassName()) {
			reader.ReportError("Wrong type in Reader when passed to Serializable::Create()");
			return -1;
		}

		if (!skipStartingObject) {
			reader.StartObject();
		}
		while (reader.NextProperty()) {
			SetFormattedReaderPosition("in file " + reader.GetCurrentFilePath() + " on line " + reader.GetCurrentFileLine());
			std::string propName = reader.ReadPropName();
			// We need to check if !propName.empty() because ReadPropName may return "" when it reads an IncludeFile without any properties in case they are all commented out or it's the last line in file.
			// Also ReadModuleProperty may return "" when it skips IncludeFile till the end of file.
			if (!propName.empty() && ReadProperty(propName, reader) < 0) {
				// TODO: Could not match property. Log here!
			}
		}

		return doCreate ? Create() : 0;
	}

	int Serializable::ReadProperty(const std::string_view& propName, Reader& reader) {
		reader.ReadPropValue();
		reader.ReportUnknownProperty(GetClassName(), propName);
		return -1;
	}

	Reader& operator>>(Reader& reader, Serializable& operand) {
		operand.Create(reader);
		return reader;
	}

	Reader& operator>>(Reader& reader, Serializable* operand) {
		if (operand) {
			operand->Create(reader);
		}
		return reader;
	}

	namespace {
		thread_local int t_TracedDepth = 0;
		// A nested object's write, timed while a capture trace runs and the nesting is shallow enough.
		struct TracedWrite {
			std::optional<CaptureTrace::Span> span;
			bool counted = false;
			explicit TracedWrite(const Serializable& operand) {
				if (!CaptureTrace::Active()) return;
				counted = true;
				if (++t_TracedDepth > CaptureTrace::ObjectDepth()) return;
				const auto* entity = dynamic_cast<const Entity*>(&operand);
				span.emplace("obj", std::to_string(t_TracedDepth) + ":" + operand.GetClassName() + ":" + (entity ? entity->GetPresetName() : std::string()));
			}
			~TracedWrite() {
				span.reset();
				if (counted) --t_TracedDepth;
			}
		};
	} // namespace

	Writer& operator<<(Writer& writer, const Serializable& operand) {
		TracedWrite traced(operand);
		operand.Save(writer);
		writer.ObjectEnd();
		return writer;
	}

	Writer& operator<<(Writer& writer, const Serializable* operand) {
		if (operand) {
			TracedWrite traced(*operand);
			operand->Save(writer);
			writer.ObjectEnd();
		} else {
			writer.NoObject();
		}
		return writer;
	}
} // namespace RTE
