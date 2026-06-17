/*
    SPDX-FileCopyrightText: 2024 Konsole Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "config-konsole.h"

#if HAVE_OPENGL

#include <QFont>
#include <QHash>
#include <QImage>
#include <QOpenGLFunctions>
#include <QRawFont>
#include <QString>

namespace Konsole
{

/**
 * UV rectangle in the glyph atlas texture (values in [0,1]).
 * Also stores the bearing (pixel offset of the glyph top-left corner
 * relative to the cell origin) and the actual pixel dimensions of the
 * rasterised glyph bitmap.
 */
struct GlyphInfo {
    /** Texture coordinates of the glyph in the atlas. */
    float u0 = 0.0f, v0 = 0.0f;
    float u1 = 0.0f, v1 = 0.0f;

    /** Horizontal bearing: pixels from cell left edge to glyph left edge. */
    int bearingX = 0;
    /** Vertical bearing: pixels from cell top edge to glyph top edge. */
    int bearingY = 0;

    /** Pixel width/height of the rasterised glyph bitmap. */
    int width = 0;
    int height = 0;

    bool valid = false;
};

/**
 * Key used to look up a cached glyph.  Combines the glyph index with the
 * font variant flags so that e.g. bold 'A' and regular 'A' occupy separate
 * atlas slots.
 */
struct GlyphKey {
    quint32 glyphIndex = 0;
    bool bold = false;
    bool italic = false;

    bool operator==(const GlyphKey &o) const
    {
        return glyphIndex == o.glyphIndex && bold == o.bold && italic == o.italic;
    }
};

inline size_t qHash(const GlyphKey &key, size_t seed = 0)
{
    return ::qHash(key.glyphIndex ^ (key.bold ? 0x10000u : 0u) ^ (key.italic ? 0x20000u : 0u), seed);
}

/**
 * @class GlyphAtlas
 * @brief Manages an OpenGL texture atlas that caches rasterised terminal
 *        glyphs.
 *
 * The atlas is a single square R8 texture (one byte per texel used as an
 * alpha mask).  Glyphs are rasterised via @c QRawFont::alphaMapForGlyph()
 * and packed using a simple shelf algorithm.  When the atlas is full it is
 * cleared and re-populated from scratch (glyphs are re-uploaded on demand).
 *
 * Usage:
 * @code
 *   atlas.setFonts(regular, bold, italic, boldItalic);
 *   atlas.initialize();          // call once in initializeGL()
 *   GlyphInfo gi = atlas.getGlyph("A", false, false);
 *   // use gi.u0 .. gi.v1 as texture coordinates
 * @endcode
 */
class GlyphAtlas : protected QOpenGLFunctions
{
public:
    /** Side length (in texels) of the square atlas texture. */
    static constexpr int ATLAS_SIZE = 2048;

    /** 1-texel padding between packed glyphs to avoid bleeding artefacts. */
    static constexpr int GLYPH_PADDING = 1;

    explicit GlyphAtlas();
    ~GlyphAtlas();

    /**
     * Provide the four font variants used for glyph rasterisation.
     * This must be called before @c initialize() or @c getGlyph().
     */
    void setFonts(const QRawFont &regular, const QRawFont &bold, const QRawFont &italic, const QRawFont &boldItalic);

    /** Allocate the OpenGL texture.  Must be called with an active GL context. */
    void initialize();

    /** Delete the OpenGL texture and clear all cached glyph data. */
    void destroy();

    /**
     * Look up (or rasterise and cache) the first glyph of @p text.
     * Returns an invalid GlyphInfo if the glyph could not be rasterised.
     */
    GlyphInfo getGlyph(const QString &text, bool bold, bool italic);

    /** OpenGL texture object name. */
    GLuint textureId() const
    {
        return m_textureId;
    }

    /**
     * Discard all cached glyphs and reset the shelf state.
     * The GL texture is cleared but NOT deleted; it can be re-used
     * immediately after this call.
     */
    void invalidate();

private:
    /** Rasterise the glyph and upload it into the atlas. */
    GlyphInfo uploadGlyph(const GlyphKey &key, quint32 glyphIndex, const QRawFont &font);

    QRawFont m_regular;
    QRawFont m_bold;
    QRawFont m_italic;
    QRawFont m_boldItalic;

    GLuint m_textureId = 0;
    QHash<GlyphKey, GlyphInfo> m_cache;

    // Shelf-packing state
    int m_cursorX = GLYPH_PADDING; ///< Next free X position on the current shelf
    int m_cursorY = GLYPH_PADDING; ///< Y origin of the current shelf
    int m_shelfH = 0; ///< Height of the tallest glyph on the current shelf

    bool m_initialized = false;
};

} // namespace Konsole

#endif // HAVE_OPENGL
