/*
    SPDX-FileCopyrightText: 2026 Konsole contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TerminalVulkanSurface.h"

#include "TerminalColor.h"
#include "TerminalDisplay.h"
#include "TerminalFonts.h"
#include "konsoledebug.h"

#include <QCache>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QPaintEngine>
#include <QPainter>
#include <QPainterPath>
#include <QQuickItem>
#include <QSGClipNode>
#include <QSGImageNode>
#include <QSGOpacityNode>
#include <QSGRectangleNode>
#include <QSGTextNode>
#include <QScreen>
#include <QTextLayout>
#include <QTimer>
#include <private/qquickwidget_p.h>

#include <chrono>
#include <functional>
#include <memory>
#include <utility>

namespace Konsole
{
namespace
{
// Rebuild only damaged strips. The scene graph retains the other strips and
// batches rectangles, image textures and glyph atlas draws through Vulkan.

class PaintClip : public QSGClipNode
{
public:
    explicit PaintClip(const QRegion &region)
    {
        setRegion(region);
    }

    void setRegion(const QRegion &region)
    {
        clip = region;
        setClipRect(region.boundingRect());
        setIsRectangular(region.rectCount() <= 1);
        if (region.rectCount() > 1) {
            auto mesh = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), region.rectCount() * 6);
            mesh->setDrawingMode(QSGGeometry::DrawTriangles);
            auto vertices = mesh->vertexDataAsPoint2D();
            for (const QRect &rect : region) {
                const float left = rect.x(), top = rect.y();
                const float right = rect.x() + rect.width(), bottom = rect.y() + rect.height();
                (vertices++)->set(left, top);
                (vertices++)->set(right, top);
                (vertices++)->set(left, bottom);
                (vertices++)->set(left, bottom);
                (vertices++)->set(right, top);
                (vertices++)->set(right, bottom);
            }
            // setGeometry releases the previous mesh when OwnsGeometry is set.
            setGeometry(mesh);
            setFlag(OwnsGeometry);
        }
        markDirty(DirtyGeometry);
    }

    QRegion clip;
};

struct Strip {
    QRect rect;
    QSGTransformNode *node;
    int offset = 0;
};

class TerminalSceneNode : public QSGNode
{
public:
    QSize size;
    int lineHeight = 0;
    int contentTop = 0;
    QRect contentRect;
    QVector<Strip> strips;
    // The cache owns textures; image nodes borrow them and are all destroyed
    // with this scene graph root when its Vulkan device is invalidated.
    QCache<QByteArray, std::shared_ptr<QSGTexture>> textures{32 * 1024 * 1024};
};

class TextureOwnerNode : public QSGNode
{
public:
    std::shared_ptr<QSGTexture> texture;
};

class ScenePaintEngine : public QPaintEngine
{
    struct TextState {
        QTransform transform;
        QRegion clip;
        QColor color;
        qreal opacity;
        bool antialias;
        bool operator==(const TextState &) const = default;
    };
    struct TextRun {
        QString text;
        QFont font;
        QPointF point;
        QPointF lastPoint;
        qreal glyphWidth = 0;
        qreal cellWidth = 0;
        qreal ascent = 0;
        qreal descent = 0;
        QTextItem::RenderFlags flags;
        TextState state;
        bool independentGlyph = false;
    };

public:
    ScenePaintEngine(QQuickWindow *window, TerminalSceneNode *root, const QVector<Strip *> &strips, qreal dpr)
        : QPaintEngine(PrimitiveTransform | AlphaBlend | PorterDuff | PainterPaths | Antialiasing | ConstantOpacity | PixmapTransform)
        , _window(window)
        , _root(root)
        , _strips(strips)
        , _dpr(dpr)
    {
    }

    bool begin(QPaintDevice *) override
    {
        setActive(true);
        return true;
    }
    bool end() override
    {
        flushText();
        setActive(false);
        return true;
    }
    Type type() const override
    {
        return User;
    }
    void updateState(const QPaintEngineState &) override
    {
        flushText();
    }

    void drawRects(const QRectF *rects, int count) override
    {
        flushText();
        for (int i = 0; i < count; ++i) {
            const QRectF rect = rects[i];
            if (state->pen().style() == Qt::NoPen && state->brush().style() == Qt::SolidPattern) {
                const bool clearBackground = state->brush().color() == _window->color() && qFuzzyCompare(state->opacity(), qreal(1))
                    && (state->compositionMode() == QPainter::CompositionMode_Source || state->brush().color().alpha() == 255);
                append(
                    rect,
                    [this, rect, clearBackground]() -> QSGNode * {
                        if (clearBackground) {
                            return nullptr;
                        }
                        auto node = _window->createRectangleNode();
                        node->setRect(rect);
                        node->setColor(state->brush().color());
                        return node;
                    },
                    clearBackground || state->compositionMode() == QPainter::CompositionMode_Source);
            } else {
                rasterize(rect, [rect](QPainter &paint) {
                    paint.drawRect(rect);
                });
            }
        }
    }

    void drawRects(const QRect *rects, int count) override
    {
        for (int i = 0; i < count; ++i) {
            const QRectF rect(rects[i]);
            drawRects(&rect, 1);
        }
    }

    void drawLines(const QLineF *lines, int count) override
    {
        for (int i = 0; i < count; ++i) {
            const QLineF line = lines[i];
            rasterize(QRectF(line.p1(), line.p2()).normalized(), [line](QPainter &paint) {
                paint.drawLine(line);
            });
        }
    }

    void drawLines(const QLine *lines, int count) override
    {
        for (int i = 0; i < count; ++i) {
            const QLineF line(lines[i]);
            drawLines(&line, 1);
        }
    }

    void drawEllipse(const QRectF &rect) override
    {
        rasterize(rect, [rect](QPainter &paint) {
            paint.drawEllipse(rect);
        });
    }

    void drawEllipse(const QRect &rect) override
    {
        drawEllipse(QRectF(rect));
    }

    void drawPath(const QPainterPath &path) override
    {
        rasterize(path.boundingRect(), [&path](QPainter &paint) {
            paint.drawPath(path);
        });
    }

    void drawPolygon(const QPointF *points, int count, PolygonDrawMode mode) override
    {
        QPolygonF polygon;
        polygon.reserve(count);
        for (int i = 0; i < count; ++i) {
            polygon.append(points[i]);
        }
        rasterize(polygon.boundingRect(), [&polygon, mode](QPainter &paint) {
            if (mode == PolylineMode) {
                paint.drawPolyline(polygon);
            } else {
                paint.drawPolygon(polygon, mode == WindingMode ? Qt::WindingFill : Qt::OddEvenFill);
            }
        });
    }

    void drawPolygon(const QPoint *points, int count, PolygonDrawMode mode) override
    {
        QPolygonF polygon;
        polygon.reserve(count);
        for (int i = 0; i < count; ++i) {
            polygon.append(points[i]);
        }
        drawPolygon(polygon.constData(), count, mode);
    }

    void drawPoints(const QPointF *points, int count) override
    {
        for (int i = 0; i < count; ++i) {
            const QPointF point = points[i];
            rasterize(QRectF(point, QSizeF()), [point](QPainter &paint) {
                paint.drawPoint(point);
            });
        }
    }

    void drawPoints(const QPoint *points, int count) override
    {
        for (int i = 0; i < count; ++i) {
            const QPointF point(points[i]);
            drawPoints(&point, 1);
        }
    }

    void drawTextItem(const QPointF &point, const QTextItem &item) override
    {
        const TextState current{state->transform(),
                                currentClip(),
                                state->pen().color(),
                                state->opacity(),
                                state->renderHints().testFlag(QPainter::TextAntialiasing)};
        QString text = item.text();
        text.remove(QChar(0x202d));
        const ushort code = text.size() == 1 ? text.at(0).unicode() : 0;
        // Box, block and braille symbols also have independent cell glyphs.
        // Animated terminal graphics often repeat these across an entire row.
        const bool independentGlyph =
            text.size() == 1 && ((code >= 32 && code < 127) || (code >= 0x2500 && code <= 0x28ff)) && !item.renderFlags().testFlag(QTextItem::RightToLeft);
        const qreal cellWidth = point.x() - _text.lastPoint.x();
        if (independentGlyph && _text.independentGlyph && _text.state == current && _text.font == item.font() && _text.flags == item.renderFlags()
            && qAbs(point.y() - _text.point.y()) < 0.001 && qAbs(item.width() - _text.glyphWidth) < 0.001 && cellWidth > 0
            && (_text.text.size() == 1 || qAbs(cellWidth - _text.cellWidth) < 0.001)) {
            _text.text += text;
            _text.lastPoint = point;
            _text.cellWidth = cellWidth;
            return;
        }
        flushText();
        _text = {independentGlyph ? text : item.text(),
                 item.font(),
                 point,
                 point,
                 item.width(),
                 0,
                 item.ascent(),
                 item.descent(),
                 item.renderFlags(),
                 current,
                 independentGlyph};
        if (!independentGlyph) {
            flushText();
        }
    }

    void drawPixmap(const QRectF &rect, const QPixmap &pixmap, const QRectF &source) override
    {
        drawImage(rect, pixmap.toImage(), source);
    }

    void drawImage(const QRectF &rect, const QImage &image, const QRectF &source, Qt::ImageConversionFlags = Qt::AutoColor) override
    {
        flushText();
        const QByteArray key = QByteArray::number(image.cacheKey());
        imageNode(rect, image, source, key);
    }

private:
    QRegion currentClip() const
    {
        return painter()->hasClipping() ? state->transform().map(painter()->clipRegion()) : QRegion(QRect(QPoint(), _root->size));
    }

    void flushText()
    {
        if (_text.text.isEmpty()) {
            return;
        }
        const TextRun run = std::exchange(_text, TextRun());
        QFont font = run.font;
        qreal width = run.glyphWidth;
        if (run.independentGlyph && run.text.size() > 1) {
            // Combine independently positioned cell glyphs without introducing
            // ligatures or changing their spacing, including gaps across spaces.
            font.setKerning(false);
            font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() | QFont::PreferNoShaping));
            font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
            const qreal naturalWidth = QFontMetricsF(font).horizontalAdvance(run.text.at(0));
            font.setLetterSpacing(QFont::AbsoluteSpacing, run.cellWidth - naturalWidth);
            width = run.cellWidth * run.text.size();
        }
        const QRectF bounds(run.point.x() - 2, run.point.y() - run.ascent - 2, width + 4, run.ascent + run.descent + 4);
        // Layout/shaping stays with Qt. QSGTextNode caches the glyph images in
        // GPU atlases; terminal frames are not rasterized to full-screen images.
        append(
            bounds,
            [this, &run, &font, width] {
                auto node = _window->createTextNode();
                node->setColor(run.state.color);
                node->setRenderType(run.state.antialias ? QSGTextNode::QtRendering : QSGTextNode::NativeRendering);
                QTextLayout layout(run.text, font);
                QTextOption options;
                options.setTextDirection(run.flags.testFlag(QTextItem::RightToLeft) ? Qt::RightToLeft : Qt::LeftToRight);
                options.setAlignment(Qt::AlignLeft);
                layout.setTextOption(options);
                layout.beginLayout();
                QTextLine line = layout.createLine();
                line.setLineWidth(qCeil(width) + 1);
                layout.endLayout();
                node->addTextLayout(QPointF(run.point.x(), run.point.y() - line.ascent()), &layout);
                return node;
            },
            false,
            &run.state);
    }

    // Each command is clipped in device coordinates before its painter
    // transform. This covers double-height lines, sixel clipping and overlays.
    void append(const QRectF &bounds, const std::function<QSGNode *()> &makeNode, bool replace = false, const TextState *textState = nullptr)
    {
        const QTransform transform = textState ? textState->transform : state->transform();
        const QRegion clip = textState ? textState->clip : currentClip();
        const QRegion coverage = clip & transform.mapRect(bounds).toAlignedRect();
        for (Strip *strip : _strips) {
            const QRegion region = clip & strip->rect;
            const QRegion covered = coverage & strip->rect;
            if (covered.isEmpty()) {
                continue;
            }
            if (replace) {
                // Source backgrounds replace alpha and any previous preedit
                // glyphs rather than blending twice into the same pixels.
                for (auto child = strip->node->firstChild(); child; child = child->nextSibling()) {
                    auto previous = static_cast<PaintClip *>(child);
                    previous->setRegion(previous->clip - covered);
                }
            }
            QSGNode *node = makeNode();
            if (!node) {
                continue;
            }
            // Commands with the same clip share a scene graph clip node so
            // Qt can batch their glyphs and rectangles in one Vulkan draw.
            auto clipped = static_cast<PaintClip *>(strip->node->lastChild());
            if (replace || !clipped || clipped->clip != region) {
                clipped = new PaintClip(region);
                strip->node->appendChildNode(clipped);
            }
            QSGNode *parent = clipped;
            // Identity transforms and full opacity are overwhelmingly common.
            // Extra nodes split batches and make rebuilding busy rows costly.
            if (!transform.isIdentity()) {
                auto transformed = new QSGTransformNode;
                transformed->setMatrix(QMatrix4x4(transform));
                parent->appendChildNode(transformed);
                parent = transformed;
            }
            const qreal alpha = textState ? textState->opacity : state->opacity();
            if (!qFuzzyCompare(alpha, qreal(1))) {
                auto opacity = new QSGOpacityNode;
                opacity->setOpacity(alpha);
                parent->appendChildNode(opacity);
                parent = opacity;
            }
            parent->appendChildNode(node);
        }
    }

    void imageNode(const QRectF &rect, const QImage &image, const QRectF &source, const QByteArray &key)
    {
        append(rect, [this, &rect, &image, &source, &key] {
            std::shared_ptr<QSGTexture> texture;
            if (auto cached = _root->textures.object(key)) {
                texture = *cached;
            } else {
                texture.reset(_window->createTextureFromImage(image, QQuickWindow::TextureCanUseAtlas));
                _root->textures.insert(key, new std::shared_ptr<QSGTexture>(texture), int(image.sizeInBytes()));
            }
            auto owner = new TextureOwnerNode;
            owner->texture = texture;
            auto node = _window->createImageNode();
            owner->appendChildNode(node);
            node->setTexture(texture.get());
            node->setRect(rect);
            node->setSourceRect(source);
            node->setFiltering(state->renderHints().testFlag(QPainter::SmoothPixmapTransform) ? QSGTexture::Linear : QSGTexture::Nearest);
            return owner;
        });
    }

    void rasterize(const QRectF &bounds, const std::function<void(QPainter &)> &draw)
    {
        flushText();
        // Rare vector decorations use small cached textures. Text and cell
        // backgrounds use GPU nodes directly; no full-screen CPU upload occurs.
        const qreal padding = qMax(qreal(2), state->pen().widthF() * 2);
        const QRect rect = bounds.adjusted(-padding, -padding, padding, padding).toAlignedRect();
        QImage image((QSizeF(rect.size()) * _dpr).toSize(), QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(_dpr);
        image.fill(Qt::transparent);
        QPainter paint(&image);
        paint.setPen(state->pen());
        paint.setBrush(state->brush());
        paint.setBrushOrigin(state->brushOrigin());
        paint.setRenderHints(state->renderHints());
        paint.translate(-rect.topLeft());
        draw(paint);
        paint.end();
        const QByteArray key = QByteArray::number(image.width()) + ':' + QByteArray::number(image.height()) + ':' + QByteArray::number(_dpr) + ':'
            + QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes()), QCryptographicHash::Sha256);
        imageNode(rect, image, QRectF(QPointF(), image.size()), key);
    }

    QQuickWindow *_window;
    TerminalSceneNode *_root;
    QVector<Strip *> _strips;
    qreal _dpr;
    TextRun _text;
};

class ScenePaintDevice : public QPaintDevice
{
public:
    ScenePaintDevice(ScenePaintEngine &engine, QWidget *widget)
        : _engine(engine)
        , _widget(widget)
    {
    }
    QPaintEngine *paintEngine() const override
    {
        return &_engine;
    }

protected:
    int metric(PaintDeviceMetric metric) const override
    {
        switch (metric) {
        case PdmWidth:
            return _widget->width();
        case PdmHeight:
            return _widget->height();
        case PdmWidthMM:
            return qRound(_widget->width() * 25.4 / _widget->logicalDpiX());
        case PdmHeightMM:
            return qRound(_widget->height() * 25.4 / _widget->logicalDpiY());
        case PdmDpiX:
            return _widget->logicalDpiX();
        case PdmDpiY:
            return _widget->logicalDpiY();
        case PdmPhysicalDpiX:
            return _widget->physicalDpiX();
        case PdmPhysicalDpiY:
            return _widget->physicalDpiY();
        case PdmDepth:
            return 32;
        case PdmNumColors:
            return 16777216;
        case PdmDevicePixelRatio:
            // Scene graph coordinates are logical pixels. Qt Quick applies
            // the window's DPR once when rendering; QPainter must not add a
            // second device transform before we build those nodes.
            return 1;
        case PdmDevicePixelRatioScaled:
            return int(devicePixelRatioFScale());
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        case PdmDevicePixelRatioF_EncodedA:
        case PdmDevicePixelRatioF_EncodedB:
            return encodeMetricF(metric, 1.0);
#endif
        default:
            return QPaintDevice::metric(metric);
        }
    }

private:
    ScenePaintEngine &_engine;
    QWidget *_widget;
};
}

class TerminalScene : public QQuickItem
{
public:
    explicit TerminalScene(TerminalVulkanSurface *surface)
        : _surface(surface)
    {
        setFlag(ItemHasContents);
        auto resize = [this] {
            damage |= boundingRect().toAlignedRect();
            update();
        };
        connect(this, &QQuickItem::widthChanged, this, resize);
        connect(this, &QQuickItem::heightChanged, this, resize);
        resize();
    }

    QRegion damage;

    void scroll(int lines, const QRect &characterRegion)
    {
        const int height = _surface->_display->terminalFont()->fontHeight();
        const int top = _surface->_display->contentRect().top() + characterRegion.top() * height;
        const QRect content = _surface->_display->contentRect();
        const QRect region(content.left(), top, content.width(), characterRegion.height() * height);
        const int delta = -lines * height;
        // Preserve damage from updates coalesced before this scroll too.
        damage = (damage - region) | ((damage & region).translated(0, delta) & region);
        damage |=
            delta < 0 ? QRect(region.left(), region.bottom() + delta + 1, region.width(), -delta) : QRect(region.left(), region.top(), region.width(), delta);
        _scrolls.append({region, delta});
        update();
    }

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override
    {
        // QQuickWidget uses its GUI-thread render loop, so the terminal model
        // and existing painter are accessed on their owning thread.
        auto root = static_cast<TerminalSceneNode *>(oldNode);
        const QSize size = boundingRect().size().toSize();
        const int lineHeight = qMax(1, _surface->_display->terminalFont()->fontHeight());
        const int contentTop = _surface->_display->contentRect().top();
        const QRect content = _surface->_display->contentRect();
        if (!root || root->size != size || root->lineHeight != lineHeight || root->contentRect != content) {
            delete root;
            root = new TerminalSceneNode;
            root->size = size;
            root->lineHeight = lineHeight;
            root->contentTop = contentTop;
            root->contentRect = content;
            // Keep the scroll-highlight gutter separate. Its fading rectangle
            // can damage the full height without invalidating any glyph rows.
            for (const QRect &gutter : {QRect(0, contentTop, content.left(), size.height() - contentTop),
                                        QRect(content.right() + 1, contentTop, size.width() - content.right() - 1, size.height() - contentTop)}) {
                if (!gutter.isEmpty()) {
                    auto node = new QSGTransformNode;
                    root->appendChildNode(node);
                    root->strips.append({gutter, node});
                }
            }
            for (int y = 0; y < size.height();) {
                const int height = y < contentTop ? contentTop - y : qMin(lineHeight, size.height() - y);
                auto node = new QSGTransformNode;
                root->appendChildNode(node);
                root->strips.append({QRect(y < contentTop ? 0 : content.left(), y, y < contentTop ? size.width() : content.width(), height), node});
                y += height;
            }
            damage = boundingRect().toAlignedRect();
        } else {
            for (const auto &[region, delta] : std::as_const(_scrolls)) {
                QVector<int> indices;
                QVector<Strip> previous;
                for (int i = 0; i < root->strips.size(); ++i) {
                    const Strip &strip = root->strips[i];
                    if (region.contains(strip.rect) && strip.rect.height() == lineHeight) {
                        indices.append(i);
                        previous.append(strip);
                    }
                }
                const int lines = -delta / lineHeight;
                for (int i = 0; i < indices.size(); ++i) {
                    Strip &strip = root->strips[indices[i]];
                    const int source = i + lines;
                    if (source >= 0 && source < previous.size()) {
                        strip.node = previous[source].node;
                        strip.offset = previous[source].offset + delta;
                        QMatrix4x4 transform;
                        transform.translate(0, strip.offset);
                        strip.node->setMatrix(transform);
                    } else {
                        strip.node = new QSGTransformNode;
                        strip.offset = 0;
                        damage |= strip.rect;
                        root->appendChildNode(strip.node);
                    }
                }
                for (int i = 0; i < previous.size(); ++i) {
                    const int destination = i - lines;
                    if (destination < 0 || destination >= previous.size()) {
                        delete previous[i].node;
                    }
                }
            }
        }
        _scrolls.clear();
        QVector<Strip *> strips;
        QRegion repaint;
        for (Strip &strip : root->strips) {
            if (damage.intersects(strip.rect)) {
                while (auto child = strip.node->firstChild()) {
                    delete child;
                }
                strip.offset = 0;
                strip.node->setMatrix(QMatrix4x4());
                strips.append(&strip);
                repaint |= strip.rect;
            }
        }
        damage = QRegion();
        if (!strips.isEmpty()) {
            ScenePaintEngine engine(window(), root, strips, _surface->devicePixelRatioF());
            ScenePaintDevice device(engine, _surface);
            QPainter paint(&device);
            paint.setFont(_surface->font());
            paint.setClipRegion(repaint);
            _surface->paintTerminal(paint, repaint);
        }
        if (qEnvironmentVariableIsSet("KONSOLE_RENDER_STATS")) {
            if (!_statsTimer.isValid()) {
                _statsTimer.start();
            }
            ++_frames;
            _rows += strips.size();
            if (_statsTimer.elapsed() >= 1000) {
                qInfo() << "Vulkan scene updates/sec:" << _frames * 1000.0 / _statsTimer.elapsed() << "repainted strips/frame:" << _rows / qreal(_frames);
                _frames = 0;
                _rows = 0;
                _statsTimer.restart();
            }
        }
        return root;
    }

private:
    TerminalVulkanSurface *_surface;
    QVector<std::pair<QRect, int>> _scrolls;
    QElapsedTimer _statsTimer;
    int _frames = 0;
    int _rows = 0;
};

TerminalVulkanSurface::TerminalVulkanSurface(TerminalDisplay *display)
    : QQuickWidget(display)
    , _display(display)
    , _scene(new TerminalScene(this))
{
    setObjectName(QStringLiteral("terminalVulkanSurface"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_AcceptTouchEvents, false);
    setAcceptDrops(false);
    setFocusPolicy(Qt::NoFocus);
    setClearColor(Qt::transparent);
    setResizeMode(QQuickWidget::SizeRootObjectToView);
    // Register the root so Qt preserves it when moving a terminal between
    // top-level tab/split windows and recreating its render control.
    setContent(QUrl(), nullptr, _scene);
    connect(this, &QQuickWidget::sceneGraphError, this, [this](QQuickWindow::SceneGraphError, const QString &message) {
        qCWarning(KonsoleDebug) << "Vulkan terminal rendering failed:" << message;
        QTimer::singleShot(0, _display, &TerminalDisplay::disableGpuRendering);
    });
    connect(quickWindow(), &QQuickWindow::sceneGraphInitialized, this, [this] {
        qCDebug(KonsoleDebug) << "Terminal scene graph renderer:" << quickWindow()->rendererInterface()->graphicsApi()
                              << "screen refresh rate:" << (_display->screen() ? _display->screen()->refreshRate() : 0);
        _scene->damage = rect();
        _scene->update();
    });
}

void TerminalVulkanSurface::scheduleUpdate(const QRegion &region)
{
    if (!(region & rect()).isEmpty()) {
        _scene->damage |= region;
        _scene->update();
        requestFrame();
    }
}

void TerminalVulkanSurface::scrollImage(int lines, const QRect &region)
{
    _scene->scroll(lines, region);
    requestFrame();
}

void TerminalVulkanSurface::requestFrame()
{
    // Emulation already batches to the display's refresh interval. Qt Quick
    // Widgets adds another fixed 5 ms batching timer, which misses high-refresh
    // deadlines. Keep its normal rendering/lifetime machinery, but schedule
    // that pending render for the next event-loop turn instead.
    // Private layouts require the exact Qt build used at compilation. If Qt
    // was upgraded independently, retain Qt's default batching until rebuilt.
    if (qstrcmp(qVersion(), QT_VERSION_STR) != 0) {
        return;
    }
    auto widget = QQuickWidgetPrivate::get(this);
    if (widget->eventPending && widget->updateTimer.isActive()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        widget->updateTimer.start(std::chrono::milliseconds(0), Qt::PreciseTimer, this);
#else
        widget->updateTimer.start(0, Qt::PreciseTimer, this);
#endif
    }
}

void TerminalVulkanSurface::paintTerminal(QPainter &painter, const QRegion &region)
{
    // A single clear supplies the background beneath all retained rows. It
    // also avoids fractional-pixel seams between translated row rectangles.
    QColor background = Qt::transparent;
    if (_display->wallpaper()->isNull()) {
        background = _display->terminalColor()->backgroundColor();
        if (_display->window()->testAttribute(Qt::WA_TranslucentBackground)) {
            background.setAlpha(qAlpha(_display->terminalColor()->blendColor()));
        }
    }
    if (quickWindow()->color() != background) {
        setClearColor(background);
    }
    _display->paintTerminal(painter, region);
}
}
