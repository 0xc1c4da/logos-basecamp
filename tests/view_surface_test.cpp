// srcdeps: surface/SurfaceCompositor.cpp surface/LogosSurfaceItem.cpp
// libdeps: Qt6::WaylandCompositor
//
// RFC-4 §Tests / maps architecture §16.6: the nested compositor and the
// Logos.Surface item, end to end with a REAL Wayland client. The client is
// this same binary re-run with --surface-client <token>: a QGuiApplication on
// the wayland QPA (software Quick backend, shm buffers) that opens one window
// filled with a colour and reports its logical size and every touch point on
// stdout. No fixture module and no `qml` tool needed, so the check runs in the
// dependency-free unit-test harness.
//
// The host runs under the offscreen QPA with QT_SCALE_FACTOR=2 and the software
// scene graph: a 400x300 logical item must produce an 800x600 client buffer.

#include "surface/LogosSurfaceItem.h"
#include "surface/SurfaceCompositor.h"

#include <QApplication>
#include <QGuiApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTouchEvent>
#include <QWaylandOutput>
#include <QWaylandSurface>
#include <QWaylandView>
#include <QWaylandXdgShell>
#include <QtTest>

#include <cstdio>

namespace {

// ---------------------------------------------------------------- client ---

int runClient(int argc, char** argv)
{
    // What ui-host does for a --surface-socket spawn (RFC-1/2): wayland QPA,
    // app_id from the token, no client-side decoration. The environment
    // (WAYLAND_DISPLAY, QT_QPA_PLATFORM, QT_QUICK_BACKEND) comes from the host.
    const QString token = QString::fromLatin1(argv[2]);
    QGuiApplication app(argc, argv);
    QGuiApplication::setDesktopFileName(QStringLiteral("logos-view-") + token);

    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    QQmlComponent component(view.engine());
    component.setData(R"(
        import QtQuick
        Rectangle {
            color: "#2255cc"
            onWidthChanged: console.log("SIZE", width, height)
            onHeightChanged: console.log("SIZE", width, height)
            MultiPointTouchArea {
                anchors.fill: parent
                onPressed: (points) => { for (const p of points) console.log("TOUCH", p.x, p.y) }
            }
            Component.onCompleted: console.log("SIZE", width, height)
        }
    )", QUrl(QStringLiteral("client.qml")));
    if (component.isError()) {
        for (const auto& e : component.errors()) qWarning() << e.toString();
        return 2;
    }
    auto* root = qobject_cast<QQuickItem*>(component.create());
    if (!root) return 2;
    view.setContent(QUrl(), &component, root);
    // Same "qml: KIND ..." shape as console.log, so the host parses one format.
    auto report = [&view] {
        fprintf(stderr, "qml: DPR %g\n", view.devicePixelRatio());
        fflush(stderr);
    };
    QObject::connect(&view, &QWindow::screenChanged, &view, report);
    QObject::connect(view.screen(), &QScreen::physicalDotsPerInchChanged, &view, report);
    view.show();
    report();
    return app.exec();
}

// ------------------------------------------------------------------ host ---

QString newToken()
{
    QString t;
    while (t.size() < 32) t += QString::number(QRandomGenerator::global()->generate(), 16);
    return t.left(32);
}

struct Client {
    QProcess process;
    QString token;
    qint64 pid = 0;         // captured at start: processId() is 0 once it exits
    QStringList lines;      // stdout lines, as "KIND a b"
    QStringList raw;

    void start(const QString& socket, const QString& withToken)
    {
        token = withToken;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland"));
        env.insert(QStringLiteral("WAYLAND_DISPLAY"), socket);
        env.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
        env.insert(QStringLiteral("QT_WAYLAND_DISABLE_WINDOWDECORATION"), QStringLiteral("1"));
        // A Qt built with journald logs there when stderr is not a tty.
        env.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
        env.insert(QStringLiteral("QT_LOGGING_RULES"), QStringLiteral("qml.debug=true"));
        env.remove(QStringLiteral("QT_SCALE_FACTOR"));
        env.remove(QStringLiteral("DISPLAY"));
        process.setProcessEnvironment(env);
        process.setProcessChannelMode(QProcess::MergedChannels);
        QObject::connect(&process, &QProcess::readyRead, &process, [this] {
            while (process.canReadLine()) {
                const QString line = QString::fromUtf8(process.readLine()).trimmed();
                raw << line;
                // console.log from QML arrives as "qml: ..."
                const int at = line.indexOf(QLatin1String("qml: "));
                if (at >= 0) lines << line.mid(at + 5);
            }
        });
        process.start(QCoreApplication::applicationFilePath(),
                      {QStringLiteral("--surface-client"), token});
        process.waitForStarted(5000);
        pid = process.processId();
    }

