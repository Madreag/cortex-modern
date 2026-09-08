#include "PrimitiveMan.h"

#include "FrameMan.h"
#include "SceneMan.h"
#include "ConsoleMan.h"
#include "MOSprite.h"
#include "Shader.h"

#include "tracy/Tracy.hpp"

#include <array>
#include "Draw.h"
#include "glad/gl.h"
#include "Shader.h"
#include "PresetMan.h"
#include "CheckpointArchive.h"
#include "GUICheckpoint.h"
#include <iostream>
#include <set>
#include <unordered_map>

using namespace RTE;

std::unique_ptr<GraphicalPrimitive> PrimitiveMan::MakeUniqueOfAppropriateTypeFromPrimitiveRawPtr(GraphicalPrimitive* primitive) {
	switch (primitive->GetPrimitiveType()) {
		case GraphicalPrimitive::PrimitiveType::Line:
			return std::unique_ptr<LinePrimitive>(static_cast<LinePrimitive*>(primitive));
		case GraphicalPrimitive::PrimitiveType::Arc:
			return std::unique_ptr<ArcPrimitive>(static_cast<ArcPrimitive*>(primitive));
		case GraphicalPrimitive::PrimitiveType::Spline:
			return std::unique_ptr<SplinePrimitive>(static_cast<SplinePrimitive*>(primitive));
		case GraphicalPrimitive::PrimitiveType::Box:
			return std::unique_ptr<BoxPrimitive>(static_cast<BoxPrimitive*>(primitive));
		case GraphicalPrimitive::PrimitiveType::BoxFill:
			return std::unique_ptr<BoxFillPrimitive>(static_cast<BoxFillPrimitive*>(primitive));
		case GraphicalPrimitive::PrimitiveType::RoundedBox:
			return std::unique_ptr<RoundedBoxPrimitive>(static_cast<RoundedBoxPrimitive*>(primitive));
		case GraphicalPrimitive::PrimitiveType::RoundedBoxFill:
			return std::unique_ptr<RoundedBoxFillPrimitive>(static_cast<RoundedBoxFillPrimitive*>(primitive));
		case GraphicalPrimitive::PrimitiveType::Circle:
			return std::unique_ptr<CirclePrimitive>(static_cast<CirclePrimitive*>(primitive));
		case GraphicalPrimitive::PrimitiveType::CircleFill:
			return std::unique_ptr<CircleFillPrimitive>(static_cast<CircleFillPrimitive*>(primitive));
		case GraphicalPrimitive::PrimitiveType::Ellipse:
			return std::unique_ptr<EllipsePrimitive>(static_cast<EllipsePrimitive*>(primitive));
		case GraphicalPrimitive::PrimitiveType::EllipseFill:
			return std::unique_ptr<EllipseFillPrimitive>(static_cast<EllipseFillPrimitive*>(primitive));
		case GraphicalPrimitive::PrimitiveType::Triangle:
			return std::unique_ptr<TrianglePrimitive>(static_cast<TrianglePrimitive*>(primitive));
		case GraphicalPrimitive::PrimitiveType::TriangleFill:
			return std::unique_ptr<TriangleFillPrimitive>(static_cast<TriangleFillPrimitive*>(primitive));
		case GraphicalPrimitive::PrimitiveType::Polygon:
			return std::unique_ptr<PolygonPrimitive>(static_cast<PolygonPrimitive*>(primitive));
		case GraphicalPrimitive::PrimitiveType::PolygonFill:
			return std::unique_ptr<PolygonFillPrimitive>(static_cast<PolygonFillPrimitive*>(primitive));
		case GraphicalPrimitive::PrimitiveType::Text:
			return std::unique_ptr<TextPrimitive>(static_cast<TextPrimitive*>(primitive));
		case GraphicalPrimitive::PrimitiveType::Bitmap:
			return std::unique_ptr<BitmapPrimitive>(static_cast<BitmapPrimitive*>(primitive));
		default:
			return nullptr;
	}
}

void PrimitiveMan::SchedulePrimitive(std::unique_ptr<GraphicalPrimitive>&& primitive) {
	std::lock_guard<std::mutex> lock(m_Mutex);
	m_ScheduledPrimitives.emplace_back(std::move(primitive));
}

void PrimitiveMan::SchedulePrimitivesForBlendedDrawing(DrawBlendMode blendMode, int blendAmountR, int blendAmountG, int blendAmountB, int blendAmountA, const std::vector<GraphicalPrimitive*>& primitives) {
	if (blendMode < DrawBlendMode::NoBlend || blendMode >= DrawBlendMode::BlendModeCount) {
		g_ConsoleMan.PrintString("ERROR: Encountered invalid blending mode when attempting to draw primitives! Drawing will be skipped! See the DrawBlendMode enumeration for valid modes.");
		return;
	}
	blendAmountR = std::clamp(blendAmountR, static_cast<int>(BlendAmountLimits::MinBlend), static_cast<int>(BlendAmountLimits::MaxBlend));
	blendAmountG = std::clamp(blendAmountG, static_cast<int>(BlendAmountLimits::MinBlend), static_cast<int>(BlendAmountLimits::MaxBlend));
	blendAmountB = std::clamp(blendAmountB, static_cast<int>(BlendAmountLimits::MinBlend), static_cast<int>(BlendAmountLimits::MaxBlend));
	blendAmountA = std::clamp(blendAmountA, static_cast<int>(BlendAmountLimits::MinBlend), static_cast<int>(BlendAmountLimits::MaxBlend));

	for (GraphicalPrimitive* primitive: primitives) {
		primitive->m_BlendMode = blendMode;
		primitive->m_ColorChannelBlendAmounts = {blendAmountR, blendAmountG, blendAmountB, blendAmountA};
		SchedulePrimitive(MakeUniqueOfAppropriateTypeFromPrimitiveRawPtr(primitive));
	}
}

