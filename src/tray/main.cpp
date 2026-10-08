// lockstep-tray — a system-tray status indicator and repo manager.
//
// A thin GUI client of the daemon (same as the CLI): it polls `status` over the
// socket every few seconds, tints the "¿?" mark green/yellow/red — the ¿ for
// this machine, the ? for the other machine(s) — and notifies on state changes. "Add repo…" and "Remove repo" shell out to `lockstep
// add`/`remove` — the config-mutation logic lives there, not here.

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QTimer>

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <vector>

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

// The more severe of two healths (Unknown < Green < Yellow < Red).
Health worse(Health a, Health b) { return std::max(a, b); }

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

// Plain colored dot — fallback when the brand image is unavailable. Left half
// is this machine, right half the other machine(s), matching the mark.
QIcon dotIcon(Health local, Health remote) {
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(colorFor(local));
    p.drawPie(3, 3, 16, 16, 90 * 16, 180 * 16);
    p.setBrush(colorFor(remote));
    p.drawPie(3, 3, 16, 16, 270 * 16, 180 * 16);
    p.end();
    return QIcon(pm);
}

// The mark split into its two glyphs: the ¿ (left) and the ? (right). They
// interlock — their bounding boxes overlap — so a vertical cut won't do; instead
// each connected shape (the glyph bodies and their dots) goes to whichever side
// its centroid falls on. Computed once from the full-resolution art.
struct MarkHalves {
    QImage left, right;
    bool ok = false;
};

const MarkHalves& markHalves() {
    static MarkHalves halves = [] {
        MarkHalves h;
        QImage img = brandBase().toImage().convertToFormat(QImage::Format_ARGB32);
        if (img.isNull()) return h;
        const int W = img.width(), H = img.height();
        h.left = QImage(W, H, QImage::Format_ARGB32);
        h.right = QImage(W, H, QImage::Format_ARGB32);
        h.left.fill(Qt::transparent);
        h.right.fill(Qt::transparent);

        std::vector<char> seen(size_t(W) * H, 0);
        std::vector<QPoint> shape, stack;
        bool anyLeft = false, anyRight = false;
        for (int y0 = 0; y0 < H; ++y0) {
            for (int x0 = 0; x0 < W; ++x0) {
                if (seen[size_t(y0) * W + x0] || qAlpha(img.pixel(x0, y0)) == 0) continue;
                // Flood-fill one shape (4-connected, any non-transparent pixel).
                shape.clear();
                stack.assign(1, QPoint(x0, y0));
                seen[size_t(y0) * W + x0] = 1;
                long long sumX = 0;
                while (!stack.empty()) {
                    QPoint pt = stack.back();
                    stack.pop_back();
                    shape.push_back(pt);
                    sumX += pt.x();
                    const QPoint nbrs[] = {{pt.x() + 1, pt.y()}, {pt.x() - 1, pt.y()},
                                           {pt.x(), pt.y() + 1}, {pt.x(), pt.y() - 1}};
                    for (const QPoint& n : nbrs) {
                        if (n.x() < 0 || n.y() < 0 || n.x() >= W || n.y() >= H) continue;
                        char& s = seen[size_t(n.y()) * W + n.x()];
                        if (s || qAlpha(img.pixel(n)) == 0) continue;
                        s = 1;
                        stack.push_back(n);
                    }
                }
                bool isLeft = sumX < (long long)(W / 2) * (long long)shape.size();
                QImage& dst = isLeft ? h.left : h.right;
                (isLeft ? anyLeft : anyRight) = true;
                for (const QPoint& pt : shape) dst.setPixel(pt, img.pixel(pt));
            }
        }
        h.ok = anyLeft && anyRight;
        return h;
    }();
    return halves;
}

