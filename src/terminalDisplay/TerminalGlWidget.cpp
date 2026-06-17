/*
    SPDX-FileCopyrightText: 2024 Konsole Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TerminalGlWidget.h"

#if HAVE_OPENGL

#include "TerminalFonts.h"
#include "../characters/CharacterColor.h"

#include <QDebug>
#include <QRawFont>
#include <QScreen>
#include <cstddef>

namespace Konsole
{

// ---------------------------------------------------------------------------
// GLSL shader sources
// ---------------------------------------------------------------------------

// Background pass: one coloured quad per cell that has a non-default
// background colour.
static const char *BG_VERT = R"(
#version 330 core

// Per-vertex: corners of a unit quad [0,1]x[0,1]
layout(location = 0) in vec2 quadCorner;

// Per-instance
layout(location = 1) in vec2  cellPos;  // (col, row)
layout(location = 2) in vec4  bgColor;  // RGBA

out vec4 vColor;

uniform vec2 viewSize;   // viewport in pixels
uniform vec2 cellSize;   // cell width/height in pixels
uniform vec2 offset;     // terminal content offset in pixels

void main()
{
    vec2 px = offset + cellPos * cellSize + quadCorner * cellSize;
    // Convert to Normalised Device Coordinates (y flipped for screen space)
    vec2 ndc = px / viewSize * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vColor = bgColor;
}
)";

static const char *BG_FRAG = R"(
#version 330 core
in  vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)";

// Glyph pass: one textured quad per visible glyph, sampling from the atlas.
static const char *GLYPH_VERT = R"(
#version 330 core

// Per-vertex: corners of a unit quad [0,1]x[0,1]
layout(location = 0) in vec2 quadCorner;

// Per-instance
layout(location = 1) in vec2  cellPos;     // (col, row)
layout(location = 2) in vec4  fgColor;     // RGBA
layout(location = 3) in vec4  atlasUV;     // (u0, v0, u1, v1)
layout(location = 4) in vec2  glyphOffset; // bearing offset in px
layout(location = 5) in vec2  glyphSize;   // bitmap size in px

out vec4  vFgColor;
out vec2  vTexCoord;

uniform vec2 viewSize;
uniform vec2 cellSize;
uniform vec2 offset;

void main()
{
    vec2 px = offset + cellPos * cellSize + glyphOffset + quadCorner * glyphSize;
    vec2 ndc = px / viewSize * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);

    vec2 uv0 = atlasUV.xy;
    vec2 uv1 = atlasUV.zw;
    vTexCoord = uv0 + quadCorner * (uv1 - uv0);
    vFgColor  = fgColor;
}
)";

static const char *GLYPH_FRAG = R"(
#version 330 core
in  vec4      vFgColor;
in  vec2      vTexCoord;
out vec4      fragColor;

uniform sampler2D glyphAtlas;

void main()
{
    float alpha = texture(glyphAtlas, vTexCoord).r;
    fragColor = vec4(vFgColor.rgb, vFgColor.a * alpha);
}
)";

// ---------------------------------------------------------------------------
// Unit-quad vertex data (two triangles, CCW)
// ---------------------------------------------------------------------------
// clang-format off
static const float QUAD_VERTS[] = {
    0.0f, 0.0f,
    1.0f, 0.0f,
    1.0f, 1.0f,
    0.0f, 0.0f,
    1.0f, 1.0f,
    0.0f, 1.0f,
};
// clang-format on

// ---------------------------------------------------------------------------

TerminalGlWidget::TerminalGlWidget(QWidget *parent)
    : QOpenGLWidget(parent)
    , m_quadVbo(QOpenGLBuffer::VertexBuffer)
    , m_bgInstanceVbo(QOpenGLBuffer::VertexBuffer)
    , m_glyphInstanceVbo(QOpenGLBuffer::VertexBuffer)
{
    // Allow alpha blending for transparent glyph rendering
    setAttribute(Qt::WA_AlwaysStackOnTop);

    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    // Synchronise buffer swaps with the display's vertical blank so that
    // rendering is capped at the monitor's native refresh rate.
    fmt.setSwapInterval(1);
    setFormat(fmt);

    // Drive rendering at the screen's native refresh rate.  We use a
    // PreciseTimer so the interval is as close to the screen period as the
    // platform allows.  The timer only schedules an update when new data is
    // actually available (m_dataDirty), so it is cheap when the terminal is
    // idle.
    m_renderTimer = new QTimer(this);
    m_renderTimer->setTimerType(Qt::PreciseTimer);
    connect(m_renderTimer, &QTimer::timeout, this, [this]() {
        if (m_dataDirty) {
            update();
        }
    });

    // Re-calculate the timer interval whenever the widget moves to a
    // different screen (e.g. dragging to a 120 Hz monitor from a 60 Hz one).
    connect(this, &QWidget::screenChanged, this, &TerminalGlWidget::updateRenderTimerForScreen);
}

TerminalGlWidget::~TerminalGlWidget()
{
    // Make the context current before destroying GL objects
    makeCurrent();
    m_bgVao.destroy();
    m_glyphVao.destroy();
    m_quadVbo.destroy();
    m_bgInstanceVbo.destroy();
    m_glyphInstanceVbo.destroy();
    m_atlas.destroy();
    doneCurrent();
}

// ---------------------------------------------------------------------------
// QOpenGLWidget overrides
// ---------------------------------------------------------------------------

void TerminalGlWidget::initializeGL()
{
    initializeOpenGLFunctions();

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_atlas.initialize();

    if (!buildShaders()) {
        qWarning() << "TerminalGlWidget: shader compilation failed — GPU rendering disabled";
        return;
    }

    // Upload the shared unit-quad geometry
    m_quadVbo.create();
    m_quadVbo.bind();
    m_quadVbo.allocate(QUAD_VERTS, sizeof(QUAD_VERTS));
    m_quadVbo.release();

    // Create instance VBOs (data uploaded each frame)
    m_bgInstanceVbo.create();
    m_glyphInstanceVbo.create();

    setupVaos();

    // Start the screen-rate render timer now that we have a valid GL context.
    // screen() is guaranteed to be non-null once the widget has been realised.
    updateRenderTimerForScreen(screen());
}

void TerminalGlWidget::resizeGL(int /*w*/, int /*h*/)
{
    // Viewport is managed automatically by QOpenGLWidget
}