void PrimitiveMan::DrawLinePrimitive(int player, const Vector& startPos, const Vector& endPos, unsigned char color, int thickness) {
	SchedulePrimitive(std::make_unique<LinePrimitive>(player, startPos, endPos, thickness, color));
}

void PrimitiveMan::DrawArcPrimitive(const Vector& centerPos, float startAngle, float endAngle, int radius, unsigned char color) {
	SchedulePrimitive(std::make_unique<ArcPrimitive>(-1, centerPos, startAngle, endAngle, radius, 1, color));
}

void PrimitiveMan::DrawArcPrimitive(const Vector& centerPos, float startAngle, float endAngle, int radius, unsigned char color, int thickness) {
	SchedulePrimitive(std::make_unique<ArcPrimitive>(-1, centerPos, startAngle, endAngle, radius, thickness, color));
}

void PrimitiveMan::DrawArcPrimitive(int player, const Vector& centerPos, float startAngle, float endAngle, int radius, unsigned char color) {
	SchedulePrimitive(std::make_unique<ArcPrimitive>(player, centerPos, startAngle, endAngle, radius, 1, color));
}

void PrimitiveMan::DrawArcPrimitive(int player, const Vector& centerPos, float startAngle, float endAngle, int radius, unsigned char color, int thickness) {
	SchedulePrimitive(std::make_unique<ArcPrimitive>(player, centerPos, startAngle, endAngle, radius, thickness, color));
}

void PrimitiveMan::DrawSplinePrimitive(const Vector& startPos, const Vector& guideA, const Vector& guideB, const Vector& endPos, unsigned char color) {
	SchedulePrimitive(std::make_unique<SplinePrimitive>(-1, startPos, guideA, guideB, endPos, color));
}

void PrimitiveMan::DrawSplinePrimitive(int player, const Vector& startPos, const Vector& guideA, const Vector& guideB, const Vector& endPos, unsigned char color) {
	SchedulePrimitive(std::make_unique<SplinePrimitive>(player, startPos, guideA, guideB, endPos, color));
}

void PrimitiveMan::DrawBoxPrimitive(const Vector& topLeftPos, const Vector& bottomRightPos, unsigned char color) {
	SchedulePrimitive(std::make_unique<BoxPrimitive>(-1, topLeftPos, bottomRightPos, color));
}

void PrimitiveMan::DrawBoxPrimitive(int player, const Vector& topLeftPos, const Vector& bottomRightPos, unsigned char color) {
	SchedulePrimitive(std::make_unique<BoxPrimitive>(player, topLeftPos, bottomRightPos, color));
}

void PrimitiveMan::DrawBoxFillPrimitive(const Vector& topLeftPos, const Vector& bottomRightPos, unsigned char color) {
	SchedulePrimitive(std::make_unique<BoxFillPrimitive>(-1, topLeftPos, bottomRightPos, color));
}

void PrimitiveMan::DrawBoxFillPrimitive(int player, const Vector& topLeftPos, const Vector& bottomRightPos, unsigned char color) {
	SchedulePrimitive(std::make_unique<BoxFillPrimitive>(player, topLeftPos, bottomRightPos, color));
}

void PrimitiveMan::DrawRoundedBoxPrimitive(const Vector& topLeftPos, const Vector& bottomRightPos, int cornerRadius, unsigned char color) {
	SchedulePrimitive(std::make_unique<RoundedBoxPrimitive>(-1, topLeftPos, bottomRightPos, cornerRadius, color));
}

void PrimitiveMan::DrawRoundedBoxPrimitive(int player, const Vector& topLeftPos, const Vector& bottomRightPos, int cornerRadius, unsigned char color) {
	SchedulePrimitive(std::make_unique<RoundedBoxPrimitive>(player, topLeftPos, bottomRightPos, cornerRadius, color));
}

void PrimitiveMan::DrawRoundedBoxFillPrimitive(const Vector& topLeftPos, const Vector& bottomRightPos, int cornerRadius, unsigned char color) {
	SchedulePrimitive(std::make_unique<RoundedBoxFillPrimitive>(-1, topLeftPos, bottomRightPos, cornerRadius, color));
}

void PrimitiveMan::DrawRoundedBoxFillPrimitive(int player, const Vector& topLeftPos, const Vector& bottomRightPos, int cornerRadius, unsigned char color) {
	SchedulePrimitive(std::make_unique<RoundedBoxFillPrimitive>(player, topLeftPos, bottomRightPos, cornerRadius, color));
}

void PrimitiveMan::DrawCirclePrimitive(const Vector& centerPos, int radius, unsigned char color) {
	SchedulePrimitive(std::make_unique<CirclePrimitive>(-1, centerPos, radius, color));
}

void PrimitiveMan::DrawCirclePrimitive(int player, const Vector& centerPos, int radius, unsigned char color) {
	SchedulePrimitive(std::make_unique<CirclePrimitive>(player, centerPos, radius, color));
}

void PrimitiveMan::DrawCircleFillPrimitive(const Vector& centerPos, int radius, unsigned char color) {
	SchedulePrimitive(std::make_unique<CircleFillPrimitive>(-1, centerPos, radius, color));
}

void PrimitiveMan::DrawCircleFillPrimitive(int player, const Vector& centerPos, int radius, unsigned char color) {
	SchedulePrimitive(std::make_unique<CircleFillPrimitive>(player, centerPos, radius, color));
}

void PrimitiveMan::DrawEllipsePrimitive(const Vector& centerPos, int horizRadius, int vertRadius, unsigned char color) {
	SchedulePrimitive(std::make_unique<EllipsePrimitive>(-1, centerPos, horizRadius, vertRadius, color));
}

