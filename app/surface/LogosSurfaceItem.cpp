#include "LogosSurfaceItem.h"

#include <QLoggingCategory>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGSimpleRectNode>
#include <QTimer>

#ifdef LOGOS_HAVE_WAYLAND_COMPOSITOR
#include "SurfaceCompositor.h"
#include <QWaylandBufferRef>
#include <QWaylandOutput>
#include <QWaylandQuickShellSurfaceItem>
#include <QWaylandSurface>
#include <QWaylandView>
#include <QWaylandXdgShell>
#endif

Q_LOGGING_CATEGORY(lcSurfaceItem, "basecamp.surface.item")

namespace {
constexpr const char* kEngineModuleProperty = "logos.surface.module";
}

int LogosSurfaceItem::s_nextViewSerial = 1;

LogosSurfaceItem::LogosSurfaceItem(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::NoButton);   // input goes to the shell item
}

LogosSurfaceItem::~LogosSurfaceItem() = default;

void LogosSurfaceItem::bindEngine(QQmlEngine* engine, const QString& module)
{
    if (engine) engine->setProperty(kEngineModuleProperty, module);
}

QString LogosSurfaceItem::boundModule(QQmlEngine* engine)
{
    return engine ? engine->property(kEngineModuleProperty).toString() : QString();
}

void LogosSurfaceItem::setModule(const QString& module)
{
    if (m_module == module) return;
    m_module = module;
    emit moduleChanged();
    tryBind();
}

void LogosSurfaceItem::setSession(const QString& session)
{
    if (m_session == session) return;
    m_session = session;
    emit sessionChanged();
}

void LogosSurfaceItem::setPlaceholderColor(const QColor& color)
{
    if (m_placeholderColor == color) return;
    m_placeholderColor = color;
    emit placeholderColorChanged();
    update();
}

void LogosSurfaceItem::componentComplete()
{
    QQuickItem::componentComplete();
    tryBind();
}

void LogosSurfaceItem::itemChange(ItemChange change, const ItemChangeData& value)
{
    QQuickItem::itemChange(change, value);
    if (change == ItemSceneChange) {
        tryBind();
        updateOutputGate();
    } else if (change == ItemVisibleHasChanged) {
        updateOutputGate();
    }
}

void LogosSurfaceItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() == oldGeometry.size()) return;
#ifdef LOGOS_HAVE_WAYLAND_COMPOSITOR
    if (m_shellItem) m_shellItem->setSize(newGeometry.size());
#endif
    sendConfigure();
    update();
}

QSGNode* LogosSurfaceItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    // Once the client's buffer is on screen the shell item paints; the
    // placeholder behind it would only cost fill rate.
    if (m_ready) {
        delete oldNode;
        return nullptr;
    }
    auto* node = static_cast<QSGSimpleRectNode*>(oldNode);
    if (!node) node = new QSGSimpleRectNode;
    node->setRect(boundingRect());
    node->setColor(m_placeholderColor);
    return node;
}

void LogosSurfaceItem::setReady(bool ready)
{
    if (m_ready == ready) return;
    m_ready = ready;
    emit readyChanged();
    update();
}

void LogosSurfaceItem::setTransport(const QString& transport)
{
    if (m_transport == transport) return;
    m_transport = transport;
    emit transportChanged();
}

void LogosSurfaceItem::fail(const QString& code)
{
    if (m_failed) return;
    m_failed = true;
    setTransport(QStringLiteral("none"));
    qCWarning(lcSurfaceItem) << "Logos.Surface for" << m_module << ":" << code;
    emit error(code);
}

void LogosSurfaceItem::tryBind()
{
    if (m_failed || m_waiting) return;
    if (!isComponentComplete() || !window() || m_module.isEmpty()) return;
#ifndef LOGOS_HAVE_WAYLAND_COMPOSITOR
    fail(QStringLiteral("noCompositor"));
#else
    if (m_shellItem) return;

    SurfaceCompositor* compositor = SurfaceCompositor::existing();
    if (!compositor || !compositor->isAvailable()) {
        fail(QStringLiteral("noCompositor"));
        return;
    }
    // A view may only show its own backend. The sandbox recorded on the engine
    // which module this view belongs to; nothing the QML can set changes it.
    const QString own = boundModule(qmlEngine(this));
    if (own.isEmpty() || own != m_module) {
        fail(QStringLiteral("tokenMismatch"));
        return;
    }
    const QString token = compositor->tokenFor(m_module);
    if (token.isEmpty()) {
        fail(QStringLiteral("notGuiModule"));
        return;
    }

    m_compositor = compositor;
    m_token = token;
    compositor->ensureOutput(window());
    connect(compositor, &SurfaceCompositor::surfaceLost, this, [this](const QString& t) {
        if (t == m_token) detach(QStringLiteral("clientExited"));
    });
    connect(compositor, &SurfaceCompositor::surfaceBound, this, [this](const QString& t) {
        if (t == m_token && !m_shellItem) {
            m_waiting = false;
            attach();
        }
    });
    if (compositor->boundSurface(token))
        attach();
    else
        m_waiting = true;
#endif
}