void TerminalGlWidget::paintGL()
{
    // Use the physical (device) pixel dimensions for the viewport so that
    // rendering fills the entire framebuffer on HiDPI displays.  On a 150 %
    // scaled monitor (DPR = 1.5) the framebuffer is 1.5× wider and taller
    // than width()/height() (which return logical pixels); using logical
    // dimensions here would fill only ~44 % of the framebuffer (1/1.5²) and
    // make the terminal appear tiny.
    const qreal dpr = devicePixelRatioF();
    const int vpW = qRound(width() * dpr);
    const int vpH = qRound(height() * dpr);

    glViewport(0, 0, vpW, vpH);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Mark this frame as consumed regardless of whether we draw anything so
    // that the render timer does not keep re-scheduling empty paints.
    m_dataDirty = false;

    if (!m_dataReady || m_bgInstances.isEmpty()) {
        return;
    }

    uploadInstanceData();

    // Shader uniforms are in logical pixels.  The NDC calculation
    //   ndc = px / viewSize * 2 - 1
    // is device-independent: a point at (logicalW, logicalH) maps to NDC
    // (1, 1) which glViewport then stretches to physical (vpW, vpH).
    const float fw = static_cast<float>(m_fontWidth);
    const float fh = static_cast<float>(m_fontHeight);
    const float ox = static_cast<float>(m_contentOffset.x());
    const float oy = static_cast<float>(m_contentOffset.y());
    const float vw = static_cast<float>(width());
    const float vh = static_cast<float>(height());

    // ------------------------------------------------------------------
    // Background pass
    // ------------------------------------------------------------------
    if (!m_bgInstances.isEmpty()) {
        m_bgProgram.bind();
        m_bgProgram.setUniformValue("viewSize", QVector2D(vw, vh));
        m_bgProgram.setUniformValue("cellSize", QVector2D(fw, fh));
        m_bgProgram.setUniformValue("offset", QVector2D(ox, oy));

        m_bgVao.bind();
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, m_bgInstances.size());
        m_bgVao.release();
        m_bgProgram.release();
    }

    // ------------------------------------------------------------------
    // Glyph pass
    // ------------------------------------------------------------------
    if (!m_glyphInstances.isEmpty()) {
        m_glyphProgram.bind();
        m_glyphProgram.setUniformValue("viewSize", QVector2D(vw, vh));
        m_glyphProgram.setUniformValue("cellSize", QVector2D(fw, fh));
        m_glyphProgram.setUniformValue("offset", QVector2D(ox, oy));
        m_glyphProgram.setUniformValue("glyphAtlas", 0);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_atlas.textureId());

        m_glyphVao.bind();
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, m_glyphInstances.size());
        m_glyphVao.release();

        glBindTexture(GL_TEXTURE_2D, 0);
        m_glyphProgram.release();
    }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void TerminalGlWidget::updateDisplayData(const Character *image,
                                         int imageSize,
                                         int columns,
                                         int lines,
                                         const QColor *colorTable,
                                         TerminalFont *font,
                                         const QRect &contentRect)
{
    if (!image || imageSize <= 0 || columns <= 0 || lines <= 0 || !colorTable || !font) {
        return;
    }

    m_columns = columns;
    m_lines = lines;
    m_fontWidth = font->fontWidth();
    m_fontHeight = font->fontHeight();
    m_fontAscent = font->fontAscent();
    m_contentOffset = contentRect.topLeft();

    // Rebuild the font objects used by the atlas only when the font changes
    // to avoid invalidating the glyph cache on every update.
    const QFont baseFont = font->getVTFont();
    if (baseFont != m_lastFont) {
        m_lastFont = baseFont;
        QFont boldFont = baseFont;
        boldFont.setBold(true);
        QFont italicFont = baseFont;
        italicFont.setItalic(true);
        QFont boldItalicFont = baseFont;
        boldItalicFont.setBold(true);
        boldItalicFont.setItalic(true);

        m_atlas.setFonts(QRawFont::fromFont(baseFont),
                         QRawFont::fromFont(boldFont),
                         QRawFont::fromFont(italicFont),
                         QRawFont::fromFont(boldItalicFont));
    }

    m_bgInstances.clear();
    m_glyphInstances.clear();

    m_bgInstances.reserve(columns * lines);
    m_glyphInstances.reserve(columns * lines);

    // Iterate over cells; clamp to the actual image size as a safety guard.
    const int total = qMin(imageSize, columns * lines);
    for (int i = 0; i < total; ++i) {
        const int row = i / columns;
        const int col = i % columns;
        const Character &ch = image[i];

        // Background quad
        {
            const QColor bg = ch.backgroundColor.color(colorTable);
            BgInstance bi;
            bi.col = static_cast<float>(col);
            bi.row = static_cast<float>(row);
            bi.r = static_cast<float>(bg.redF());
            bi.g = static_cast<float>(bg.greenF());
            bi.b = static_cast<float>(bg.blueF());
            bi.a = static_cast<float>(bg.alphaF());
            m_bgInstances.append(bi);
        }

        // Glyph quad (skip space characters and the right half of
        // double-width pairs to avoid rendering them twice)
        if (ch.character != 0 && ch.character != ' ' && !ch.isRightHalfOfDoubleWide()) {
            if (ch.rendition.f.conceal) {
                continue;
            }

            const QString text = QString(QChar(static_cast<ushort>(ch.character)));
            const bool bold = ch.rendition.f.bold;
            const bool italic = ch.rendition.f.italic;
            const GlyphInfo gi = m_atlas.getGlyph(text, bold, italic);

            if (!gi.valid) {
                continue;
            }

            const QColor fg = ch.foregroundColor.color(colorTable);

            GlyphInstance glyph;
            glyph.col = static_cast<float>(col);
            glyph.row = static_cast<float>(row);
            glyph.r = static_cast<float>(fg.redF());
            glyph.g = static_cast<float>(fg.greenF());
            glyph.b = static_cast<float>(fg.blueF());
            glyph.a = static_cast<float>(fg.alphaF());
            glyph.u0 = gi.u0;
            glyph.v0 = gi.v0;
            glyph.u1 = gi.u1;
            glyph.v1 = gi.v1;
            glyph.offX = static_cast<float>(gi.bearingX);
            // bearingY is boundingRect.top() (negative for chars above baseline).
            // Adding fontAscent gives the pixel offset from cell top to glyph top.
            glyph.offY = static_cast<float>(m_fontAscent + gi.bearingY);
            glyph.w = static_cast<float>(gi.width);
            glyph.h = static_cast<float>(gi.height);
            m_glyphInstances.append(glyph);
        }
    }

    m_dataReady = true;
    m_dataDirty = true;
    update();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool TerminalGlWidget::buildShaders()
{
    if (!m_bgProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, BG_VERT)) {
        qWarning() << "BG vertex shader:" << m_bgProgram.log();
        return false;
    }
    if (!m_bgProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, BG_FRAG)) {
        qWarning() << "BG fragment shader:" << m_bgProgram.log();
        return false;
    }
    if (!m_bgProgram.link()) {
        qWarning() << "BG program link:" << m_bgProgram.log();
        return false;
    }

    if (!m_glyphProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, GLYPH_VERT)) {
        qWarning() << "Glyph vertex shader:" << m_glyphProgram.log();
        return false;
    }
    if (!m_glyphProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, GLYPH_FRAG)) {
        qWarning() << "Glyph fragment shader:" << m_glyphProgram.log();
        return false;
    }
    if (!m_glyphProgram.link()) {
        qWarning() << "Glyph program link:" << m_glyphProgram.log();
        return false;
    }

    return true;
}