void PrimitiveMan::DrawEllipsePrimitive(int player, const Vector& centerPos, int horizRadius, int vertRadius, unsigned char color) {
	SchedulePrimitive(std::make_unique<EllipsePrimitive>(player, centerPos, horizRadius, vertRadius, color));
}

void PrimitiveMan::DrawEllipseFillPrimitive(const Vector& centerPos, int horizRadius, int vertRadius, unsigned char color) {
	SchedulePrimitive(std::make_unique<EllipseFillPrimitive>(-1, centerPos, horizRadius, vertRadius, color));
}

void PrimitiveMan::DrawEllipseFillPrimitive(int player, const Vector& centerPos, int horizRadius, int vertRadius, unsigned char color) {
	SchedulePrimitive(std::make_unique<EllipseFillPrimitive>(player, centerPos, horizRadius, vertRadius, color));
}

void PrimitiveMan::DrawTrianglePrimitive(const Vector& pointA, const Vector& pointB, const Vector& pointC, unsigned char color) {
	SchedulePrimitive(std::make_unique<TrianglePrimitive>(-1, pointA, pointB, pointC, color));
}

void PrimitiveMan::DrawTrianglePrimitive(int player, const Vector& pointA, const Vector& pointB, const Vector& pointC, unsigned char color) {
	SchedulePrimitive(std::make_unique<TrianglePrimitive>(player, pointA, pointB, pointC, color));
}

void PrimitiveMan::DrawTriangleFillPrimitive(const Vector& pointA, const Vector& pointB, const Vector& pointC, unsigned char color) {
	SchedulePrimitive(std::make_unique<TriangleFillPrimitive>(-1, pointA, pointB, pointC, color));
}

void PrimitiveMan::DrawTriangleFillPrimitive(int player, const Vector& pointA, const Vector& pointB, const Vector& pointC, unsigned char color) {
	SchedulePrimitive(std::make_unique<TriangleFillPrimitive>(player, pointA, pointB, pointC, color));
}

void PrimitiveMan::DrawPolygonOrPolygonFillPrimitive(int player, const Vector& startPos, unsigned char color, const std::vector<Vector*>& vertices, bool filled) {
	if (vertices.size() < 2) {
		g_ConsoleMan.PrintString("ERROR: Polygon primitive should have at least 2 vertices! Drawing will be skipped!");
		return;
	}
	if (filled) {
		SchedulePrimitive(std::make_unique<PolygonFillPrimitive>(player, startPos, color, vertices));
	} else {
		SchedulePrimitive(std::make_unique<PolygonPrimitive>(player, startPos, color, vertices));
	}
}

void PrimitiveMan::DrawTextPrimitive(const Vector& start, const std::string& text, bool isSmall, int alignment) {
	SchedulePrimitive(std::make_unique<TextPrimitive>(-1, start, text, isSmall, alignment, 0));
}

void PrimitiveMan::DrawTextPrimitive(const Vector& start, const std::string& text, bool isSmall, int alignment, float rotAngle) {
	SchedulePrimitive(std::make_unique<TextPrimitive>(-1, start, text, isSmall, alignment, rotAngle));
}

void PrimitiveMan::DrawTextPrimitive(int player, const Vector& start, const std::string& text, bool isSmall, int alignment) {
	SchedulePrimitive(std::make_unique<TextPrimitive>(player, start, text, isSmall, alignment, 0));
}

void PrimitiveMan::DrawTextPrimitive(int player, const Vector& start, const std::string& text, bool isSmall, int alignment, float rotAngle) {
	SchedulePrimitive(std::make_unique<TextPrimitive>(player, start, text, isSmall, alignment, rotAngle));
}

void PrimitiveMan::DrawBitmapPrimitive(int player, const Vector& centerPos, const MOSprite* moSprite, float rotAngle, unsigned int frame, float scale, bool hFlipped, bool vFlipped) {
	SchedulePrimitive(std::make_unique<BitmapPrimitive>(player, centerPos, moSprite, rotAngle, frame, scale, hFlipped, vFlipped));
}

void PrimitiveMan::DrawBitmapPrimitive(int player, const Vector& centerPos, const std::string& filePath, float rotAngle, float scale, bool hFlipped, bool vFlipped) {
	SchedulePrimitive(std::make_unique<BitmapPrimitive>(player, centerPos, filePath, rotAngle, scale, hFlipped, vFlipped));
}

void PrimitiveMan::DrawIconPrimitive(int player, const Vector& centerPos, Entity* entity) {
	if (const MOSprite* moSprite = dynamic_cast<MOSprite*>(entity)) {
		auto primitive = std::make_unique<BitmapPrimitive>(player, centerPos, moSprite->GetGraphicalIcon(), 0, false, false);
		primitive->m_SpriteOwner = moSprite; primitive->m_IconBitmap = true;
		primitive->m_ImageOwners.push_back(moSprite->ShareSpriteBitmap(moSprite->GetGraphicalIcon()));
		SchedulePrimitive(std::move(primitive));
	}
}

