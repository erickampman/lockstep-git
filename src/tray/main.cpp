// lockstep-tray — a system-tray status indicator and repo manager.
//
// A thin GUI client of the daemon (same as the CLI): it polls `status` over the
// socket every few seconds, colors a tray dot green/yellow/red, and notifies on
// state changes. "Add repo…" and "Remove repo" shell out to `lockstep
// add`/`remove` — the config-mutation logic lives there, not here.

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QTimer>

#include <optional>
#include <set>
#include <string>

#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#endif

#include <nlohmann/json.hpp>

#include "config.h"  // lockstep::repo_name
#include "ipc.h"
#include "paths.h"

using nlohmann::json;

namespace {

enum class Health { Unknown, Green, Yellow, Red };

QColor colorFor(Health h) {
    switch (h) {
        case Health::Green:  return QColor(0x2e, 0xa0, 0x43);
        case Health::Yellow: return QColor(0xd2, 0x9a, 0x00);
        case Health::Red:    return QColor(0xc0, 0x39, 0x2b);
        default:             return QColor(0x9e, 0x9e, 0x9e);
    }
}

QString textFor(Health h) {
    switch (h) {
        case Health::Green:  return QStringLiteral("all clear");
        case Health::Yellow: return QStringLiteral("heads up");
        case Health::Red:    return QStringLiteral("blocked");
        default:             return QStringLiteral("daemon not running");
    }
}

// The brand mark, loaded once from the compiled-in resource.
const QPixmap& brandBase() {
    static QPixmap pm(QStringLiteral(":/lockstep.png"));
    return pm;
}

// Plain colored dot — fallback when the brand image is unavailable.
QIcon dotIcon(Health h) {
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(colorFor(h));
    p.drawEllipse(3, 3, 16, 16);
    p.end();
    return QIcon(pm);
}

// The brand mark with a status-colored dot badged into the bottom-right corner
// (white-ringed for contrast against the red artwork). Falls back to the plain
// dot if the resource didn't load.
QIcon trayIcon(Health h) {
    const QPixmap& base = brandBase();
    if (base.isNull()) return dotIcon(h);

    const int S = 44;  // rendered large; the menubar downscales it crisply
    QPixmap canvas(S, S);
    canvas.fill(Qt::transparent);
    QPainter p(&canvas);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    QPixmap art = base.scaled(S, S, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    p.drawPixmap((S - art.width()) / 2, (S - art.height()) / 2, art);

    const qreal rr = S * 0.26;                 // badge radius
    const QPointF c(S - rr - 1.0, S - rr - 1.0);  // bottom-right
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawEllipse(c, rr, rr);                  // halo
    p.setBrush(colorFor(h));
    p.drawEllipse(c, rr * 0.72, rr * 0.72);    // status dot
    p.end();
    return QIcon(canvas);
}

// Locate the lockstep CLI: installed symlink, then beside us, then PATH.
QString lockstepBin() {
    QString local = QDir::homePath() + "/.local/bin/lockstep";
    if (QFileInfo::exists(local)) return local;
    QString beside = QCoreApplication::applicationDirPath() + "/lockstep";
    if (QFileInfo::exists(beside)) return beside;
    QString onPath = QStandardPaths::findExecutable(QStringLiteral("lockstep"));
    return onPath.isEmpty() ? QStringLiteral("lockstep") : onPath;
}

QString describeRepo(const json& r) {
    int dirty = r.value("dirty", 0);
    QString s = dirty == 0 ? QStringLiteral("clean") : QStringLiteral("%1 dirty").arg(dirty);
    if (r.value("has_upstream", false)) {
        int ahead = r.value("ahead", 0), behind = r.value("behind", 0);
        if (ahead == 0 && behind == 0) {
            s += QStringLiteral(", up to date");
        } else {
            if (ahead) s += QStringLiteral(", ahead %1").arg(ahead);
            if (behind) s += QStringLiteral(", behind %1").arg(behind);
        }
    } else {
        s += QStringLiteral(", no upstream");
    }
    return s;
}

QString mark(bool clean) {
    return clean ? QString::fromUtf8("✓ ") : QString::fromUtf8("● ");  // ✓ / ●
}

// macOS: make this a menubar-only "accessory" app — no Dock icon, no Cmd-Tab
// entry. Equivalent to Info.plist LSUIElement, but set at runtime so the plain
// binary needs no .app bundle. Must run after QApplication creates NSApplication.
void makeMenubarOnly() {
#ifdef __APPLE__
    using MsgCls = id (*)(Class, SEL);
    using MsgPolicy = void (*)(id, SEL, long);
    id app = reinterpret_cast<MsgCls>(objc_msgSend)(
        objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
    if (app)  // NSApplicationActivationPolicyAccessory == 1
        reinterpret_cast<MsgPolicy>(objc_msgSend)(
            app, sel_registerName("setActivationPolicy:"), 1);
#endif
}

struct Tray {
    QSystemTrayIcon icon;
    QMenu menu;
    Health last = Health::Unknown;

    void addInfo(const QString& text) {
        QAction* a = menu.addAction(text);
        a->setEnabled(false);
    }

    void runCli(const QStringList& args) {
        QProcess p;
        p.start(lockstepBin(), args);
        p.waitForFinished(20000);
        QString out = (QString::fromUtf8(p.readAllStandardOutput()) +
                       QString::fromUtf8(p.readAllStandardError()))
                          .trimmed();
        icon.showMessage(QStringLiteral("lockstep ") + args.join(' '),
                         out.isEmpty() ? QStringLiteral("done") : out,
                         QSystemTrayIcon::Information, 6000);
    }

    void addRepo() {
        QString dir = QFileDialog::getExistingDirectory(
            nullptr, QStringLiteral("Add a repo to watch"), QDir::homePath());
        if (dir.isEmpty()) return;
        runCli({QStringLiteral("add"), dir});
        refresh();
    }

    void removeRepo(const QString& name) {
        runCli({QStringLiteral("remove"), name});
        refresh();
    }

    // Notify when the situation worsens into yellow/red.
    void maybeNotify(Health h, const QString& detail) {
        if (h == last) return;
        if (h == Health::Red)
            icon.showMessage(QStringLiteral("lockstep — blocked"), detail,
                             QSystemTrayIcon::Critical, 8000);
        else if (h == Health::Yellow)
            icon.showMessage(QStringLiteral("lockstep — heads up"), detail,
                             QSystemTrayIcon::Warning, 6000);
    }

    void refresh() {
        auto reply = lockstep::ipc::request(lockstep::socket_path(),
                                            {{"cmd", "status"}});
        menu.clear();

        if (!reply) {
            icon.setIcon(trayIcon(Health::Unknown));
            icon.setToolTip(QStringLiteral("lockstep — daemon not running"));
            addInfo(QStringLiteral("lockstep — daemon not running"));
            menu.addSeparator();
            menu.addAction(QStringLiteral("Refresh now"), [this] { refresh(); });
            menu.addAction(QStringLiteral("Quit"), [] { qApp->quit(); });
            last = Health::Unknown;
            return;
        }
        const json& st = *reply;

        // Which repos do we watch locally (by basename)?
        std::set<std::string> localNames;
        for (const auto& r : st.value("repos", json::array())) {
            std::string path = r.value("path", "");
            if (!path.empty()) localNames.insert(lockstep::repo_name(path));
        }

        // Compute health from the cached cross-machine state.
        bool red = false, yellow = false;
        QString detail;
        const json sync = st.value("sync", json::object());
        if (!sync.value("configured", true)) yellow = true;
        if (sync.value("undecryptable", 0) > 0) yellow = true;
        if (!sync.value("status", std::string()).empty()) yellow = true;
        for (const auto& m : st.value("others", json::array())) {
            for (const auto& r : m.value("repos", json::array())) {
                if (r.value("clean", true)) continue;
                std::string name = r.value("name", "");
                QString line =
                    QString::fromStdString(m.value("machine", "?")) + " · " +
                    QString::fromStdString(name) + " — " + describeRepo(r);
                if (localNames.count(name)) {
                    red = true;
                    if (detail.isEmpty()) detail = line;
                } else {
                    yellow = true;
                }
            }
        }
        Health h = red ? Health::Red : (yellow ? Health::Yellow : Health::Green);

        // --- Build the menu ---
        addInfo(QStringLiteral("lockstep — ") + textFor(h));
        menu.addSeparator();

        addInfo(QStringLiteral("This machine"));
        for (const auto& r : st.value("repos", json::array())) {
            std::string path = r.value("path", "");
            QString name = QString::fromStdString(lockstep::repo_name(path));
            addInfo(QStringLiteral("  ") + mark(r.value("clean", true)) + name +
                    QStringLiteral(" — ") + describeRepo(r));
        }

        menu.addSeparator();
        auto others = st.value("others", json::array());
        if (!sync.value("configured", true)) {
            addInfo(QStringLiteral("Other machines — not available (") +
                    QString::fromStdString(sync.value("status", "not configured")) +
                    QStringLiteral(")"));
        } else if (others.empty()) {
            addInfo(QStringLiteral("Other machines — none have published yet"));
        } else {
            QString when = QString::fromStdString(sync.value("last_sync", ""));
            addInfo(QStringLiteral("Other machines") +
                    (when.isEmpty() ? QString() : QStringLiteral(" (synced ") + when + ")"));
            for (const auto& m : others) {
                addInfo(QStringLiteral("  ") + QString::fromStdString(m.value("machine", "?")) +
                        QStringLiteral(" (as of ") + QString::fromStdString(m.value("as_of", "?")) + ")");
                for (const auto& r : m.value("repos", json::array())) {
                    addInfo(QStringLiteral("    ") + mark(r.value("clean", true)) +
                            QString::fromStdString(r.value("name", "?")) +
                            QStringLiteral(" — ") + describeRepo(r));
                }
            }
        }

        menu.addSeparator();
        menu.addAction(QStringLiteral("Add repo…"), [this] { addRepo(); });
        if (!localNames.empty()) {
            QMenu* rm = menu.addMenu(QStringLiteral("Remove repo"));
            for (const auto& n : localNames) {
                QString name = QString::fromStdString(n);
                rm->addAction(name, [this, name] { removeRepo(name); });
            }
        }
        menu.addSeparator();
        menu.addAction(QStringLiteral("Refresh now"), [this] { refresh(); });
        menu.addAction(QStringLiteral("Quit"), [] { qApp->quit(); });

        icon.setIcon(trayIcon(h));
        icon.setToolTip(QStringLiteral("lockstep — ") + textFor(h));
        maybeNotify(h, detail.isEmpty() ? textFor(h) : detail);
        last = h;
    }
};

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);  // tray-only: no window closing should quit us
    makeMenubarOnly();                      // macOS: no Dock icon

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        qWarning("lockstep-tray: no system tray available on this session");
        return 1;
    }

    static Tray tray;  // constructed after QApplication; lives for the app's life
    tray.icon.setIcon(trayIcon(Health::Unknown));
    tray.icon.setContextMenu(&tray.menu);
    tray.icon.setToolTip(QStringLiteral("lockstep"));
    tray.icon.show();
    tray.refresh();

    QTimer* timer = new QTimer(&app);
    QObject::connect(timer, &QTimer::timeout, [] { tray.refresh(); });
    timer->start(5000);

    return app.exec();
}