void TerminalGlWidget::setupVaos()
{
    // ------------------------------------------------------------------
    // Background VAO
    // ------------------------------------------------------------------
    m_bgVao.create();
    m_bgVao.bind();

    // Slot 0: quad corner (shared geometry, divisor = 0)
    m_quadVbo.bind();
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glVertexAttribDivisor(0, 0);
    m_quadVbo.release();

    // Slots 1-2: per-instance background data (divisor = 1)
    m_bgInstanceVbo.bind();
    const int bgStride = sizeof(BgInstance);
    // location 1: cellPos (col, row)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, bgStride, reinterpret_cast<void *>(offsetof(BgInstance, col)));
    glVertexAttribDivisor(1, 1);
    // location 2: bgColor (r, g, b, a)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, bgStride, reinterpret_cast<void *>(offsetof(BgInstance, r)));
    glVertexAttribDivisor(2, 1);
    m_bgInstanceVbo.release();

    m_bgVao.release();

    // ------------------------------------------------------------------
    // Glyph VAO
    // ------------------------------------------------------------------
    m_glyphVao.create();
    m_glyphVao.bind();

    // Slot 0: quad corner
    m_quadVbo.bind();
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glVertexAttribDivisor(0, 0);
    m_quadVbo.release();

    m_glyphInstanceVbo.bind();
    const int gs = sizeof(GlyphInstance);
    // location 1: cellPos
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, gs, reinterpret_cast<void *>(offsetof(GlyphInstance, col)));
    glVertexAttribDivisor(1, 1);
    // location 2: fgColor
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, gs, reinterpret_cast<void *>(offsetof(GlyphInstance, r)));
    glVertexAttribDivisor(2, 1);
    // location 3: atlasUV
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, gs, reinterpret_cast<void *>(offsetof(GlyphInstance, u0)));
    glVertexAttribDivisor(3, 1);
    // location 4: glyphOffset
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, gs, reinterpret_cast<void *>(offsetof(GlyphInstance, offX)));
    glVertexAttribDivisor(4, 1);
    // location 5: glyphSize
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, gs, reinterpret_cast<void *>(offsetof(GlyphInstance, w)));
    glVertexAttribDivisor(5, 1);
    m_glyphInstanceVbo.release();

    m_glyphVao.release();
}

