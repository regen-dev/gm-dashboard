/*
 * gm-viewer — Qt6 WebEngine viewer for GM Dashboard
 *
 * Frameless window with saved position/size.
 * Position is set BEFORE the window is shown — zero flicker.
 * JS<>C++ bridge via QWebChannel for drag, resize, close, minimize, geolocation.
 *
 * Usage: gm-viewer <html-file> [config-dir]
 */
#include <QApplication>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebChannel>
#include <QFile>
#include <QDir>
#include <QScreen>
#include <QWindow>
#include <QTimer>
#include <QMessageBox>
#include <QProcess>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLockFile>

#define POS_FILE "/dev/shm/gm-window-pos"
#define LOCK_FILE "/dev/shm/gm-viewer.lock"
#define DEFAULT_W 1400
#define DEFAULT_H 900

/* ── Bridge object exposed to JS as "wm" ──────────────────────── */

class WmBridge : public QObject {
    Q_OBJECT
public:
    WmBridge(QWebEngineView *view, const QString &cfgDir)
        : QObject(view), m_view(view), m_cfgDir(cfgDir) {}

public slots:
    void close() { saveGeometry(); m_view->close(); }

    void minimize() { m_view->showMinimized(); }

    void startDrag() {
        if (auto *w = m_view->windowHandle())
            w->startSystemMove();
    }

    void startResize(const QString &edge) {
        Qt::Edges edges;
        if (edge.contains('n')) edges |= Qt::TopEdge;
        if (edge.contains('s')) edges |= Qt::BottomEdge;
        if (edge.contains('w')) edges |= Qt::LeftEdge;
        if (edge.contains('e')) edges |= Qt::RightEdge;
        if (auto *w = m_view->windowHandle())
            w->startSystemResize(edges);
    }

    void toggleMaximize() {
        if (m_view->isMaximized())
            m_view->showNormal();
        else
            m_view->showMaximized();
    }

    void saveGeometry() {
        if (m_view->isMaximized()) return;
        auto g = m_view->geometry();
        FILE *f = fopen(POS_FILE, "w");
        if (f) {
            fprintf(f, "%d %d %d %d\n", g.x(), g.y(), g.width(), g.height());
            fclose(f);
        }
    }

    /* ── City search dialog: called from weather card JS ── */
    void requestLocation() {
        emit locationStatus("Opening city search...");

        auto *dlg = new QDialog(m_view);
        dlg->setWindowTitle("GM Dashboard — Set Location");
        dlg->setMinimumSize(450, 340);
        dlg->setAttribute(Qt::WA_DeleteOnClose);

        auto *layout = new QVBoxLayout(dlg);
        layout->setSpacing(10);

        auto *label = new QLabel("Search for your city:");
        label->setStyleSheet("font-weight: bold;");

        auto *input = new QLineEdit();
        input->setPlaceholderText("Start typing (e.g. Brasilia, London, Tokyo)...");
        input->setClearButtonEnabled(true);

        auto *list = new QListWidget();
        list->setAlternatingRowColors(true);

        auto *statusLabel = new QLabel("");
        statusLabel->setStyleSheet("color: gray; font-size: 11px;");

        auto *buttons = new QHBoxLayout();
        auto *okBtn = new QPushButton("Save");
        auto *cancelBtn = new QPushButton("Cancel");
        okBtn->setEnabled(false);
        okBtn->setDefault(true);
        buttons->addStretch();
        buttons->addWidget(okBtn);
        buttons->addWidget(cancelBtn);

        layout->addWidget(label);
        layout->addWidget(input);
        layout->addWidget(list);
        layout->addWidget(statusLabel);
        layout->addLayout(buttons);

        /* State */
        auto *nam = new QNetworkAccessManager(dlg);
        auto *debounce = new QTimer(dlg);
        debounce->setSingleShot(true);
        debounce->setInterval(400);

        struct CityData {
            QString name;     /* "Brasilia" */
            QString display;  /* "Brasilia, Distrito Federal, Brasil" */
        };
        auto *cities = new QList<CityData>();

        /* Debounced search as user types */
        QObject::connect(input, &QLineEdit::textEdited, dlg, [debounce](const QString &) {
            debounce->start();
        });

        QObject::connect(debounce, &QTimer::timeout, dlg,
            [input, nam, list, statusLabel, cities, okBtn]() {
            QString text = input->text().trimmed();
            if (text.length() < 2) {
                list->clear();
                cities->clear();
                return;
            }

            statusLabel->setText("Searching...");

            QUrl url("https://nominatim.openstreetmap.org/search");
            QUrlQuery q;
            q.addQueryItem("city", text);
            q.addQueryItem("format", "json");
            q.addQueryItem("limit", "8");
            q.addQueryItem("addressdetails", "1");
            q.addQueryItem("accept-language", "en");
            url.setQuery(q);

            QNetworkRequest req(url);
            req.setHeader(QNetworkRequest::UserAgentHeader, "gm-dashboard/1.0");

            auto *reply = nam->get(req);
            QObject::connect(reply, &QNetworkReply::finished, list,
                [reply, list, statusLabel, cities, okBtn]() {
                reply->deleteLater();

                if (reply->error() != QNetworkReply::NoError) {
                    statusLabel->setText("Search failed: " + reply->errorString());
                    return;
                }

                auto doc = QJsonDocument::fromJson(reply->readAll());
                list->clear();
                cities->clear();
                okBtn->setEnabled(false);

                for (auto val : doc.array()) {
                    auto obj = val.toObject();
                    auto addr = obj["address"].toObject();

                    QString name = obj["name"].toString();
                    QString city = addr["city"].toString();
                    if (city.isEmpty()) city = name;

                    QString state = addr["state"].toString();
                    QString country = addr["country"].toString();

                    /* Build display: "City, State, Country" */
                    QString display = city;
                    if (!state.isEmpty()) display += ", " + state;
                    if (!country.isEmpty()) display += ", " + country;

                    /* Avoid duplicates */
                    bool dup = false;
                    for (const auto &c : *cities)
                        if (c.display == display) { dup = true; break; }
                    if (dup) continue;

                    cities->append({city, display});
                    list->addItem(display);
                }

                int n = cities->size();
                statusLabel->setText(n > 0
                    ? QString("%1 result%2").arg(n).arg(n > 1 ? "s" : "")
                    : "No cities found");
            });
        });

        /* Selection enables OK button */
        QObject::connect(list, &QListWidget::currentRowChanged, dlg,
            [okBtn](int row) { okBtn->setEnabled(row >= 0); });

        /* Double-click = select + save */
        QObject::connect(list, &QListWidget::itemDoubleClicked, dlg,
            [okBtn, dlg](QListWidgetItem *) {
            okBtn->setEnabled(true);
            okBtn->click();
        });

        /* OK: save and reload */
        QObject::connect(okBtn, &QPushButton::clicked, dlg,
            [this, dlg, list, cities]() {
            int row = list->currentRow();
            if (row < 0 || row >= cities->size()) return;

            QString city = cities->at(row).name;
            writeWeatherConf(m_cfgDir + "/weather.conf", city);
            emit locationStatus("Saved: " + city + " — Reloading...");
            dlg->accept();
            QTimer::singleShot(500, this, [this]{ regenerateAndReload(); });
        });

        QObject::connect(cancelBtn, &QPushButton::clicked, dlg, [this, dlg]() {
            emit locationError("Cancelled");
            dlg->reject();
        });

        /* Clean up cities list on dialog close */
        QObject::connect(dlg, &QDialog::finished, dlg, [cities]() { delete cities; });

        input->setFocus();
        dlg->show();
    }

signals:
    void locationStatus(const QString &msg);
    void locationError(const QString &msg);

private:
    QWebEngineView *m_view;
    QString m_cfgDir;