namespace {
	using PrimitiveType = GraphicalPrimitive::PrimitiveType;
	struct PrimitiveCheckpointPool {
		std::vector<const BITMAP*> images;
		std::vector<const Vector*> vertices;
		std::unordered_map<const BITMAP*, size_t> imageIDs;
		std::unordered_map<const Vector*, size_t> vertexIDs;
		size_t Image(const BITMAP* image) {
			if (!image) return 0;
			auto [found, added] = imageIDs.emplace(image, images.size() + 1);
			if (added) images.push_back(image);
			return found->second;
		}
		size_t Vertex(const Vector* vertex) {
			if (!vertex) return 0;
			auto [found, added] = vertexIDs.emplace(vertex, vertices.size() + 1);
			if (added) vertices.push_back(vertex);
			return found->second;
		}
	};
	template <class Archive> void PrimitiveScalarFields(Archive& archive, GraphicalPrimitive& primitive) {
		auto fields = [&](auto&... values) { (archive.Value(values), ...); };
		fields(primitive.m_StartPos, primitive.m_EndPos, primitive.m_DrawRadiusSquared, primitive.m_Color, primitive.m_Player,
		       primitive.m_BlendMode, primitive.m_ColorChannelBlendAmounts, primitive.m_Depth);
		switch (primitive.GetPrimitiveType()) {
			case PrimitiveType::Line: fields(static_cast<LinePrimitive&>(primitive).m_Thickness); break;
			case PrimitiveType::Arc: { auto& value = static_cast<ArcPrimitive&>(primitive); fields(value.m_StartAngle, value.m_EndAngle, value.m_Radius, value.m_Thickness); break; }
			case PrimitiveType::Spline: { auto& value = static_cast<SplinePrimitive&>(primitive); fields(value.m_GuidePointAPos, value.m_GuidePointBPos); break; }
			case PrimitiveType::RoundedBox: fields(static_cast<RoundedBoxPrimitive&>(primitive).m_CornerRadius); break;
			case PrimitiveType::RoundedBoxFill: fields(static_cast<RoundedBoxFillPrimitive&>(primitive).m_CornerRadius); break;
			case PrimitiveType::Circle: fields(static_cast<CirclePrimitive&>(primitive).m_Radius); break;
			case PrimitiveType::CircleFill: fields(static_cast<CircleFillPrimitive&>(primitive).m_Radius); break;
			case PrimitiveType::Ellipse: { auto& value = static_cast<EllipsePrimitive&>(primitive); fields(value.m_HorizRadius, value.m_VertRadius); break; }
			case PrimitiveType::EllipseFill: { auto& value = static_cast<EllipseFillPrimitive&>(primitive); fields(value.m_HorizRadius, value.m_VertRadius); break; }
			case PrimitiveType::Triangle: { auto& value = static_cast<TrianglePrimitive&>(primitive); fields(value.m_PointAPos, value.m_PointBPos, value.m_PointCPos); break; }
			case PrimitiveType::TriangleFill: { auto& value = static_cast<TriangleFillPrimitive&>(primitive); fields(value.m_PointAPos, value.m_PointBPos, value.m_PointCPos); break; }
			case PrimitiveType::Text: { auto& value = static_cast<TextPrimitive&>(primitive); fields(value.m_Text, value.m_IsSmall, value.m_Alignment, value.m_RotAngle, value.m_TargetPosAlignment); break; }
			case PrimitiveType::Bitmap: { auto& value = static_cast<BitmapPrimitive&>(primitive); fields(value.m_RotAngle, value.m_HFlipped, value.m_VFlipped, value.m_Scale, value.m_SpriteFrame, value.m_IconBitmap); break; }
			case PrimitiveType::Box: case PrimitiveType::BoxFill: case PrimitiveType::Polygon: case PrimitiveType::PolygonFill: break;
			default: throw std::runtime_error("unsupported graphical primitive type");
		}
	}
	std::string SavePrimitiveRecord(const GraphicalPrimitive& primitive, PrimitiveCheckpointPool& pool) {
		CheckpointWriter writer("GraphicalPrimitive1");
		writer(primitive.GetPrimitiveType());
		PrimitiveScalarFields(writer, const_cast<GraphicalPrimitive&>(primitive));
		if (primitive.GetPrimitiveType() == PrimitiveType::Text) writer(pool.Image(static_cast<const TextPrimitive&>(primitive).m_TextBitmap));
		else if (primitive.GetPrimitiveType() == PrimitiveType::Bitmap) {
			const auto& bitmap = static_cast<const BitmapPrimitive&>(primitive);
			const auto* owner = static_cast<const MOSprite*>(bitmap.m_SpriteOwner.get());
			if (owner && (bitmap.m_IconBitmap ? owner->GetGraphicalIcon() : owner->GetSpriteFrame(bitmap.m_SpriteFrame)) != bitmap.m_Bitmap) owner = nullptr;
			writer(pool.Image(bitmap.m_Bitmap), bitmap.m_PendingSpriteReference.empty() ? GUICheckpoint::SaveEntityReference(owner) : bitmap.m_PendingSpriteReference);
		} else if (primitive.GetPrimitiveType() == PrimitiveType::Polygon || primitive.GetPrimitiveType() == PrimitiveType::PolygonFill) {
			const auto& vertices = primitive.GetPrimitiveType() == PrimitiveType::Polygon ? static_cast<const PolygonPrimitive&>(primitive).m_Vertices : static_cast<const PolygonFillPrimitive&>(primitive).m_Vertices;
			writer(vertices.size());
			for (const auto* vertex: vertices) writer(pool.Vertex(vertex));
		}
		return writer.Text();
	}
	std::string SavePrimitiveList(const std::vector<const GraphicalPrimitive*>& primitives, const char* version) {
		PrimitiveCheckpointPool pool;
		std::vector<std::string> records;
		for (const auto* primitive: primitives) {
			if (!primitive) throw std::runtime_error("null scheduled graphical primitive");
			records.push_back(SavePrimitiveRecord(*primitive, pool));
		}
		CheckpointWriter writer(version);
		writer(pool.images.size());
		for (const auto* bitmap: pool.images) writer(GUICheckpoint::SaveSharedBitmap(bitmap));
		writer(pool.vertices.size());
		for (const auto* vertex: pool.vertices) writer(*vertex);
		writer(records);
		return writer.Text();
	}
	std::deque<std::unique_ptr<GraphicalPrimitive>> LoadPrimitiveList(std::string_view text, const char* version, bool validateOnly) {
		CheckpointReader reader(text, version, true);
		std::vector<std::string> imageValues, records;
		std::vector<Vector> vertexValues;
		reader.Value(imageValues); reader.Value(vertexValues); reader.Value(records);
		std::vector<std::shared_ptr<BITMAP>> images;
		for (const auto& value: imageValues) {
			GUICheckpoint::LoadSharedBitmap(value, true);
			if (!validateOnly) {
				auto bitmap = GUICheckpoint::LoadSharedBitmap(value);
				if (!bitmap) throw std::runtime_error("null graphical primitive bitmap owner");
				images.push_back(std::move(bitmap));
			}
		}
		std::vector<std::shared_ptr<Vector>> vertices;
		if (!validateOnly) for (const auto& value: vertexValues) vertices.push_back(GraphicalPrimitive::OwnVertices({new Vector(value)}).front());
		std::deque<std::unique_ptr<GraphicalPrimitive>> primitives;
		for (const auto& record: records) {
			CheckpointReader fields(record, "GraphicalPrimitive1", true);
			PrimitiveType type; fields.Value(type);
			auto primitive = GraphicalPrimitive::CreateCheckpointType(type);
			PrimitiveScalarFields(fields, *primitive);
			if (primitive->m_BlendMode < DrawBlendMode::NoBlend || primitive->m_BlendMode >= DrawBlendMode::BlendModeCount) throw std::runtime_error("invalid graphical primitive blend mode");
			if (type == PrimitiveType::Text || type == PrimitiveType::Bitmap) {
				size_t image; fields.Value(image);
				if (image > imageValues.size()) throw std::runtime_error("invalid graphical primitive bitmap reference");
				BITMAP* bitmap = image && !validateOnly ? images[image - 1].get() : nullptr;
				if (type == PrimitiveType::Text) static_cast<TextPrimitive&>(*primitive).m_TextBitmap = bitmap;
				else {
					auto& value = static_cast<BitmapPrimitive&>(*primitive); value.m_Bitmap = bitmap;
					fields.Value(value.m_PendingSpriteReference);
					GUICheckpoint::LoadEntityReference(value.m_PendingSpriteReference, true);
				}
				if (image && !validateOnly) primitive->m_ImageOwners.push_back(images[image - 1]);
			} else if (type == PrimitiveType::Polygon || type == PrimitiveType::PolygonFill) {
				std::vector<size_t> references; fields.Value(references);
				auto& points = type == PrimitiveType::Polygon ? static_cast<PolygonPrimitive&>(*primitive).m_Vertices : static_cast<PolygonFillPrimitive&>(*primitive).m_Vertices;
				for (size_t reference: references) {
					if (reference > vertexValues.size()) throw std::runtime_error("invalid graphical primitive vertex reference");
					if (!validateOnly) { points.push_back(reference ? vertices[reference - 1].get() : nullptr); primitive->m_VertexOwners.push_back(reference ? vertices[reference - 1] : nullptr); }
				}
			}
			fields.Finish(); primitives.push_back(std::move(primitive));
		}
		reader.Finish();
		return primitives;
	}
	std::vector<Vector*> PrimitiveVertices(const std::deque<std::unique_ptr<GraphicalPrimitive>>& primitives) {
		std::vector<Vector*> vertices;
		std::set<Vector*> seen;
		for (const auto& primitive: primitives) {
			const std::vector<Vector*>* points = nullptr;
			if (primitive->GetPrimitiveType() == PrimitiveType::Polygon) points = &static_cast<PolygonPrimitive&>(*primitive).m_Vertices;
			else if (primitive->GetPrimitiveType() == PrimitiveType::PolygonFill) points = &static_cast<PolygonFillPrimitive&>(*primitive).m_Vertices;
			if (points) for (auto* point: *points) if (point && seen.insert(point).second) vertices.push_back(point);
		}
		return vertices;
	}
}