void TerminalGlWidget::uploadInstanceData()
{
    // Background instances
    m_bgInstanceVbo.bind();
    const int bgBytes = m_bgInstances.size() * static_cast<int>(sizeof(BgInstance));
    if (bgBytes > 0) {
        if (m_bgInstanceVbo.size() < bgBytes) {
            m_bgInstanceVbo.allocate(m_bgInstances.constData(), bgBytes);
        } else {
            m_bgInstanceVbo.write(0, m_bgInstances.constData(), bgBytes);
        }
    }
    m_bgInstanceVbo.release();

    // Glyph instances
    m_glyphInstanceVbo.bind();
    const int glBytes = m_glyphInstances.size() * static_cast<int>(sizeof(GlyphInstance));
    if (glBytes > 0) {
        if (m_glyphInstanceVbo.size() < glBytes) {
            m_glyphInstanceVbo.allocate(m_glyphInstances.constData(), glBytes);
        } else {
            m_glyphInstanceVbo.write(0, m_glyphInstances.constData(), glBytes);
        }
    }
    m_glyphInstanceVbo.release();
}

void TerminalGlWidget::updateRenderTimerForScreen(QScreen *newScreen)
{
    if (!m_renderTimer) {
        return;
    }

    const qreal refreshRate = (newScreen && newScreen->refreshRate() > 0.0)
        ? newScreen->refreshRate()
        : 60.0;

    // Convert Hz to milliseconds, clamped to at least 1 ms.
    // QTimer::start(msec) both sets the interval and (re)starts the timer,
    // so moving to a different-rate monitor takes effect immediately.
    const int intervalMs = qMax(1, qRound(1000.0 / refreshRate));
    m_renderTimer->start(intervalMs);
}

} // namespace Konsole

#endif // HAVE_OPENGL