    void writeWeatherConf(const QString &path, const QString &city) {
        QStringList lines;
        QFile f(path);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!f.atEnd()) {
                QString line = f.readLine().trimmed();
                if (!line.startsWith("CITY="))
                    lines.append(line);
            }
            f.close();
        }
        lines.append("CITY=" + city);

        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            for (const auto &l : lines)
                f.write((l + '\n').toUtf8());
            f.close();
        }
    }

    void regenerateAndReload() {
        QProcess proc;
        QString home = QDir::homePath();
        proc.setProgram(home + "/.local/bin/gm-dashboard");
        proc.setArguments({"--no-browser"});
        proc.start();
        proc.waitForFinished(30000);
        m_view->reload();
    }
};

/* ── Main ─────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: gm-viewer <html-file> [config-dir]\n");
        return 1;
    }

    qputenv("QT_QPA_PLATFORM", "xcb");

    QApplication app(argc, argv);
    app.setApplicationName("gm-dashboard");
    app.setDesktopFileName("gm-dashboard");

    /* ── Single instance lock ── */
    QLockFile lockFile(LOCK_FILE);
    if (!lockFile.tryLock(0)) {
        qint64 pid = 0;
        QString hostname, appname;
        lockFile.getLockInfo(&pid, &hostname, &appname);
        fprintf(stderr, "gm-viewer: already running (PID %lld). Activating existing instance.\n",
            (long long)pid);
        return 1;
    }

    /* Config dir from arg or default */
    QString cfgDir;
    if (argc >= 3)
        cfgDir = QString::fromLocal8Bit(argv[2]);
    else
        cfgDir = QDir::homePath() + "/.config/gm-dashboard";

    /* ── Frameless window ── */
    QWebEngineView view;
    view.setWindowFlags(Qt::FramelessWindowHint);
    view.setAttribute(Qt::WA_TranslucentBackground, false);

    /* ── Restore saved geometry BEFORE show ── */
    int wx = -1, wy = -1, ww = DEFAULT_W, wh = DEFAULT_H;
    FILE *pf = fopen(POS_FILE, "r");
    if (pf) {
        if (fscanf(pf, "%d %d %d %d", &wx, &wy, &ww, &wh) == 4 &&
            ww > 300 && wh > 200) {
            /* valid */
        } else {
            wx = -1;
        }
        fclose(pf);
    }

    if (wx >= 0 && wy >= 0) {
        view.setGeometry(wx, wy, ww, wh);
    } else {
        view.resize(ww, wh);
        QScreen *scr = app.primaryScreen();
        if (scr) {
            QRect sr = scr->availableGeometry();
            view.move((sr.width() - ww) / 2, (sr.height() - wh) / 2);
        }
    }

    /* ── WebChannel bridge ── */
    auto *channel = new QWebChannel(&view);
    auto *bridge = new WmBridge(&view, cfgDir);
    channel->registerObject("wm", bridge);
    view.page()->setWebChannel(channel);

    /* Allow local file access, mute all audio */
    view.settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    view.settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    view.settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, true);
    view.page()->setAudioMuted(true);

    /* ── Load HTML ── */
    QString path = QString::fromLocal8Bit(argv[1]);
    if (!path.startsWith('/'))
        path = QDir::currentPath() + '/' + path;
    view.load(QUrl::fromLocalFile(path));

    /* ── Show ── */
    view.show();

    /* ── Periodic geometry save ── */
    auto *timer = new QTimer(&view);
    QObject::connect(timer, &QTimer::timeout, bridge, &WmBridge::saveGeometry);
    timer->start(5000);
    QObject::connect(&app, &QApplication::aboutToQuit, bridge, &WmBridge::saveGeometry);

    return app.exec();
}

#include "gm-viewer.moc"