const char* GraphicalPrimitive::CheckpointTypeName(PrimitiveType type) {
	static const std::array<const char*, 18> names{"GraphicalPrimitive", "LinePrimitive", "ArcPrimitive", "SplinePrimitive", "BoxPrimitive", "BoxFillPrimitive", "RoundedBoxPrimitive", "RoundedBoxFillPrimitive", "CirclePrimitive", "CircleFillPrimitive", "EllipsePrimitive", "EllipseFillPrimitive", "TrianglePrimitive", "TriangleFillPrimitive", "PolygonPrimitive", "PolygonFillPrimitive", "TextPrimitive", "BitmapPrimitive"};
	if (static_cast<size_t>(type) >= names.size()) throw std::runtime_error("invalid graphical primitive type");
	return names[static_cast<size_t>(type)];
}

std::unique_ptr<GraphicalPrimitive> GraphicalPrimitive::CreateCheckpointType(PrimitiveType type) {
	switch (type) {
		case PrimitiveType::Line: return std::make_unique<LinePrimitive>(-1, Vector(), Vector(), static_cast<unsigned char>(0));
		case PrimitiveType::Arc: return std::make_unique<ArcPrimitive>(-1, Vector(), 0, 0, 0, 0, 0);
		case PrimitiveType::Spline: return std::make_unique<SplinePrimitive>(-1, Vector(), Vector(), Vector(), Vector(), 0);
		case PrimitiveType::Box: return std::make_unique<BoxPrimitive>(-1, Vector(), Vector(), 0);
		case PrimitiveType::BoxFill: return std::make_unique<BoxFillPrimitive>(-1, Vector(), Vector(), 0);
		case PrimitiveType::RoundedBox: return std::make_unique<RoundedBoxPrimitive>(-1, Vector(), Vector(), 0, 0);
		case PrimitiveType::RoundedBoxFill: return std::make_unique<RoundedBoxFillPrimitive>(-1, Vector(), Vector(), 0, 0);
		case PrimitiveType::Circle: return std::make_unique<CirclePrimitive>(-1, Vector(), 0, 0);
		case PrimitiveType::CircleFill: return std::make_unique<CircleFillPrimitive>(-1, Vector(), 0, 0);
		case PrimitiveType::Ellipse: return std::make_unique<EllipsePrimitive>(-1, Vector(), 0, 0, 0);
		case PrimitiveType::EllipseFill: return std::make_unique<EllipseFillPrimitive>(-1, Vector(), 0, 0, 0);
		case PrimitiveType::Triangle: return std::make_unique<TrianglePrimitive>(-1, Vector(), Vector(), Vector(), 0);
		case PrimitiveType::TriangleFill: return std::make_unique<TriangleFillPrimitive>(-1, Vector(), Vector(), Vector(), 0);
		case PrimitiveType::Polygon: return std::make_unique<PolygonPrimitive>(-1, Vector(), 0, std::vector<Vector*>{});
		case PrimitiveType::PolygonFill: return std::make_unique<PolygonFillPrimitive>(-1, Vector(), 0, std::vector<Vector*>{});
		case PrimitiveType::Text: return std::make_unique<TextPrimitive>(-1, Vector(), "", false, 0, 0);
		case PrimitiveType::Bitmap: return std::make_unique<BitmapPrimitive>(-1, Vector(), static_cast<BITMAP*>(nullptr), 0, false, false);
		default: throw std::runtime_error("unsupported graphical primitive type");
	}
}

