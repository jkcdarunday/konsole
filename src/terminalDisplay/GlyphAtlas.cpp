/*
    SPDX-FileCopyrightText: 2024 Konsole Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "GlyphAtlas.h"

#if HAVE_OPENGL

#include <QDebug>
#include <QImage>
#include <QOpenGLContext>

namespace Konsole
{

GlyphAtlas::GlyphAtlas() = default;

GlyphAtlas::~GlyphAtlas()
{
    destroy();
}

void GlyphAtlas::setFonts(const QRawFont &regular, const QRawFont &bold, const QRawFont &italic, const QRawFont &boldItalic)
{
    m_regular = regular;
    m_bold = bold;
    m_italic = italic;
    m_boldItalic = boldItalic;
    invalidate();
}

void GlyphAtlas::initialize()
{
    if (m_initialized) {
        return;
    }

    initializeOpenGLFunctions();

    glGenTextures(1, &m_textureId);
    glBindTexture(GL_TEXTURE_2D, m_textureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Allocate atlas storage as a single-channel (red) texture
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_SIZE, ATLAS_SIZE, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    m_initialized = true;
}

void GlyphAtlas::destroy()
{
    if (!m_initialized) {
        return;
    }
    if (m_textureId != 0) {
        glDeleteTextures(1, &m_textureId);
        m_textureId = 0;
    }
    m_cache.clear();
    m_cursorX = GLYPH_PADDING;
    m_cursorY = GLYPH_PADDING;
    m_shelfH = 0;
    m_initialized = false;
}

void GlyphAtlas::invalidate()
{
    m_cache.clear();
    m_cursorX = GLYPH_PADDING;
    m_cursorY = GLYPH_PADDING;
    m_shelfH = 0;

    if (m_initialized && m_textureId != 0) {
        // Clear the texture so stale glyph data is not shown
        QByteArray zeros(ATLAS_SIZE * ATLAS_SIZE, 0);
        glBindTexture(GL_TEXTURE_2D, m_textureId);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ATLAS_SIZE, ATLAS_SIZE, GL_RED, GL_UNSIGNED_BYTE, zeros.constData());
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

GlyphInfo GlyphAtlas::getGlyph(const QString &text, bool bold, bool italic)
{
    if (!m_initialized) {
        return {};
    }

    const QRawFont &font = (bold && italic) ? m_boldItalic : bold ? m_bold : italic ? m_italic : m_regular;
    if (!font.isValid()) {
        return {};
    }

    // Map the first character of text to a glyph index
    const QVector<quint32> indexes = font.glyphIndexesForString(text);
    if (indexes.isEmpty()) {
        return {};
    }
    const quint32 glyphIndex = indexes.first();

    GlyphKey key{glyphIndex, bold, italic};

    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        return it.value();
    }

    return uploadGlyph(key, glyphIndex, font);
}

GlyphInfo GlyphAtlas::uploadGlyph(const GlyphKey &key, quint32 glyphIndex, const QRawFont &font)
{
    // Rasterise the glyph to a greyscale alpha map
    const QImage glyphImage = font.alphaMapForGlyph(glyphIndex, QRawFont::PixelAntialiasing);

    if (glyphImage.isNull()) {
        GlyphInfo invalid;
        m_cache.insert(key, invalid);
        return invalid;
    }

    // Ensure format is Indexed8 (single byte per pixel)
    const QImage alphaImage = glyphImage.format() == QImage::Format_Indexed8
        ? glyphImage
        : glyphImage.convertToFormat(QImage::Format_Indexed8);

    const int gw = alphaImage.width();
    const int gh = alphaImage.height();

    if (gw == 0 || gh == 0) {
        GlyphInfo invalid;
        m_cache.insert(key, invalid);
        return invalid;
    }

    // Advance to next shelf if this glyph doesn't fit on the current one
    if (m_cursorX + gw + GLYPH_PADDING > ATLAS_SIZE) {
        m_cursorY += m_shelfH + GLYPH_PADDING;
        m_cursorX = GLYPH_PADDING;
        m_shelfH = 0;
    }

    // If we've run out of vertical space, clear the atlas and start over
    if (m_cursorY + gh + GLYPH_PADDING > ATLAS_SIZE) {
        invalidate();
        // After invalidate, re-start this glyph placement
        m_cursorX = GLYPH_PADDING;
        m_cursorY = GLYPH_PADDING;
        m_shelfH = 0;
    }

    // Collect scanlines into a contiguous byte buffer
    QByteArray pixelData;
    pixelData.resize(gw * gh);
    for (int row = 0; row < gh; ++row) {
        const uchar *src = alphaImage.constScanLine(row);
        memcpy(pixelData.data() + row * gw, src, gw);
    }

    // Upload the glyph bitmap to the atlas texture
    glBindTexture(GL_TEXTURE_2D, m_textureId);
    // Ensure correct alignment for sub-image uploads of non-power-of-2 widths
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, m_cursorX, m_cursorY, gw, gh, GL_RED, GL_UNSIGNED_BYTE, pixelData.constData());
    glBindTexture(GL_TEXTURE_2D, 0);

    // Compute the bearing (offset from the cell top-left to the glyph top-left)
    // QRawFont uses screen coordinates: y increases downward, baseline at y=0.
    // boundingRect.top() is negative for glyphs that sit above the baseline (most chars).
    // bearingX/Y are relative to the glyph origin (baseline, left edge).
    const QRectF boundingRect = font.boundingRect(glyphIndex);
    const int bearingX = static_cast<int>(boundingRect.left());
    // Store the raw top value; the caller adds fontAscent to get screen-space offY.
    const int bearingY = static_cast<int>(boundingRect.top());

    GlyphInfo info;
    info.u0 = static_cast<float>(m_cursorX) / ATLAS_SIZE;
    info.v0 = static_cast<float>(m_cursorY) / ATLAS_SIZE;
    info.u1 = static_cast<float>(m_cursorX + gw) / ATLAS_SIZE;
    info.v1 = static_cast<float>(m_cursorY + gh) / ATLAS_SIZE;
    info.bearingX = bearingX;
    info.bearingY = bearingY;
    info.width = gw;
    info.height = gh;
    info.valid = true;

    m_cache.insert(key, info);

    // Advance shelf cursor
    m_cursorX += gw + GLYPH_PADDING;
    if (gh > m_shelfH) {
        m_shelfH = gh;
    }

    return info;
}

} // namespace Konsole

#endif // HAVE_OPENGL
