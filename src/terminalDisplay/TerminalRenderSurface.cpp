/*
    SPDX-FileCopyrightText: 2026 Konsole contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TerminalRenderSurface.h"

#include "TerminalDisplay.h"
#include "konsoledebug.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QScreen>
#include <QTimer>

#include <utility>

using namespace Konsole;

TerminalRenderSurface::TerminalRenderSurface(TerminalDisplay *display)
    : QOpenGLWidget(display)
    , _display(display)
{
    setObjectName(QStringLiteral("terminalGpuSurface"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setUpdateBehavior(QOpenGLWidget::PartialUpdate);

    auto surfaceFormat = format();
    surfaceFormat.setAlphaBufferSize(8);
    surfaceFormat.setStencilBufferSize(8);
    surfaceFormat.setSwapInterval(1);
    setFormat(surfaceFormat);
}

void TerminalRenderSurface::scheduleUpdate(const QRegion &region)
{
    const QRegion damage = region & rect();
    if (damage.isEmpty()) {
        return;
    }
    _dirtyRegion |= damage;
    update();
}

void TerminalRenderSurface::initializeGL()
{
    _dirtyRegion = rect();
    const auto functions = context()->functions();
    qCDebug(KonsoleDebug) << "Terminal OpenGL renderer:" << reinterpret_cast<const char *>(functions->glGetString(GL_RENDERER))
                          << "screen refresh rate:" << (_display->screen() ? _display->screen()->refreshRate() : 0);
}

void TerminalRenderSurface::resizeGL(int, int)
{
    // The framebuffer is recreated on resize, DPR changes and reparenting.
    _dirtyRegion = rect();
}

void TerminalRenderSurface::paintGL()
{
    const QRegion damage = std::exchange(_dirtyRegion, QRegion());
    if (damage.isEmpty()) {
        return;
    }

    QPainter paint(this);
    paint.setClipRegion(damage);
    // Replace old alpha as well as color, so translucent backgrounds and
    // animated overlays do not accumulate in the preserved framebuffer.
    paint.setCompositionMode(QPainter::CompositionMode_Source);
    paint.fillRect(rect(), Qt::transparent);
    paint.setCompositionMode(QPainter::CompositionMode_SourceOver);
    _display->paintTerminal(paint, damage);
}

void TerminalRenderSurface::showEvent(QShowEvent *event)
{
    QOpenGLWidget::showEvent(event);
    // Qt initializes the context during the first show/resize. Defer fallback
    // until that work is complete and never delete a surface inside its event.
    QTimer::singleShot(0, this, [this] {
        if (!isValid() && !size().isEmpty()) {
            qCWarning(KonsoleDebug) << "OpenGL initialization failed; using raster terminal rendering";
            _display->disableGpuRendering();
        } else {
            scheduleUpdate(rect());
        }
    });
}
