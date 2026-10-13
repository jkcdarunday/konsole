/*
    SPDX-FileCopyrightText: 2026 Konsole contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TERMINALRENDERSURFACE_H
#define TERMINALRENDERSURFACE_H

#include <QOpenGLWidget>
#include <QRegion>

namespace Konsole
{
class TerminalDisplay;

// A separate surface preserves the QWidget input, accessibility and raster
// fallback paths. All terminal primitives are painted directly by Qt's OpenGL
// paint engine, including its cached glyph textures.
class TerminalRenderSurface : public QOpenGLWidget
{
public:
    explicit TerminalRenderSurface(TerminalDisplay *display);
    void scheduleUpdate(const QRegion &region);

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;
    void showEvent(QShowEvent *event) override;

private:
    TerminalDisplay *_display;
    QRegion _dirtyRegion;
};
}

#endif
