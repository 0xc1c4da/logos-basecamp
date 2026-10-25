#include "SurfaceCompositor.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QScreen>
#include <QTimer>
#include <QWaylandClient>
#include <QWaylandCompositor>
#include <QWaylandOutput>
#include <QWaylandOutputMode>
#include <QWaylandQuickOutput>
#include <QWaylandSurface>
#include <QWaylandXdgShell>
#include <QtMath>

Q_LOGGING_CATEGORY(lcSurface, "basecamp.surface")

namespace {

constexpr const char* kAppIdPrefix = "logos-view-";

SurfaceCompositor* g_instance = nullptr;

QString randomHex(int bytes)
{
    QByteArray raw(bytes, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(raw.data()),
                                          bytes / int(sizeof(quint32)));
    return QString::fromLatin1(raw.toHex());
}

// wl_display_add_socket() creates the socket under XDG_RUNTIME_DIR and Qt
// turns a failure there into qFatal. Check first; a host without a runtime
// dir gets `transport: none`, not a crash.
bool runtimeDirUsable()
{
    const QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (dir.isEmpty()) return false;
    const QFileInfo info(dir);
    return info.isDir() && info.isWritable();
}

} // namespace

SurfaceCompositor* SurfaceCompositor::instance()
{
    if (!g_instance) {
        if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
            qCWarning(lcSurface) << "SurfaceCompositor needs a QGuiApplication";
            return nullptr;
        }
        g_instance = new SurfaceCompositor(QCoreApplication::instance());
    }
    return g_instance;
}

SurfaceCompositor* SurfaceCompositor::existing()
{
    return g_instance;
}

SurfaceCompositor::SurfaceCompositor(QObject* parent)
    : QObject(parent)
{
    if (!runtimeDirUsable()) {
        qCWarning(lcSurface) << "XDG_RUNTIME_DIR is unset or not writable;"
                             << "view surfaces will not be composited";
        return;
    }

    m_compositor = new QWaylandCompositor(this);
    m_compositor->setSocketName(
        QByteArrayLiteral("logos-basecamp-")
        + QByteArray::number(QCoreApplication::applicationPid()) + '-'
        + randomHex(4).toLatin1());

    // xdg-shell only. No wl_shell, no layer shell, no decorations: the host
    // decides where a surface goes and how big it is.
    m_xdgShell = new QWaylandXdgShell(m_compositor);
    connect(m_xdgShell, &QWaylandXdgShell::toplevelCreated,
            this, &SurfaceCompositor::onToplevelCreated);

    // Qt's Wayland client waits for a wl_output before it maps anything, and a
    // module's window is created before any host item has asked for an output
    // for its own window. This headless output is what the client sees until
    // its surface enters the window output ensureOutput() creates.
    m_headlessOutput = new QWaylandOutput(m_compositor, nullptr);
    QWaylandOutputMode mode(QSize(1920, 1080), 60000);
    m_headlessOutput->addMode(mode, true);
    m_headlessOutput->setCurrentMode(mode);
    // Its scale is the primary screen's, so a client's very first buffer is
    // already at the resolution the common single-DPR host will show it at;
    // the per-window output it enters later corrects the rest.
    if (QScreen* primary = QGuiApplication::primaryScreen())
        m_headlessOutput->setScaleFactor(qMax(1, qCeil(primary->devicePixelRatio())));
    m_compositor->setDefaultOutput(m_headlessOutput);

    // The pid gate. There is no connection-level signal in Qt 6.9; a client's
    // first wl_compositor.create_surface is the earliest the host sees it.
    connect(m_compositor, &QWaylandCompositor::surfaceRequested,
            this, &SurfaceCompositor::onSurfaceRequested);

    m_compositor->create();
    m_available = m_compositor->isCreated();
    if (m_available)
        qCInfo(lcSurface) << "nested compositor on" << m_compositor->socketName();
    else
        qCWarning(lcSurface) << "nested compositor failed to start";
}

SurfaceCompositor::~SurfaceCompositor()
{
    if (g_instance == this) g_instance = nullptr;
}

bool SurfaceCompositor::isAvailable() const
{
    return m_available;
}

QString SurfaceCompositor::socketName() const
{
    return m_available ? QString::fromLatin1(m_compositor->socketName()) : QString();
}

void SurfaceCompositor::expect(const QString& module, const QString& token, qint64 pid)
{
    if (token.isEmpty() || pid <= 0) return;
    // One token per module: a reload of the same module replaces its entry.
    if (m_tokenByModule.contains(module))
        forget(m_tokenByModule.value(module));
    m_expected.insert(token, Expected{module, token, pid});
    m_tokenByModule.insert(module, token);
}

void SurfaceCompositor::forget(const QString& token)
{
    const Expected e = m_expected.take(token);
    if (!e.module.isEmpty() && m_tokenByModule.value(e.module) == token)
        m_tokenByModule.remove(e.module);
    // Drop the binding without a surfaceLost: the module is being unloaded
    // and its item is going away with it.
    m_bound.remove(token);
}

bool SurfaceCompositor::isExpected(const QString& token) const
{
    return m_expected.contains(token);
}

QString SurfaceCompositor::tokenFor(const QString& module) const
{
    return m_tokenByModule.value(module);
}

QWaylandXdgSurface* SurfaceCompositor::boundSurface(const QString& token) const
{
    return m_bound.value(token).xdgSurface.data();
}