void LogosSurfaceItem::attach()
{
#ifdef LOGOS_HAVE_WAYLAND_COMPOSITOR
    if (!m_compositor || m_shellItem) return;
    QWaylandXdgSurface* xdg = m_compositor->boundSurface(m_token);
    QWaylandXdgToplevel* toplevel = m_compositor->boundToplevel(m_token);
    if (!xdg || !toplevel || !xdg->surface()) return;

    m_xdgSurface = xdg;
    m_toplevel = toplevel;

    // The output must exist before the shell item meets its surface; Qt
    // asserts outputFor(window) when it connects the two.
    m_compositor->ensureOutput(window());

    m_shellItem = new QWaylandQuickShellSurfaceItem(this);
    m_shellItem->setSize(size());
    m_shellItem->setInputEventsEnabled(true);
    m_shellItem->setTouchEventsEnabled(true);
    m_shellItem->setFocusOnClick(true);
    m_shellItem->setShellSurface(xdg);

    m_viewId = QStringLiteral("%1/%2/%3").arg(m_module, m_session).arg(s_nextViewSerial++);
    emit viewIdChanged();

    QWaylandSurface* surface = xdg->surface();
    auto onContent = [this, surface] {
        if (!m_shellItem || !surface->hasContent()) return;
        const QWaylandBufferRef buffer = m_shellItem->view()->currentBuffer();
        if (buffer.hasBuffer())
            setTransport(buffer.isSharedMemory() ? QStringLiteral("wayland-shm")
                                                 : QStringLiteral("wayland-dmabuf"));
        setReady(true);
    };
    connect(surface, &QWaylandSurface::hasContentChanged, this, onContent);
    connect(surface, &QWaylandSurface::redraw, this, onContent);

    sendConfigure();
    updateOutputGate();
    onContent();

    if (!m_enterTimer) {
        m_enterTimer = new QTimer(this);
        m_enterTimer->setInterval(100);
        connect(m_enterTimer, &QTimer::timeout, this, &LogosSurfaceItem::ensureEntered);
    }
    m_enterAttempts = 0;
    m_enterTimer->start();
    qCInfo(lcSurfaceItem) << "attached" << m_viewId;
#endif
}

void LogosSurfaceItem::ensureEntered()
{
#ifdef LOGOS_HAVE_WAYLAND_COMPOSITOR
    // Qt sends wl_output.enter from the first frame-callback round after the
    // surface has content, and marks it done even when the client has not yet
    // bound that output (the per-window output is created after the client
    // connected, so this is the normal case here). A client that misses the
    // enter keeps rendering at the headless output's scale. Until its buffer
    // scale matches the window output, re-attach the view to the output —
    // which resets Qt's entered flag — and ask for a host frame to resend it.
    if (!m_shellItem || !m_xdgSurface || !m_compositor || !isVisible() || !window()) {
        if (m_enterTimer) m_enterTimer->stop();
        return;
    }
    QWaylandOutput* output = m_compositor->outputFor(window());
    const bool matched = output && m_xdgSurface->surface()->bufferScale() == output->scaleFactor();
    if (matched || ++m_enterAttempts > 30) {
        m_enterTimer->stop();
        if (!matched)
            qCWarning(lcSurfaceItem) << m_viewId << "client never adopted the output scale";
        return;
    }
    if (output) {
        m_shellItem->setOutput(nullptr);
        m_shellItem->setOutput(output);
        m_shellItem->update();
    }
#endif
}

void LogosSurfaceItem::detach(const QString& errorCode)
{
#ifdef LOGOS_HAVE_WAYLAND_COMPOSITOR
    if (!m_shellItem) return;
    if (m_enterTimer) m_enterTimer->stop();
    // Deleting the item drops its view; setShellSurface(nullptr) on an
    // already-destroyed shell surface only draws a Qt warning.
    m_shellItem->setOutput(nullptr);
    m_shellItem->setVisible(false);
    m_shellItem->deleteLater();
    m_shellItem = nullptr;
    m_toplevel.clear();
    m_xdgSurface.clear();
    setReady(false);
    setTransport(QStringLiteral("none"));
    m_waiting = true;   // a re-bound surface under the same token re-attaches
    qCWarning(lcSurfaceItem) << m_viewId << errorCode;
    emit error(errorCode);
#else
    Q_UNUSED(errorCode);
#endif
}

void LogosSurfaceItem::sendConfigure()
{
#ifdef LOGOS_HAVE_WAYLAND_COMPOSITOR
    if (!m_toplevel) return;
    const QSize logical(qRound(width()), qRound(height()));
    if (logical.isEmpty()) return;
    // Logical pixels; the client multiplies by the output scale itself.
    m_toplevel->sendConfigure(logical, QList<int>{});
#endif
}

void LogosSurfaceItem::updateOutputGate()
{
#ifdef LOGOS_HAVE_WAYLAND_COMPOSITOR
    if (!m_shellItem || !m_compositor) return;
    // An invisible item leaves its output: the surface then gets no frame
    // callbacks and the client idles (RFC-5 rule 1). The hidden-tab case is
    // already covered by the tab's window not rendering at all.
    QWaylandOutput* output = isVisible() && window() ? m_compositor->outputFor(window()) : nullptr;
    m_shellItem->setOutput(output);
#endif
}
