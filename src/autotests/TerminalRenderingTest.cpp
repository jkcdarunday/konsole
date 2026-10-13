/*
    SPDX-FileCopyrightText: 2026 Konsole contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QQuickWidget>
#include <QVBoxLayout>
#if QT_CONFIG(vulkan)
#include <QVulkanInstance>
#endif
#include <QtTest>
#include <algorithm>

#include "../Emulation.h"
#include "../profile/ProfileManager.h"
#include "../session/Session.h"
#include "../session/SessionController.h"
#include "../session/SessionManager.h"
#include "../terminalDisplay/TerminalDisplay.h"

using namespace Konsole;

class TerminalRenderingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testVulkanFrames()
    {
#if !QT_CONFIG(vulkan)
        QSKIP("Qt was built without Vulkan support");
#else
        if (QGuiApplication::platformName() == QLatin1String("offscreen") || QGuiApplication::platformName() == QLatin1String("minimal")) {
            QSKIP("A window system is required for Vulkan presentation");
        }
        QVulkanInstance probe;
        if (!probe.create()) {
            QSKIP("No Vulkan implementation available");
        }
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
        const bool hidden = qEnvironmentVariableIsSet("KONSOLE_GPU_TEST_HIDDEN");
        TerminalDisplay display;
        display.setAttribute(Qt::WA_TranslucentBackground);
        display.setAttribute(Qt::WA_DontShowOnScreen, hidden);
        Session session;
        disconnect(session.emulation(), &Emulation::imageSizeInitialized, &session, &Session::run);
        const QString profileName = qEnvironmentVariable("KONSOLE_GPU_TEST_PROFILE");
        const auto profile = profileName.isEmpty() ? ProfileManager::instance()->defaultProfile() : ProfileManager::instance()->loadProfile(profileName);
        QVERIFY(profile);
        SessionManager::instance()->setSessionProfile(&session, profile);
        const ColorScheme scheme;
        display.setWallpaper(scheme.wallpaper());
        SessionController controller(&session, &display, nullptr);
        display.setSessionController(&controller);
        session.addView(&display);
        display.applyProfile(profile);
        display.setBlinkingCursorEnabled(false);
        display.setBlinkingTextEnabled(false);
        display.setAnimatingCursorEnabled(false);
        display.resize(800, 480);
        display.show();
        if (!hidden) {
            QVERIFY(QTest::qWaitForWindowExposed(&display));
        }
        auto surface = display.findChild<QQuickWidget *>(QStringLiteral("terminalVulkanSurface"));
        QVERIFY(surface);
        QTRY_VERIFY(surface->quickWindow()->isSceneGraphInitialized());
        QCOMPARE(surface->quickWindow()->rendererInterface()->graphicsApi(), QSGRendererInterface::Vulkan);
        const QImage empty = surface->grabFramebuffer();
        QVERIFY(!empty.isNull());
        const QByteArray text("\033[2J\033[H\033[31mRed \033[32mGreen \033[34mBlue\033[0m\r\nUnicode: 日本語 λ █─┼\r\n");
        session.emulation()->receiveData(text.constData(), text.size());
        QTest::qWait(80);
        const QImage populated = surface->grabFramebuffer();
        QVERIFY(populated != empty);
        int coloredPixels = 0;
        for (int y = 0; y < qMin(100, populated.height()); ++y) {
            for (int x = 0; x < qMin(500, populated.width()); ++x) {
                const QColor color = populated.pixelColor(x, y);
                coloredPixels += qMax(color.red(), qMax(color.green(), color.blue())) - qMin(color.red(), qMin(color.green(), color.blue())) > 40;
            }
        }
        QVERIFY2(coloredPixels > 100, "The Vulkan surface did not draw colored terminal glyphs");

        // Multiple source clears subtract holes from an existing clip mesh.
        // Replacing that mesh must release its old geometry exactly once.
        display.update(QRegion(QRect(0, 0, display.width(), 40)) | QRect(0, 100, display.width(), 40));
        QTest::qWait(40);
        QVERIFY(!surface->grabFramebuffer().isNull());

        // Dirty strips must clear old glyphs and retain untouched content.
        const QByteArray erase("\033[H\033[2K");
        session.emulation()->receiveData(erase.constData(), erase.size());
        QTest::qWait(40);
        const QImage erased = surface->grabFramebuffer();
        QVERIFY(erased != populated);
        QTest::qWait(40);
        QCOMPARE(surface->grabFramebuffer(), erased);

        display.resize(hidden ? display.screen()->availableGeometry().size() : QSize(1000, 600));
        QTest::qWait(40);
        const QImage resized = surface->grabFramebuffer();
        qInfo() << "Vulkan benchmark viewport:" << resized.size() << "font:" << display.font() << "font line characters:" << profile->useFontLineCharacters();
        QCOMPARE(resized.size(), (QSizeF(display.size()) * display.devicePixelRatioF()).toSize());

        QSignalSpy rendered(surface->quickWindow(), &QQuickWindow::afterRendering);
        QTimer input;
        input.setTimerType(Qt::PreciseTimer);
        int sequence = 0;
        connect(&input, &QTimer::timeout, &session, [&session, &sequence] {
            const QByteArray frame = "\033[HFrame " + QByteArray::number(++sequence) + "\033[K";
            session.emulation()->receiveData(frame.constData(), frame.size());
        });
        QEventLoop loop;
        QElapsedTimer elapsed;
        elapsed.start();
        input.start(1);
        QTimer::singleShot(1500, &loop, &QEventLoop::quit);
        loop.exec();
        input.stop();
        qInfo() << "Vulkan frames rendered per second:" << rendered.count() * 1000.0 / elapsed.elapsed() << "monitor Hz:" << display.screen()->refreshRate();
        QVERIFY(rendered.count() > 0);
        rendered.clear();
        disconnect(&input, nullptr, &session, nullptr);
        connect(&input, &QTimer::timeout, &session, [&session, &sequence] {
            const QByteArray line = QByteArray::number(++sequence) + " scrolling terminal output: 0123456789 abcdefghijklmnopqrstuvwxyz\r\n";
            session.emulation()->receiveData(line.constData(), line.size());
        });
        elapsed.restart();
        input.start(1);
        QTimer::singleShot(1500, &loop, &QEventLoop::quit);
        loop.exec();
        input.stop();
        qInfo() << "Vulkan scrolling frames rendered per second:" << rendered.count() * 1000.0 / elapsed.elapsed();
        QVERIFY(rendered.count() > 0);
        QTest::qWait(80);
        const QImage scrolled = surface->grabFramebuffer();
        display.update();
        QTest::qWait(80);
        const QImage repainted = surface->grabFramebuffer();
        const QRect contentPixels((QPointF(display.contentRect().topLeft()) * display.devicePixelRatioF()).toPoint(),
                                  (QSizeF(display.contentRect().size()) * display.devicePixelRatioF()).toSize());
        const QImage expected = scrolled.copy(contentPixels);
        const QImage actual = repainted.copy(contentPixels);
        QCOMPARE(actual.size(), expected.size());
        // Moving cached glyph geometry can change shader rounding by a few
        // color levels. Large differences indicate stale text or missing rows.
        int maxDifference = 0;
        for (int y = 0; y < actual.height(); ++y) {
            for (int x = 0; x < actual.width(); ++x) {
                const QRgb a = actual.pixel(x, y), b = expected.pixel(x, y);
                maxDifference = qMax(maxDifference, qMax(qAbs(qRed(a) - qRed(b)), qMax(qAbs(qGreen(a) - qGreen(b)), qAbs(qBlue(a) - qBlue(b)))));
                maxDifference = qMax(maxDifference, qAbs(qAlpha(a) - qAlpha(b)));
            }
        }
        QVERIFY2(maxDifference <= 8, "Cached scrolling differs from a full redraw");

        // Many changing rows exercise text layout and node batching much more
        // heavily than a scrolling workload that can reuse existing rows.
        QVector<qint64> frameGaps;
        qint64 previousFrame = 0;
        const auto timingConnection = connect(surface->quickWindow(), &QQuickWindow::afterRendering, &session, [&] {
            const qint64 now = elapsed.nsecsElapsed();
            if (previousFrame > 200000000) {
                frameGaps.append(now - previousFrame);
            }
            previousFrame = now;
        });
        rendered.clear();
        disconnect(&input, nullptr, &session, nullptr);
        session.emulation()->receiveData("\033[?1049h\033[?25l", 14);
        connect(&input, &QTimer::timeout, &session, [&session, &sequence, &display] {
            QByteArray frame("\033[HGPU rendering preview\r\nInput frames: ");
            frame += QByteArray::number(sequence) + "\r\nMoving bars should travel smoothly.\r\n\r\n";
            ++sequence;
            const QByteArray partials[] = {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
            for (int row = 0; row < 24; ++row) {
                frame += QByteArray((sequence + row) % qMax(1, display.columns() - 10), ' ');
                frame += "\033[38;5;" + QByteArray::number(16 + (row * 9 + sequence) % 216) + "m";
                const int fraction = sequence % 8;
                frame += fraction == 0 ? QByteArray("████████") : "\033[7m" + partials[fraction] + "\033[27m███████" + partials[fraction];
                frame += "\033[0m\033[K\r\n";
            }
            session.emulation()->receiveData(frame.constData(), frame.size());
        });
        elapsed.restart();
        input.start(7);
        QTimer::singleShot(2500, &loop, &QEventLoop::quit);
        loop.exec();
        input.stop();
        disconnect(timingConnection);
        qInfo() << "Vulkan multi-row animation frames rendered per second:" << rendered.count() * 1000.0 / elapsed.elapsed();
        if (!frameGaps.isEmpty()) {
            std::sort(frameGaps.begin(), frameGaps.end());
            qInfo() << "Vulkan multi-row frame gap, 95th percentile/max ms:" << frameGaps[frameGaps.size() * 95 / 100] / 1000000.0
                    << frameGaps.last() / 1000000.0;
        }
        QVERIFY(rendered.count() > 0);
        QWidget host;
        host.setAttribute(Qt::WA_DontShowOnScreen, hidden);
        QVBoxLayout layout(&host);
        layout.addWidget(&display);
        host.resize(900, 550);
        host.show();
        display.show();
        if (!hidden) {
            QVERIFY(QTest::qWaitForWindowExposed(&host));
        }
        QTest::qWait(80);
        QVERIFY(surface->rootObject());
        QVERIFY(!surface->grabFramebuffer().isNull());
        display.hide();
        display.setParent(nullptr);
        session.removeView(&display);
#endif
    }
};

QTEST_MAIN(TerminalRenderingTest)
#include "TerminalRenderingTest.moc"