std::string GraphicalPrimitive::SaveCheckpoint() const { return SavePrimitiveList({this}, "PrimitiveValue1"); }

bool GraphicalPrimitive::LoadCheckpoint(std::string_view text) {
	try {
		auto values = LoadPrimitiveList(text, "PrimitiveValue1", false);
		if (values.size() != 1 || values.front()->GetPrimitiveType() != GetPrimitiveType()) return false;
		auto& source = *values.front();
		switch (GetPrimitiveType()) {
#define RESTORE_PRIMITIVE(name) case PrimitiveType::name: *static_cast<name##Primitive*>(this) = std::move(static_cast<name##Primitive&>(source)); break
			RESTORE_PRIMITIVE(Line); RESTORE_PRIMITIVE(Arc); RESTORE_PRIMITIVE(Spline); RESTORE_PRIMITIVE(Box); RESTORE_PRIMITIVE(BoxFill);
			RESTORE_PRIMITIVE(RoundedBox); RESTORE_PRIMITIVE(RoundedBoxFill); RESTORE_PRIMITIVE(Circle); RESTORE_PRIMITIVE(CircleFill);
			RESTORE_PRIMITIVE(Ellipse); RESTORE_PRIMITIVE(EllipseFill); RESTORE_PRIMITIVE(Triangle); RESTORE_PRIMITIVE(TriangleFill);
			RESTORE_PRIMITIVE(Polygon); RESTORE_PRIMITIVE(PolygonFill); RESTORE_PRIMITIVE(Text); RESTORE_PRIMITIVE(Bitmap);
#undef RESTORE_PRIMITIVE
			default: return false;
		}
		return true;
	} catch (const std::exception& error) { std::cerr << "[primitive-checkpoint] " << error.what() << std::endl; return false; }
}

bool GraphicalPrimitive::ResolveCheckpointReferences() {
	if (GetPrimitiveType() != PrimitiveType::Bitmap) return true;
	auto& primitive = static_cast<BitmapPrimitive&>(*this);
	if (primitive.m_PendingSpriteReference.empty()) return true;
	try {
		const Entity* entity = GUICheckpoint::LoadEntityReference(primitive.m_PendingSpriteReference);
		if (entity) {
			const auto* sprite = dynamic_cast<const MOSprite*>(entity);
			if (!sprite) return false;
			BITMAP* image = primitive.m_IconBitmap ? sprite->GetGraphicalIcon() : sprite->GetSpriteFrame(primitive.m_SpriteFrame);
			if (GUICheckpoint::SaveBitmap(image) != GUICheckpoint::SaveBitmap(primitive.m_Bitmap)) return false;
			primitive.m_SpriteOwner = sprite; primitive.m_Bitmap = image;
			primitive.m_ImageOwners = {sprite->ShareSpriteBitmap(image)};
		}
		primitive.m_PendingSpriteReference.clear();
		return true;
	} catch (const std::exception& error) { std::cerr << "[primitive-checkpoint] " << error.what() << std::endl; return false; }
}

std::string PrimitiveMan::SaveCheckpoint() const {
	std::lock_guard<std::mutex> lock(m_Mutex);
	std::vector<const GraphicalPrimitive*> primitives;
	for (const auto& value: m_ScheduledPrimitives) primitives.push_back(value.get());
	return SavePrimitiveList(primitives, "PrimitiveMan1");
}

bool PrimitiveMan::LoadCheckpoint(std::string_view text, bool validateOnly, bool resolveReferences) {
	try {
		if (!validateOnly && SaveCheckpoint() == text) return !resolveReferences || ResolveCheckpointReferences();
		auto values = LoadPrimitiveList(text, "PrimitiveMan1", validateOnly);
		if (validateOnly) return true;
		if (resolveReferences) for (auto& value: values) if (!value->ResolveCheckpointReferences()) return false;
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_ScheduledPrimitives.swap(values);
		return true;
	} catch (const std::exception& error) { std::cerr << "[primitive-checkpoint] " << error.what() << std::endl; return false; }
}

bool PrimitiveMan::ResolveCheckpointReferences() {
	std::lock_guard<std::mutex> lock(m_Mutex);
	for (auto& value: m_ScheduledPrimitives) if (!value->ResolveCheckpointReferences()) return false;
	return true;
}

void PrimitiveMan::SetAsideQueues(QueuesSetAside& output) {
	std::lock_guard<std::mutex> lock(m_Mutex);
	output.primitives.swap(m_ScheduledPrimitives);
}

void PrimitiveMan::ReinstateQueues(QueuesSetAside& input) {
	std::lock_guard<std::mutex> lock(m_Mutex);
	m_ScheduledPrimitives.swap(input.primitives);
}

