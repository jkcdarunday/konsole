/*
    SPDX-FileCopyrightText: 2024 Konsole Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "config-konsole.h"

#if HAVE_OPENGL

#include "../characters/Character.h"
#include "GlyphAtlas.h"

#include <QFont>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QTimer>
#include <QVector>

namespace Konsole
{

class TerminalFont;

/**
 * @class TerminalGlWidget
 * @brief An OpenGL-accelerated rendering surface for the terminal display.
 *
 * This widget renders the terminal character grid using the GPU:
 *
 *  1. All cell background colours are drawn as coloured quads in one
 *     instanced draw call.
 *  2. All visible glyphs are drawn in a second instanced draw call,
 *     sampling from a @c GlyphAtlas texture atlas.
 *
 * Complex visual features (cursor, search highlights, overlays) that
 * do not benefit much from GPU batching are handled by the parent
 * @c TerminalDisplay using a normal @c QPainter and fall through to the
 * regular paint path on top of the OpenGL surface.
 *
 * The widget is intended to be embedded as a child of @c TerminalDisplay
 * and resized to cover the terminal content area exactly.
 */
class TerminalGlWidget : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core
{
    Q_OBJECT

public:
    explicit TerminalGlWidget(QWidget *parent = nullptr);
    ~TerminalGlWidget() override;

    /**
     * Update the terminal cell data that will be rendered on the next
     * paint cycle.  Call this whenever @c TerminalDisplay::updateImage()
     * is called.
     *
     * @param image       Pointer to the character image array.
     * @param imageSize   Total number of entries in @p image.
     * @param columns     Number of character columns (display width in cells).
     * @param lines       Number of character lines  (display height in cells).
     * @param colorTable  The 256-entry colour table of the active colour scheme.
     * @param font        Terminal font information (cell dimensions, font objects).
     * @param contentRect The pixel rectangle within the widget that holds the
     *                    terminal grid (accounts for padding/margins).
     */
    void updateDisplayData(const Character *image,
                           int imageSize,
                           int columns,
                           int lines,
                           const QColor *colorTable,
                           TerminalFont *font,
                           const QRect &contentRect);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    // -----------------------------------------------------------------------
    // Per-instance vertex data layouts
    // -----------------------------------------------------------------------

    /** Instance data for a background quad. */
    struct BgInstance {
        float col; ///< Cell column (float for GPU interpolation)
        float row; ///< Cell row
        float r, g, b, a; ///< Background colour
    };

    /** Instance data for a glyph quad. */
    struct GlyphInstance {
        float col;
        float row;
        float r, g, b, a; ///< Foreground colour
        float u0, v0, u1, v1; ///< Atlas texture coordinates
        float offX, offY; ///< Pixel bearing offset within cell
        float w, h; ///< Pixel size of the glyph bitmap
    };

    // -----------------------------------------------------------------------
    // GL resources
    // -----------------------------------------------------------------------

    GlyphAtlas m_atlas;

    QOpenGLShaderProgram m_bgProgram;
    QOpenGLVertexArrayObject m_bgVao;
    QOpenGLBuffer m_quadVbo; ///< Unit-square vertex positions (shared)
    QOpenGLBuffer m_bgInstanceVbo;

    QOpenGLShaderProgram m_glyphProgram;
    QOpenGLVertexArrayObject m_glyphVao;
    QOpenGLBuffer m_glyphInstanceVbo;

    // -----------------------------------------------------------------------
    // Cached display state
    // -----------------------------------------------------------------------

    QVector<BgInstance> m_bgInstances;
    QVector<GlyphInstance> m_glyphInstances;

    int m_columns = 0;
    int m_lines = 0;
    int m_fontWidth = 1;
    int m_fontHeight = 1;
    int m_fontAscent = 1;
    QPoint m_contentOffset;
    QFont m_lastFont; ///< Last base font set; used to detect font changes

    bool m_dataReady = false;
    /** Set to true when new display data has arrived but not yet been rendered. */
    bool m_dataDirty = false;

    // -----------------------------------------------------------------------
    // Screen-rate render loop
    // -----------------------------------------------------------------------

    /**
     * Timer that fires at the connected screen's refresh rate and calls
     * update() whenever m_dataDirty is set.  This ensures paintGL() is
     * scheduled at the monitor's native rate (e.g. 120 Hz) rather than
     * at the terminal emulation's bulk-output rate.
     */
    QTimer *m_renderTimer = nullptr;

    /** Update the render-timer interval to match @p newScreen's refresh rate. */
    void updateRenderTimerForScreen(QScreen *newScreen);

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    bool buildShaders();
    void setupVaos();
    void uploadInstanceData();
};

} // namespace Konsole

#endif // HAVE_OPENGL