    bool has(const QString& prefix) const
    {
        for (const QString& l : lines)
            if (l.startsWith(prefix)) return true;
        return false;
    }
    QString last(const QString& kind) const
    {
        for (int i = lines.size() - 1; i >= 0; --i)
            if (lines[i].startsWith(kind + QLatin1Char(' '))) return lines[i];
        return QString();
    }
    void stop()
    {
        if (process.state() == QProcess::NotRunning) return;
        process.terminate();
        if (!process.waitForFinished(3000)) { process.kill(); process.waitForFinished(1000); }
    }
};

const char* kHostQml = R"(
    import QtQuick
    import Logos.Surface
    Item {
        width: 400; height: 300
        Surface {
            id: surface
            objectName: "surface"
            anchors.fill: parent
            module: "fixture"
            session: "test"
            property var errors: []
            onError: (code) => errors.push(code)
        }
    }
)";

} // namespace

class ViewSurfaceTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_runtimeDir;
    SurfaceCompositor* m_compositor = nullptr;

    // A sandboxed-looking QQuickWidget: engine bound to module `boundAs`.
    QQuickWidget* makeHost(const QString& boundAs)
    {
        auto* widget = new QQuickWidget;
        widget->setResizeMode(QQuickWidget::SizeRootObjectToView);
        LogosSurfaceItem::bindEngine(widget->engine(), boundAs);
        auto* component = new QQmlComponent(widget->engine(), widget);
        component->setData(kHostQml, QUrl(QStringLiteral("host.qml")));
        if (component->isError())
            for (const auto& e : component->errors()) qWarning() << e.toString();
        widget->setContent(QUrl(), component, component->create());
        widget->resize(400, 300);
        widget->show();
        return widget;
    }

    static LogosSurfaceItem* surfaceOf(QQuickWidget* w)
    {
        return w->rootObject()->findChild<LogosSurfaceItem*>(QStringLiteral("surface"));
    }

    static void touchAt(QQuickWidget* w, const QPointF& local)
    {
        static QPointingDevice device(QStringLiteral("test touch"), 1,
                                      QInputDevice::DeviceType::TouchScreen,
                                      QPointingDevice::PointerType::Finger,
                                      QInputDevice::Capability::Position, 10, 0);
        // Through the platform input path (QTest::touchEvent), so the
        // point reaches the QQuickWidget the way a real touchscreen's would.
        QTest::touchEvent(w, &device).press(1, local.toPoint(), w);
        QTest::touchEvent(w, &device).release(1, local.toPoint(), w);
        // QWaylandCompositor flushes clients when the event dispatcher is
        // about to block; QTest::qWait never blocks it, so flush here.
        SurfaceCompositor::instance()->compositor()->processWaylandEvents();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_runtimeDir.isValid());
        qputenv("XDG_RUNTIME_DIR", m_runtimeDir.path().toUtf8());
        qmlRegisterType<LogosSurfaceItem>("Logos.Surface", 1, 0, "Surface");
        m_compositor = SurfaceCompositor::instance();
        QVERIFY(m_compositor);
        QVERIFY2(m_compositor->isAvailable(), "nested compositor did not come up");
        QVERIFY(m_compositor->socketName().startsWith(QStringLiteral("logos-basecamp-")));
        QCOMPARE(SurfaceCompositor::existing(), m_compositor);
    }

    // The happy path: bound within 2 s, sized by the host, DPR 2 buffer,
    // touch delivered, shm transport under offscreen, nothing after forget().
    void boundSizedAndTouched()
    {
        // Production order (PluginLoader): spawn, expect, THEN the view's QML.
        Client client;
        const QString token = newToken();
        QSignalSpy bound(m_compositor, &SurfaceCompositor::surfaceBound);
        client.start(m_compositor->socketName(), token);
        QVERIFY(client.pid > 0);
        m_compositor->expect(QStringLiteral("fixture"), token, client.pid);
        QCOMPARE(m_compositor->tokenFor(QStringLiteral("fixture")), token);

        QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("fixture")));
        LogosSurfaceItem* item = surfaceOf(host.data());
        QVERIFY(item);
        QVERIFY(!item->isReady());

        QTRY_VERIFY_WITH_TIMEOUT(item->isReady(), 10000);
        QCOMPARE(bound.count(), 1);
        QCOMPARE(bound.first().first().toString(), token);
        QCOMPARE(item->viewId().section(QLatin1Char('/'), 0, 1), QStringLiteral("fixture/test"));
        // Known once the first buffer is current in the scene graph.
        QTRY_COMPARE_WITH_TIMEOUT(item->transport(), QStringLiteral("wayland-shm"), 5000);
        QCOMPARE(m_compositor->refusedCount(), 0);

        // Host-driven size: 400x300 logical at output scale 2 -> 800x600 buffer.
        QTRY_VERIFY2_WITH_TIMEOUT(client.last(QStringLiteral("SIZE")) == QStringLiteral("SIZE 400 300"),
                                  qPrintable(client.raw.join(QLatin1Char('|'))), 5000);
        QWaylandXdgSurface* xdg = m_compositor->boundSurface(token);
        QVERIFY(xdg);
        QTRY_VERIFY2_WITH_TIMEOUT(xdg->surface()->bufferSize() == QSize(800, 600),
                                  qPrintable(QStringLiteral("buffer %1x%2 scale %3; client: %4")
                                      .arg(xdg->surface()->bufferSize().width())
                                      .arg(xdg->surface()->bufferSize().height())
                                      .arg(xdg->surface()->bufferScale())
                                      .arg(client.raw.join(QLatin1Char('|')))), 5000);
        QCOMPARE(xdg->surface()->bufferScale(), 2);

        // Touch through the host scene reaches the client in its own
        // logical coordinates.
        QTest::qWait(100);
        touchAt(host.data(), QPointF(200, 150));
        QTRY_VERIFY_WITH_TIMEOUT(client.has(QStringLiteral("TOUCH")), 5000);
        const QStringList touch = client.last(QStringLiteral("TOUCH")).split(QLatin1Char(' '));
        QCOMPARE(touch.size(), 3);
        QVERIFY2(qAbs(touch[1].toDouble() - 200) <= 2, qPrintable(client.last(QStringLiteral("TOUCH"))));
        QVERIFY2(qAbs(touch[2].toDouble() - 150) <= 2, qPrintable(client.last(QStringLiteral("TOUCH"))));

        // Resize: a configure in logical pixels, a new buffer at scale.
        host->resize(200, 150);
        QTRY_COMPARE_WITH_TIMEOUT(client.last(QStringLiteral("SIZE")), QStringLiteral("SIZE 200 150"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(xdg->surface()->bufferSize(), QSize(400, 300), 5000);

        // Invisible item: off its output, so no frame callbacks (RFC-5 rule 1).
        item->setVisible(false);
        QTest::qWait(50);
        item->setVisible(true);
        QVERIFY(item->isReady());

        // Unload order (RFC-5 rule 4): forget, then stop. No lost surface, no
        // error on the item.
        QSignalSpy lost(m_compositor, &SurfaceCompositor::surfaceLost);
        m_compositor->forget(token);
        QVERIFY(!m_compositor->isExpected(token));
        QVERIFY(m_compositor->tokenFor(QStringLiteral("fixture")).isEmpty());
        client.stop();
        QTest::qWait(200);
        QCOMPARE(lost.count(), 0);
        QCOMPARE(item->property("errors").toList().size(), 0);
    }

    // A client that exits on its own: surfaceLost, and the item reports it.
    void clientExitIsReported()
    {
        Client client;
        const QString token = newToken();
        client.start(m_compositor->socketName(), token);
        m_compositor->expect(QStringLiteral("fixture"), token, client.pid);
        QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("fixture")));
        LogosSurfaceItem* item = surfaceOf(host.data());
        QTRY_VERIFY_WITH_TIMEOUT(item->isReady(), 10000);

        QSignalSpy lost(m_compositor, &SurfaceCompositor::surfaceLost);
        client.process.kill();
        client.process.waitForFinished(3000);
        QTRY_COMPARE_WITH_TIMEOUT(lost.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!item->isReady(), 5000);
        QCOMPARE(item->property("errors").toList(), QVariantList{QStringLiteral("clientExited")});
        QCOMPARE(item->transport(), QStringLiteral("none"));
        m_compositor->forget(token);
    }

    // Admission: an expected pid presenting the wrong app_id is refused and
    // its connection closed; the item never binds.
    void wrongAppIdIsRefused()
    {
        Client client;
        const QString expected = newToken();
        const QString presented = newToken();
        QSignalSpy refused(m_compositor, &SurfaceCompositor::surfaceRefused);
        const int before = m_compositor->refusedCount();
        client.start(m_compositor->socketName(), presented);
        m_compositor->expect(QStringLiteral("fixture"), expected, client.pid);
        QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("fixture")));
        LogosSurfaceItem* item = surfaceOf(host.data());

        QTRY_VERIFY_WITH_TIMEOUT(m_compositor->refusedCount() > before, 10000);
        QCOMPARE(refused.count(), 1);   // the toplevel, once; its pid passed the gate
        QCOMPARE(refused.last().at(0).toLongLong(), client.pid);
        QCOMPARE(refused.last().at(1).toString(), QStringLiteral("logos-view-") + presented);
        // Closed by the compositor: the client's connection drops and Qt's
        // wayland client exits.
        QTRY_VERIFY_WITH_TIMEOUT(client.process.state() == QProcess::NotRunning, 10000);
        QVERIFY(!item->isReady());
        QVERIFY(!m_compositor->boundSurface(expected));
        m_compositor->forget(expected);
        client.stop();
    }

    // Admission: a pid nobody expects is closed on its first surface request,
    // and the rest of that first batch (its toplevel) is not counted again.
    void unknownPidIsRefused()
    {
        Client client;
        QSignalSpy refused(m_compositor, &SurfaceCompositor::surfaceRefused);
        const int before = m_compositor->refusedCount();
        client.start(m_compositor->socketName(), newToken());
        QTRY_VERIFY_WITH_TIMEOUT(m_compositor->refusedCount() > before, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(client.process.state() == QProcess::NotRunning, 10000);
        QCOMPARE(refused.count(), 1);
        QCOMPARE(refused.first().at(0).toLongLong(), client.pid);
        QVERIFY(refused.first().at(1).toString().isEmpty());   // pid gate, no app_id
        client.stop();
    }

    // Sandbox invariants (RFC-3): a view may only name its own module, and a
    // module nobody spawned as gui has no token.
    void itemRefusesOtherModules()
    {
        {
            // Engine bound as "evil", QML asks for "fixture".
            QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("evil")));
            LogosSurfaceItem* item = surfaceOf(host.data());
            QTRY_COMPARE_WITH_TIMEOUT(item->property("errors").toList(),
                                      QVariantList{QStringLiteral("tokenMismatch")}, 2000);
            QCOMPARE(item->transport(), QStringLiteral("none"));
        }
        {
            // Right module, but no expectation for it.
            QVERIFY(m_compositor->tokenFor(QStringLiteral("fixture")).isEmpty());
            QScopedPointer<QQuickWidget> host(makeHost(QStringLiteral("fixture")));
            LogosSurfaceItem* item = surfaceOf(host.data());
            QTRY_COMPARE_WITH_TIMEOUT(item->property("errors").toList(),
                                      QVariantList{QStringLiteral("notGuiModule")}, 2000);
        }
    }

    // The host-only channel: nothing about tokens or module identity is
    // reachable from QML on the item.
    void itemExposesNoTokenToQml()
    {
        const QMetaObject* mo = &LogosSurfaceItem::staticMetaObject;
        QCOMPARE(mo->indexOfProperty("token"), -1);
        for (int i = 0; i < mo->methodCount(); ++i) {
            const QString name = QString::fromLatin1(mo->method(i).name());
            QVERIFY2(!name.contains(QStringLiteral("token"), Qt::CaseInsensitive)
                     && !name.contains(QStringLiteral("bindEngine")),
                     qPrintable(name));
        }
    }
};

int main(int argc, char** argv)
{
    if (argc >= 3 && qstrcmp(argv[1], "--surface-client") == 0)
        return runClient(argc, argv);

    // Host: offscreen at DPR 2 with the software scene graph (no GPU in CI).
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_SCALE_FACTOR", "2");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QApplication app(argc, argv);
    ViewSurfaceTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "view_surface_test.moc"
