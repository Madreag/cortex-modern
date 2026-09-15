#pragma once

#include <map>

namespace RTE {

	/// A class to handle the drawing of text.
	class GUIFont {
		friend class GUICheckpoint;
		friend class FrameMan;

	public:
		// Horizontal Text Alignment,
		enum {
			Left = 0,
			Centre,
			Right
		} HAlignment;

		// Vertical Text Alignment,
		enum {
			Top = 0,
			Middle,
			Bottom
		} VAlignment;

		// Character structure
		typedef struct {
			int m_Width;
			int m_Height;
			int m_Offset;
		} Character;

		// Font Color structure
		typedef struct {
			unsigned long m_Color;
			GUIBitmap* m_Bitmap;
		} FontColor;

		/// Constructor method used to instantiate a GUIFont object in system
		/// memory.
		explicit GUIFont(const std::string& Name);

		/// Loads the font from an image file.
		/// @param Screen Screen class, Filename of image.
		bool Load(GUIScreen* Screen, const std::string& Filename);

		/// Pre-Calculates the font using a specific color.
		/// @param Color Color.
		void CacheColor(unsigned long Color);

		/// Finds a font color structure from the cache.
		/// @param Color Color.
		FontColor* GetFontColor(unsigned long Color);

		/// A font bitmap where every drawable pixel is recoloured to Color, cached per color.
		/// For atlases whose antialias ink never matches m_MainColor, so CacheColor leaves them unchanged.
		GUIBitmap* InkColorBitmap(unsigned long Color);
		unsigned long InkOf(GUIBitmap* Bitmap);

		/// Draws text to a bitmap.
		/// @param Bitmap Bitmap, Position, Text, Color, Drop-shadow, 0 = none.
		/// @param GlyphFallback Optional font whose atlas supplies the glyph for any byte this font's cell has no ink for.
		void Draw(GUIBitmap* Bitmap, int X, int Y, const std::string& Text, unsigned long Shadow = 0, GUIFont* GlyphFallback = nullptr);

		/// Draws text to a bitmap aligned.
		/// @param Bitmap Bitmap, Position, Text.
		void DrawAligned(GUIBitmap* Bitmap, int X, int Y, const std::string& Text, int HAlign, int VAlign = Top, int maxWidth = 0, unsigned long Shadow = 0, GUIFont* GlyphFallback = nullptr);

		/// Sets the current color.
		/// @param Color Color.
		void SetColor(unsigned long Color);

		/// Calculates the width of a piece of text.
		/// @param Text Text.
		int CalculateWidth(const std::string& Text, GUIFont* GlyphFallback = nullptr);

		/// Calculates the width of a piece of text.
		/// @param Character Character.
		int CalculateWidth(const char Character);

		/// Calculates the height of a piece of text, if it's wrapped within a
		/// max width.
		/// @param Text Text, and the max width. If 0, no wrapping is done.
		int CalculateHeight(const std::string& Text, int MaxWidth = 0, GUIFont* GlyphFallback = nullptr);

		/// Gets the font height.
		int GetFontHeight() const;

		/// Gets the name of the font
		std::string GetName() const;

		/// The atlas ink ConvertColor already mapped for this font.
		unsigned long GetMainColor() const { return m_MainColor; }

		/// Destroys the font data
		void Destroy();

		/// Get the character kerning (spacing)
		int GetKerning() const;

		/// Set the character kerning (spacing), in pixels. 1 = one empty pixel
		/// between chars, 0 = chars are touching.
		void SetKerning(int newKerning = 1) { m_Kerning = newKerning; }

		/// Returns whether the font's cell for the character actually contains any drawable pixels.
		/// Some atlas cells carry width but no ink, so width alone cannot tell a real glyph from a blank one.
		/// @param Character Character index.
		bool HasGlyphPixels(unsigned char Character) const { return m_GlyphCovered[Character]; }

	private:
		GUIBitmap* m_Font;
		GUIScreen* m_Screen;
		std::vector<FontColor> m_ColorCache;
		std::vector<FontColor> m_InkColorCache;
		std::map<GUIBitmap*, unsigned long> m_InkOfBitmap;

		int m_FontHeight;
		unsigned long m_MainColor;
		unsigned long m_CurrentColor;
		GUIBitmap* m_CurrentBitmap;
		std::string m_Name;
		Character m_Characters[256];
		bool m_GlyphCovered[256]; // Whether the character's atlas cell holds any drawable pixels, scanned at Load

		/// Returns the font that can draw this byte: the fallback when it covers the byte and either this
		/// font's cell has no ink or the byte is >= 0x80 (the large atlas keeps HUD icons there, not letters).
		/// The caller passes the fallback through the draw/measure APIs.
		GUIFont* GlyphFontFor(unsigned char Character, GUIFont* GlyphFallback);

		int m_CharIndexCap; // The highest index of valid characters that was read in from the file

		int m_Kerning; // Spacing between characters
		int m_Leading; // Spacing between lines
	};
} // namespace RTE
