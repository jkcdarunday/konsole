/*
    SPDX-FileCopyrightText: 2026 Konsole contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TERMINALVULKANSURFACE_H
#define TERMINALVULKANSURFACE_H

#include <QQuickWidget>
#include <QRegion>

namespace Konsole
{
class TerminalDisplay;
class TerminalScene;

class TerminalVulkanSurface : public QQuickWidget
{
public:
    explicit TerminalVulkanSurface(TerminalDisplay *display);
    void scheduleUpdate(const QRegion &region);
    void scrollImage(int lines, const QRect &region);
    void paintTerminal(QPainter &painter, const QRegion &region);

private:
    friend class TerminalScene;
    void requestFrame();
    TerminalDisplay *_display;
    TerminalScene *_scene;
};
}

#endif