// `art` scaled to S×S with every opaque pixel recolored to `c`, keeping the
// shape (alpha).
QPixmap tinted(const QImage& art, const QColor& c, int S) {
    QPixmap pm = QPixmap::fromImage(
        art.scaled(S, S, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    QPainter p(&pm);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(pm.rect(), c);
    p.end();
    return pm;
}

// The brand mark tinted by health: the ¿ shows this machine, the ? the other
// machine(s), so each side reads at a glance even at menubar size. If the art
// doesn't split into two glyphs, the whole mark takes the worse of the two;
// if it didn't load at all, a split dot stands in.
QIcon trayIcon(Health local, Health remote) {
    const QPixmap& base = brandBase();
    if (base.isNull()) return dotIcon(local, remote);

    const int S = 44;  // rendered large; the menubar downscales it crisply
    const MarkHalves& halves = markHalves();
    if (!halves.ok) return QIcon(tinted(base.toImage(), colorFor(worse(local, remote)), S));

    QPixmap left = tinted(halves.left, colorFor(local), S);
    QPixmap right = tinted(halves.right, colorFor(remote), S);
    QPixmap canvas(left.size());
    canvas.fill(Qt::transparent);
    QPainter p(&canvas);
    p.drawPixmap(0, 0, left);
    p.drawPixmap(0, 0, right);
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

// True when a local repo's branch is behind its upstream — another machine
// pushed and this checkout hasn't pulled (as of the last fetch).
bool behindRemote(const json& r) {
    return r.value("has_upstream", false) && r.value("behind", 0) > 0;
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

    // A submenu titled `title` listing `lines` as read-only rows.
    void addDetails(QMenu* parent, const QString& title, const QStringList& lines) {
        QMenu* sub = parent->addMenu(title);
        for (const auto& line : lines) sub->addAction(line)->setEnabled(false);
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
            icon.setIcon(trayIcon(Health::Unknown, Health::Unknown));
            icon.setToolTip(QStringLiteral("lockstep — daemon not running"));
            addInfo(QStringLiteral("lockstep — daemon not running"));
            menu.addSeparator();
            menu.addAction(QStringLiteral("Refresh now"), [this] { refresh(); });
            menu.addAction(QStringLiteral("Quit"), [] { qApp->quit(); });
            last = Health::Unknown;
            return;
        }
        const json& st = *reply;

        // Which repos do we watch locally (by basename)? And this machine's own
        // health: yellow when a repo is behind its remote (another machine pushed;
        // pull before working here) or can't be read.
        std::set<std::string> localNames;
        bool localYellow = false;
        QString localDetail;
        for (const auto& r : st.value("repos", json::array())) {
            std::string path = r.value("path", "");
            if (!path.empty()) localNames.insert(lockstep::repo_name(path));
            QString name = QString::fromStdString(lockstep::repo_name(path));
            if (r.contains("error")) {
                localYellow = true;
                if (localDetail.isEmpty())
                    localDetail = name + " — " + QString::fromStdString(r.value("error", "?"));
            } else if (behindRemote(r)) {
                localYellow = true;
                if (localDetail.isEmpty())
                    localDetail = name + " is " + describeRepo(r).section(", ", -1) +
                                  " — pull before working";
            }
        }
        Health local = localYellow ? Health::Yellow : Health::Green;

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
        Health remote = red ? Health::Red : (yellow ? Health::Yellow : Health::Green);
        Health h = worse(local, remote);
        if (detail.isEmpty() && local == h) detail = localDetail;

        // --- Build the menu ---
        // The menu is drawn by the desktop shell (GNOME's AppIndicator extension,
        // the macOS menubar), not by Qt, so row height and font can't be styled
        // from here. Keep it short instead: only repos that need attention get a
        // row; clean ones fold into a submenu, and each other machine is one row
        // with its repos in a submenu. Rows grow with problems, not with repos.
        // One side's news is enough for the headline; when both agree, keep it short.
        QString headline = local == remote
                               ? textFor(h)
                               : QStringLiteral("this machine: ") + textFor(local) +
                                     QStringLiteral(", others: ") + textFor(remote);
        addInfo(QStringLiteral("lockstep — ") + headline);
        menu.addSeparator();

        addInfo(QStringLiteral("This machine"));
        QStringList cleanHere;
        for (const auto& r : st.value("repos", json::array())) {
            std::string path = r.value("path", "");
            QString name = QString::fromStdString(lockstep::repo_name(path));
            bool behind = behindRemote(r);
            bool fine = r.value("clean", true) && !behind && !r.contains("error");
            QString line = name + QStringLiteral(" — ") + describeRepo(r) +
                           (behind ? QStringLiteral(" — pull before working") : QString());
            if (fine)
                cleanHere << mark(true) + line;
            else
                addInfo(QStringLiteral("  ") + mark(false) + line);
        }
        if (!cleanHere.isEmpty())
            addDetails(&menu, QStringLiteral("  ") + mark(true) +
                                  QStringLiteral("%1 clean").arg(cleanHere.size()),
                       cleanHere);

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
                QStringList all, pending;
                for (const auto& r : m.value("repos", json::array())) {
                    bool clean = r.value("clean", true);
                    QString line = QString::fromStdString(r.value("name", "?")) +
                                   QStringLiteral(" — ") + describeRepo(r);
                    all << mark(clean) + line;
                    if (!clean) pending << line;
                }
                QString summary = pending.isEmpty()
                                      ? QStringLiteral("all clear")
                                      : QStringLiteral("%1 with pending work").arg(pending.size());
                addDetails(&menu,
                           QStringLiteral("  ") + mark(pending.isEmpty()) +
                               QString::fromStdString(m.value("machine", "?")) +
                               QStringLiteral(" — ") + summary + QStringLiteral(" (as of ") +
                               QString::fromStdString(m.value("as_of", "?")) + ")",
                           all);
                for (const auto& line : pending) addInfo(QStringLiteral("      ") + mark(false) + line);
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

        icon.setIcon(trayIcon(local, remote));
        icon.setToolTip(QStringLiteral("lockstep — ") + headline);
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
    tray.icon.setIcon(trayIcon(Health::Unknown, Health::Unknown));
    tray.icon.setContextMenu(&tray.menu);
    tray.icon.setToolTip(QStringLiteral("lockstep"));
    tray.icon.show();
    tray.refresh();

    QTimer* timer = new QTimer(&app);
    QObject::connect(timer, &QTimer::timeout, [] { tray.refresh(); });
    timer->start(5000);

    return app.exec();
}