QWaylandXdgToplevel* SurfaceCompositor::boundToplevel(const QString& token) const
{
    return m_bound.value(token).toplevel.data();
}

int SurfaceCompositor::scaleFor(QQuickWindow* window) const
{
    // wl_output scale is integral. A fractional DPR rounds up so the client
    // renders at least at physical resolution; the item then draws the buffer
    // into its own rect, which for a fractional DPR is a downscale.
    return qMax(1, qCeil(window->devicePixelRatio()));
}

void SurfaceCompositor::ensureOutput(QQuickWindow* window)
{
    if (!m_available || !window || m_compositor->outputFor(window)) return;

    auto* output = new QWaylandQuickOutput(m_compositor, window);
    output->setSizeFollowsWindow(true);
    output->setScaleFactor(scaleFor(window));
    connect(window, &QQuickWindow::screenChanged, output, [this, output, window] {
        output->setScaleFactor(scaleFor(window));
    });
    // The window owns the items that view this output; by the time it is
    // destroyed they are gone, and the output has nothing left to serve.
    connect(window, &QObject::destroyed, output, [output] { output->deleteLater(); });
    qCDebug(lcSurface) << "output for window" << window << "scale" << output->scaleFactor();
}

QWaylandOutput* SurfaceCompositor::outputFor(QQuickWindow* window) const
{
    return m_available && window ? m_compositor->outputFor(window) : nullptr;
}

void SurfaceCompositor::onSurfaceRequested(QWaylandClient* client, uint, int)
{
    if (!client || m_refusedClients.contains(client)) return;
    const qint64 pid = client->processId();
    for (auto it = m_expected.cbegin(); it != m_expected.cend(); ++it)
        if (it->pid == pid) return;
    qCWarning(lcSurface) << "refusing connection from unexpected pid" << pid;
    refuse(client, QString());
}

void SurfaceCompositor::onToplevelCreated(QWaylandXdgToplevel* toplevel,
                                          QWaylandXdgSurface* xdgSurface)
{
    m_pending.insert(toplevel, xdgSurface);
    connect(toplevel, &QObject::destroyed, this,
            [this, toplevel] { m_pending.remove(toplevel); });

    // app_id is a separate request after get_toplevel; usually it is already
    // in the same batch, but admit on whichever comes first and make the
    // first committed buffer the deadline.
    tryAdmit(toplevel, xdgSurface, false);
    if (!m_pending.contains(toplevel)) return;

    connect(toplevel, &QWaylandXdgToplevel::appIdChanged, this,
            [this, toplevel, xdgSurface] { tryAdmit(toplevel, xdgSurface, false); });
    connect(xdgSurface->surface(), &QWaylandSurface::hasContentChanged, this,
            [this, toplevel, xdgSurface] {
                if (xdgSurface->surface()->hasContent())
                    tryAdmit(toplevel, xdgSurface, true);
            });
}

void SurfaceCompositor::tryAdmit(QWaylandXdgToplevel* toplevel,
                                 QWaylandXdgSurface* xdgSurface, bool final)
{
    if (!m_pending.contains(toplevel)) return;   // already decided
    QWaylandSurface* surface = xdgSurface->surface();
    if (m_refusedClients.contains(surface->client())) {
        // Everything the client sent in the batch before its deferred close
        // lands here; it has been counted once already.
        m_pending.remove(toplevel);
        return;
    }
    const QString appId = toplevel->appId();
    if (appId.isEmpty() && !final) return;       // not announced yet

    const qint64 pid = surface->client() ? surface->client()->processId() : 0;

    QString token;
    if (appId.startsWith(QLatin1String(kAppIdPrefix)))
        token = appId.mid(int(qstrlen(kAppIdPrefix)));

    const auto it = m_expected.constFind(token);
    const bool ok = !token.isEmpty() && it != m_expected.cend() && it->pid == pid
                    && !m_bound.contains(token);
    m_pending.remove(toplevel);
    if (!ok) {
        qCWarning(lcSurface) << "refusing toplevel app_id" << appId << "from pid" << pid;
        refuse(surface->client(), appId);
        return;
    }

    m_bound.insert(token, Bound{toplevel, xdgSurface});
    connect(toplevel, &QObject::destroyed, this, [this, token] { onBoundGone(token); });
    connect(surface, &QWaylandSurface::surfaceDestroyed, this,
            [this, token] { onBoundGone(token); });
    qCInfo(lcSurface) << "bound surface for" << it->module << "pid" << pid;
    emit surfaceBound(token);
}

void SurfaceCompositor::refuse(QWaylandClient* client, const QString& appId)
{
    ++m_refused;
    const qint64 pid = client ? client->processId() : 0;
    emit surfaceRefused(pid, appId);
    if (!client) return;
    m_refusedClients.insert(client);
    connect(client, &QObject::destroyed, this,
            [this, client] { m_refusedClients.remove(client); });
    // Not from inside the request that triggered this: libwayland is still
    // dispatching on that client's resources.
    QPointer<QWaylandClient> guard(client);
    QTimer::singleShot(0, this, [guard] {
        if (guard) guard->close();
    });
}

void SurfaceCompositor::onBoundGone(const QString& token)
{
    qCDebug(lcSurface) << "bound toplevel gone for token" << token.left(8);
    if (!m_bound.contains(token)) return;   // forget() ran first
    m_bound.remove(token);
    if (m_expected.contains(token)) {
        qCWarning(lcSurface) << "surface of" << m_expected.value(token).module << "lost";
        emit surfaceLost(token);
    }
}
