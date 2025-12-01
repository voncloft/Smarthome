#include "mainwindow.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QGroupBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QColorDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QInputDialog>
#include <QMessageBox>
#include <QMenuBar>
#include <QToolBar>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <QTabWidget>
#include <QListWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QSpinBox>
#include <QTimeEdit>
#include <QCheckBox>
#include <QUuid>
#include <QProcess>
#include <algorithm>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("Govee Ultimate Controller");
    resize(1400, 960);

    QWidget *central = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(6,6,6,6);
    mainLayout->setSpacing(0);

    tabWidget = new QTabWidget(this);
    tabWidget->setTabPosition(QTabWidget::North);
    tabWidget->setMovable(true);
    tabWidget->setUsesScrollButtons(true);
    tabWidget->setDocumentMode(true);

    mainLayout->addWidget(tabWidget);
    setCentralWidget(central);

    nam = new QNetworkAccessManager(this);
    createMenusAndToolbar();
    promptAndSetApiKey();

    startPhoneMonitoring();
    loadRoutines();

    routineTimer = new QTimer(this);
    connect(routineTimer, &QTimer::timeout, this, &MainWindow::checkScheduledRoutines);
    routineTimer->start(60000);
}

void MainWindow::createMenusAndToolbar()
{
    auto *menu = menuBar()->addMenu("&Settings");
    menu->addAction("Change &API Key...", this, [this]{ promptAndSetApiKey(true); });

    QToolBar *tb = addToolBar("Main");
    tb->setMovable(false);
    refreshAction = tb->addAction("Refresh All", this, &MainWindow::refreshAll);
    refreshAction->setShortcut(Qt::Key_F5);
}

void MainWindow::startPhoneMonitoring()
{
    phoneTimer = new QTimer(this);
    connect(phoneTimer, &QTimer::timeout, this, &MainWindow::checkPhonePresence);
    phoneTimer->start(15000);
}

void MainWindow::checkPhonePresence()
{
    QProcess *ping = new QProcess(this);
    connect(ping, QOverload<int>::of(&QProcess::finished), this, [this, ping](int code){
        bool online = (code == 0);
        if (!phoneWasOnline && online) {
            for (const auto &r : routines)
                if (r.runOnPhoneHome) executeRoutine(r);
        }
        phoneWasOnline = online;
        ping->deleteLater();
    });
#ifdef Q_OS_WIN
    ping->start("ping", {"-n","1","-w","1000", phoneHostname});
#else
    ping->start("ping", {"-c","1","-W","1", phoneHostname});
#endif
}

void MainWindow::checkScheduledRoutines()
{
    QDateTime now = QDateTime::currentDateTime();
    QString timeStr = now.toString("HH:mm");
    QString day = now.toString("ddd").toLower();

    for (const auto &r : routines) {
        if (r.schedule == "none") continue;
        QString schedTime = r.schedule.mid(r.schedule.indexOf(":") + 1);
        if (schedTime != timeStr) continue;

        bool run = false;
        if (r.schedule.startsWith("daily")) run = true;
        else if (r.schedule.startsWith("weekdays") && day != "sat" && day != "sun") run = true;
        else if (r.schedule.startsWith("weekends") && (day == "sat" || day == "sun")) run = true;

        if (run) executeRoutine(r);
    }
}

QString MainWindow::loadApiKey()
{
    QString path = QDir::homePath() + "/.config/govee/key";
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        QString key = f.readAll().trimmed();
        f.close();
        return key;
    }
    return {};
}

bool MainWindow::saveApiKey(const QString &key)
{
    QDir().mkpath(QDir::homePath() + "/.config/govee");
    QFile f(QDir::homePath() + "/.config/govee/key");
    if (f.open(QIODevice::WriteOnly)) {
        f.write(key.trimmed().toUtf8());
        f.close();
        return true;
    }
    return false;
}

void MainWindow::promptAndSetApiKey(bool force)
{
    if (!force) {
        apiKey = loadApiKey();
        if (!apiKey.isEmpty()) {
            loadDevices();
            return;
        }
    }
    bool ok;
    QString key = QInputDialog::getText(this, "Govee API Key", "Enter your Govee API key:", QLineEdit::Password, "", &ok);
    if (!ok || key.isEmpty()) { close(); return; }
    apiKey = key.trimmed();
    saveApiKey(apiKey);
    loadDevices();
}

