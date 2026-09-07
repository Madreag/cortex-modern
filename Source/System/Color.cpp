#include "Color.h"
#include "CheckpointArchive.h"
#include "allegro/color.h"

using namespace RTE;

const std::string Color::c_ClassName = "Color";

std::string Color::SaveCheckpoint() const {
	CheckpointWriter writer("Color1");
	writer(m_R, m_G, m_B, m_Index);
	return writer.Text();
}

bool Color::LoadCheckpoint(std::string_view text, bool validateOnly) {
	try {
		CheckpointReader reader(text, "Color1", validateOnly);
		reader(m_R, m_G, m_B, m_Index);
		reader.Finish();
		return true;
	} catch (const std::exception&) { return false; }
}

int Color::Create() {
	if (Serializable::Create()) {
		return -1;
	}
	RecalculateIndex();
	return 0;
}

int Color::Create(int inputR, int inputG, int inputB) {
	SetR(inputR);
	SetG(inputG);
	SetB(inputB);
	RecalculateIndex();
	return 0;
}

int Color::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Serializable::ReadProperty(propName, reader));

	MatchProperty("Index", { SetRGBWithIndex(std::stoi(reader.ReadPropValue())); });
	MatchProperty("R", { SetR(std::stoi(reader.ReadPropValue())); });
	MatchProperty("G", { SetG(std::stoi(reader.ReadPropValue())); });
	MatchProperty("B", { SetB(std::stoi(reader.ReadPropValue())); });

	EndPropertyList;
}

int Color::Save(Writer& writer) const {
	Serializable::Save(writer);

	writer.NewPropertyWithValue("R", m_R);
	writer.NewPropertyWithValue("G", m_G);
	writer.NewPropertyWithValue("B", m_B);

	return 0;
}

void Color::SetRGBWithIndex(int index) {
	m_Index = std::clamp(index, 0, 255);

	RGB rgbColor;
	get_color(m_Index, &rgbColor);

	// Multiply by 4 because the Allegro RGB struct elements are in range 0-63, and proper RGB needs 0-255.
	m_R = rgbColor.r;
	m_G = rgbColor.g;
	m_B = rgbColor.b;
}

int Color::RecalculateIndex() {
	return m_Index = makecol8(m_R, m_G, m_B);
}