GraphicalPrimitive* PrimitiveMan::GetCheckpointPrimitive(size_t index) const {
	std::lock_guard<std::mutex> lock(m_Mutex);
	return index < m_ScheduledPrimitives.size() ? m_ScheduledPrimitives[index].get() : nullptr;
}

Vector* PrimitiveMan::GetCheckpointVertex(size_t index) const {
	std::lock_guard<std::mutex> lock(m_Mutex);
	const auto vertices = PrimitiveVertices(m_ScheduledPrimitives);
	return index < vertices.size() ? vertices[index] : nullptr;
}

int PrimitiveMan::FindCheckpointPrimitive(const void* primitive) const {
	std::lock_guard<std::mutex> lock(m_Mutex);
	for (size_t index = 0; index < m_ScheduledPrimitives.size(); ++index) if (m_ScheduledPrimitives[index].get() == primitive) return index;
	return -1;
}

int PrimitiveMan::FindCheckpointVertex(const void* vertex) const {
	std::lock_guard<std::mutex> lock(m_Mutex);
	const auto vertices = PrimitiveVertices(m_ScheduledPrimitives);
	const auto found = std::find(vertices.begin(), vertices.end(), vertex);
	return found == vertices.end() ? -1 : static_cast<int>(std::distance(vertices.begin(), found));
}

bool PrimitiveMan::RunCheckpointSelfTest() {
	QueuesSetAside original;
	SetAsideQueues(original);
	struct Restore { PrimitiveMan& manager; QueuesSetAside& original; ~Restore() { manager.ClearPrimitivesQueue(); manager.ReinstateQueues(original); } } restore{*this, original};
	bool passed = true;
	const auto check = [&](const std::string& name, bool valid) { passed &= valid; std::cout << "[primitive-checkpoint-selftest] " << (valid ? "PASS " : "FAIL ") << name << std::endl; };
	try {
		std::vector<std::string> records;
		for (int index = 1; index <= static_cast<int>(PrimitiveType::Bitmap); ++index) {
			auto primitive = GraphicalPrimitive::CreateCheckpointType(static_cast<PrimitiveType>(index));
			primitive->m_StartPos = Vector(index + 0.25F, -index - 0.75F); primitive->m_EndPos = Vector(index * 3, index * 4);
			primitive->m_Player = index % 4; primitive->m_Color = index * 7; primitive->m_Depth = index * 0.125F;
			primitive->m_BlendMode = DrawBlendMode::BlendTransparency; primitive->m_ColorChannelBlendAmounts = {11 + index, 22 + index, 33 + index, 44 + index};
			if (index == static_cast<int>(PrimitiveType::Line)) static_cast<LinePrimitive&>(*primitive).m_Thickness = 3.75F;
			if (index == static_cast<int>(PrimitiveType::Arc)) { auto& value = static_cast<ArcPrimitive&>(*primitive); value.m_StartAngle = 0.7F; value.m_EndAngle = 2.25F; value.m_Radius = 23; value.m_Thickness = 4; }
			if (index == static_cast<int>(PrimitiveType::Spline)) { auto& value = static_cast<SplinePrimitive&>(*primitive); value.m_GuidePointAPos = Vector(18, 21); value.m_GuidePointBPos = Vector(32, 46); }
			if (index == static_cast<int>(PrimitiveType::RoundedBox)) static_cast<RoundedBoxPrimitive&>(*primitive).m_CornerRadius = 5;
			if (index == static_cast<int>(PrimitiveType::RoundedBoxFill)) static_cast<RoundedBoxFillPrimitive&>(*primitive).m_CornerRadius = 9;
			if (index == static_cast<int>(PrimitiveType::Circle)) static_cast<CirclePrimitive&>(*primitive).m_Radius = 33;
			if (index == static_cast<int>(PrimitiveType::CircleFill)) static_cast<CircleFillPrimitive&>(*primitive).m_Radius = 37;
			if (index == static_cast<int>(PrimitiveType::Ellipse)) { auto& value = static_cast<EllipsePrimitive&>(*primitive); value.m_HorizRadius = 18; value.m_VertRadius = 29; }
			if (index == static_cast<int>(PrimitiveType::EllipseFill)) { auto& value = static_cast<EllipseFillPrimitive&>(*primitive); value.m_HorizRadius = 21; value.m_VertRadius = 31; }
			if (index == static_cast<int>(PrimitiveType::Triangle)) { auto& value = static_cast<TrianglePrimitive&>(*primitive); value.m_PointAPos = Vector(3, 7); value.m_PointBPos = Vector(31, 12); value.m_PointCPos = Vector(11, 42); }
			if (index == static_cast<int>(PrimitiveType::TriangleFill)) { auto& value = static_cast<TriangleFillPrimitive&>(*primitive); value.m_PointAPos = Vector(4, 8); value.m_PointBPos = Vector(32, 13); value.m_PointCPos = Vector(12, 43); }
			if (index == static_cast<int>(PrimitiveType::Text)) { auto& value = static_cast<TextPrimitive&>(*primitive); value.m_Text = "checkpoint 42"; value.m_IsSmall = true; value.m_Alignment = 1; value.m_RotAngle = 0.375F; value.CreateTextBitmap(); }
			if (index == static_cast<int>(PrimitiveType::Bitmap)) { auto& value = static_cast<BitmapPrimitive&>(*primitive); value.m_Bitmap = ContentFile("Base.rte/GUIs/Skins/Cursor.png").GetAsBitmap(); value.m_RotAngle = -0.25F; value.m_Scale = 1.75F; value.m_HFlipped = true; value.m_VFlipped = true; }
			records.push_back(primitive->SaveCheckpoint()); SchedulePrimitive(std::move(primitive));
		}
		Vector* shared = new Vector(19.25F, -31.5F);
		SchedulePrimitive(std::make_unique<PolygonPrimitive>(2, Vector(3, 4), 17, std::vector<Vector*>{shared, new Vector(5, 7), shared}));
		SchedulePrimitive(std::make_unique<PolygonFillPrimitive>(3, Vector(8, 9), 23, std::vector<Vector*>{shared, new Vector(11, 13)}));
		const std::string checkpoint = SaveCheckpoint();
		QueuesSetAside held; SetAsideQueues(held);
		check("empty_after_set_aside", GetCheckpointPrimitive(0) == nullptr);
		check("fresh_queue_restore", LoadCheckpoint(checkpoint));
		for (size_t index = 0; index < records.size(); ++index) check(GraphicalPrimitive::CheckpointTypeName(GetCheckpointPrimitive(index)->GetPrimitiveType()), GetCheckpointPrimitive(index)->SaveCheckpoint() == records[index]);
		auto* polygon = static_cast<PolygonPrimitive*>(GetCheckpointPrimitive(17));
		auto* fill = static_cast<PolygonFillPrimitive*>(GetCheckpointPrimitive(18));
		check("shared_vertices_and_order", polygon->m_Vertices[0] == polygon->m_Vertices[2] && polygon->m_Vertices[0] == fill->m_Vertices[0] && polygon->m_Vertices[0]->m_X == 19.25F);
		check("canonical_queue", SaveCheckpoint() == checkpoint);
		check("malformed_atomic", !LoadCheckpoint(checkpoint + "x") && SaveCheckpoint() == checkpoint);
		check("truncated_atomic", !LoadCheckpoint(checkpoint.substr(0, checkpoint.size() / 2)) && SaveCheckpoint() == checkpoint);
		auto* same = GetCheckpointPrimitive(0);
		check("repeat_preserves_adopted_identity", LoadCheckpoint(checkpoint) && same == GetCheckpointPrimitive(0));
		ClearPrimitivesQueue(); ReinstateQueues(held);
		check("held_vertices_preserve_identity", GetCheckpointVertex(0) == shared && shared->m_Y == -31.5F && SaveCheckpoint() == checkpoint);
	} catch (const std::exception& error) { check("exception", false); std::cerr << "[primitive-checkpoint-selftest] " << error.what() << std::endl; }
	return passed;
}