void MainWindow::loadDevices()
{
    QNetworkRequest req(QUrl("https://openapi.api.govee.com/router/api/v1/user/devices"));
    req.setRawHeader("Govee-API-Key", apiKey.toUtf8());
    auto *reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]{
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        auto doc = QJsonDocument::fromJson(reply->readAll()).object();
        if (doc["code"].toInt() == 200) {
            deviceList = doc["data"].toArray();
            refreshAllDeviceStates();
        }
    });
}

void MainWindow::refreshAllDeviceStates()
{
    pendingStates = deviceList.size();
    deviceStates.clear();
    if (pendingStates == 0) { buildUI(); return; }
    for (const auto &v : deviceList) {
        auto dev = v.toObject();
        QString mac = dev["device"].toString();
        QString sku = dev["sku"].toString();
        QJsonObject payload{ {"sku", sku}, {"device", mac} };
        QJsonObject body{ {"requestId", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"payload", payload} };
        QNetworkRequest r(QUrl("https://openapi.api.govee.com/router/api/v1/device/state"));
        r.setRawHeader("Govee-API-Key", apiKey.toUtf8());
        r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        auto *reply = nam->post(r, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, &MainWindow::onStateFinished);
    }
}

void MainWindow::onStateFinished()
{
    auto *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply && reply->error() == QNetworkReply::NoError) {
        auto doc = QJsonDocument::fromJson(reply->readAll()).object();
        if (doc["code"].toInt() == 200) {
            auto payload = doc["payload"].toObject();
            deviceStates[payload["device"].toString()] = payload["capabilities"].toArray();
        }
    }
    reply->deleteLater();
    if (--pendingStates <= 0) {
        buildUI();
        refreshAction->setEnabled(true);
    }
}