void PrimitiveMan::DrawPrimitives(int player, BITMAP* targetBitmap, const Vector& targetPos) const {
	ZoneScoped;

	if (m_ScheduledPrimitives.empty()) {
		return;
	}

	int lastDrawMode = DRAW_MODE_SOLID;
	DrawBlendMode lastBlendMode = DrawBlendMode::NoBlend;
	std::array<int, 4> lastBlendAmounts = {BlendAmountLimits::MinBlend, BlendAmountLimits::MinBlend, BlendAmountLimits::MinBlend, BlendAmountLimits::MinBlend};
	GLint currentShader = rlGetShaderCurrent();
	rlDrawRenderBatchActive();
	if (GLAD_GL_KHR_blend_equation_advanced_coherent){
		glBlendBarrierKHR();
		rlEnableAdvancedColorBlend();
	}
	const Shader* background = dynamic_cast<const Shader*>(g_PresetMan.GetEntityPreset("Shader", "Background"));
	rlEnableColorBlend();
	for (const std::unique_ptr<GraphicalPrimitive>& primitive: m_ScheduledPrimitives) {
		if (int playerToDrawFor = primitive->m_Player; playerToDrawFor == player || playerToDrawFor == -1) {
			rlDrawRenderBatchActive();
			if (DrawBlendMode blendMode = primitive->m_BlendMode; blendMode > DrawBlendMode::NoBlend) {
				if (const std::array<int, 4>& blendAmounts = primitive->m_ColorChannelBlendAmounts; blendMode != lastBlendMode || blendAmounts != lastBlendAmounts) {
					if (lastBlendMode == BlendDissolve) {
						background->Begin();
					}
					rlEnableShader(rlGetShaderCurrent());
					g_FrameMan.SetBlendMode(blendMode);
					GLint colorUniform = glGetUniformLocation(rlGetShaderCurrent(), "rteColor");
					glUniform4f(colorUniform, blendAmounts[0] / static_cast<float>(MaxBlend), blendAmounts[1] / static_cast<float>(MaxBlend), blendAmounts[2] / static_cast<float>(MaxBlend), blendAmounts[3] / static_cast<float>(MaxBlend));
					lastBlendMode = blendMode;
					lastBlendAmounts = blendAmounts;
				}
			} else {
				g_FrameMan.SetBlendMode(BlendTransparency);
				rlEnableShader(currentShader);
				GLint colorUniform = glGetUniformLocation(rlGetShaderCurrent(), "rteColor");
				glUniform4f(colorUniform, 1.0f, 1.0f, 1.0f, 1.0f);
				lastBlendMode = DrawBlendMode::NoBlend;
			}
			if (GLAD_GL_KHR_blend_equation_advanced_coherent) {
				glBlendBarrierKHR();
			}
			rlZDepth(primitive->m_Depth);
			primitive->DrawTiled(targetBitmap, targetPos);
		}
	}
	rlDrawRenderBatchActive();
	if (GLAD_GL_KHR_blend_equation_advanced_coherent) {
		rlDisableAdvancedColorBlend();
	}
	rlSetBlendMode(RL_BLEND_ALPHA);
	background->Begin();
	GLint colorUniform = glGetUniformLocation(rlGetShaderCurrent(), "rteColor");
	glUniform4f(colorUniform, 1.0f, 1.0f, 1.0f, 1.0f);
	rlZDepth(c_DefaultDrawDepth);
	drawing_mode(DRAW_MODE_SOLID, nullptr, 0, 0);
}