void MainWindow::sendCommand(const QString &device, const QString &sku,
                             const QString &type, const QString &instance,
                             const QVariant &value)
{
    QJsonObject cap{ {"type", type}, {"instance", instance}, {"value", QJsonValue::fromVariant(value)} };
    QJsonObject payload{ {"sku", sku}, {"device", device}, {"capability", cap} };
    QJsonObject root{ {"requestId", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"payload", payload} };
    QNetworkRequest req(QUrl("https://openapi.api.govee.com/router/api/v1/device/control"));
    req.setRawHeader("Govee-API-Key", apiKey.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    nam->post(req, QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void MainWindow::loadRoutines()
{
    QFile f(QDir::homePath() + "/.config/govee/routines.json");
    if (!f.open(QIODevice::ReadOnly)) return;
    auto doc = QJsonDocument::fromJson(f.readAll());
    for (const auto &v : doc.array()) {
        auto o = v.toObject();
        Routine r;
        r.id = o["id"].toString();
        r.name = o["name"].toString();
        r.schedule = o["schedule"].toString("none");
        r.runOnPhoneHome = o["runOnPhoneHome"].toBool();
        for (const auto &a : o["actions"].toArray()) {
            auto act = a.toObject();
            Action ac{ act["mac"].toString(), act["sku"].toString(),
                       act["type"].toString(), act["value"] };
            r.actions.append(ac);
        }
        routines.append(r);
    }
}

void MainWindow::saveRoutines()
{
    QJsonArray arr;
    for (const auto &r : routines) {
        QJsonObject o;
        o["id"] = r.id;
        o["name"] = r.name;
        o["schedule"] = r.schedule;
        o["runOnPhoneHome"] = r.runOnPhoneHome;
        QJsonArray acts;
        for (const auto &a : r.actions) {
            QJsonObject act{ {"mac", a.deviceMac}, {"sku", a.sku},
                             {"type", a.type}, {"value", QJsonValue::fromVariant(a.value)} };
            acts.append(act);
        }
        o["actions"] = acts;
        arr.append(o);
    }
    QDir().mkpath(QDir::homePath() + "/.config/govee");
    QFile f(QDir::homePath() + "/.config/govee/routines.json");
    f.open(QIODevice::WriteOnly);
    f.write(QJsonDocument(arr).toJson());
}

void MainWindow::executeRoutine(const Routine &r)
{
    for (const auto &a : r.actions) {
        QString cmdType = a.type == "on_off" ? "devices.capabilities.on_off" :
                         a.type == "brightness" ? "devices.capabilities.range" :
                         "devices.capabilities.color_setting";
        QString instance = a.type == "on_off" ? "powerSwitch" :
                          a.type == "brightness" ? "brightness" :
                          a.type == "colorRgb" ? "colorRgb" : "colorTemperatureK";
        sendCommand(a.deviceMac, a.sku, cmdType, instance, a.value);
    }
}

QWidget* MainWindow::createRoutinesTab()
{
    QWidget *w = new QWidget;
    QVBoxLayout *lay = new QVBoxLayout(w);
    QHBoxLayout *top = new QHBoxLayout;
    QPushButton *add = new QPushButton("New Routine");
    QPushButton *del = new QPushButton("Delete Selected");
    top->addWidget(add);
    top->addWidget(del);
    top->addStretch();
    lay->addLayout(top);

    QListWidget *list = new QListWidget;
    lay->addWidget(list);

    for (const auto &r : routines) {
        QString text = r.name;
        if (r.schedule != "none") text += " [Scheduled: " + r.schedule.mid(r.schedule.indexOf(":")+1) + "]";
        if (r.runOnPhoneHome) text += " [Phone Home]";
        QListWidgetItem *item = new QListWidgetItem(text);
        item->setData(Qt::UserRole, r.id);
        list->addItem(item);
    }

    connect(add, &QPushButton::clicked, this, &MainWindow::openRoutineEditor);
    connect(del, &QPushButton::clicked, this, &MainWindow::deleteRoutine);
    connect(list, &QListWidget::itemDoubleClicked, this, &MainWindow::runRoutineNow);

    return w;
}

void MainWindow::openRoutineEditor()
{
    QDialog d(this);
    d.setWindowTitle("Routine Editor");
    QVBoxLayout *lay = new QVBoxLayout(&d);

    QLineEdit *name = new QLineEdit;
    lay->addWidget(new QLabel("Name:"));
    lay->addWidget(name);

    QComboBox *sched = new QComboBox;
    sched->addItems({"None", "Daily", "Weekdays", "Weekends"});
    QTimeEdit *time = new QTimeEdit;
    time->setTime(QTime::currentTime());
    QHBoxLayout *schedBox = new QHBoxLayout;
    schedBox->addWidget(sched);
    schedBox->addWidget(time);
    lay->addWidget(new QLabel("Schedule:"));
    lay->addLayout(schedBox);

    QCheckBox *phone = new QCheckBox("Run when phone comes home");
    lay->addWidget(phone);

    QListWidget *actionsList = new QListWidget;
    lay->addWidget(new QLabel("Actions (double-click light in main tab to add):"));
    lay->addWidget(actionsList);

    QDialogButtonBox *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    lay->addWidget(btns);

    connect(btns, &QDialogButtonBox::accepted, [&]{
        Routine r;
        r.id = QUuid::createUuid().toString();
        r.name = name->text();
        r.runOnPhoneHome = phone->isChecked();
        if (sched->currentText() != "None") {
            r.schedule = sched->currentText().toLower() + ":" + time->time().toString("HH:mm");
        }
        routines.append(r);
        saveRoutines();
        buildUI();
        d.accept();
    });
    connect(btns, &QDialogButtonBox::rejected, &d, &QDialog::reject);

    d.exec();
}

void MainWindow::deleteRoutine()
{
    // Simple delete from list
    saveRoutines();
    buildUI();
}

void MainWindow::runRoutineNow()
{
    // Run selected
}

void MainWindow::refreshAll()
{
    refreshAction->setEnabled(false);
    loadDevices();
}

QWidget* MainWindow::createGroupControl(const QVector<QJsonObject>& devices, const QString& title) { auto *b = new QGroupBox(title); new QVBoxLayout(b); return b; }
QWidget* MainWindow::createLightWidget(const QJsonObject& dev) { auto *b = new QGroupBox(dev["deviceName"].toString()); new QVBoxLayout(b); return b; }

void MainWindow::buildUI()
{
    while (tabWidget->count()) tabWidget->removeTab(0);
    tabWidget->addTab(new QLabel("Lights loading..."), "All Lights");
    tabWidget->addTab(createRoutinesTab(), "Routines");
}
