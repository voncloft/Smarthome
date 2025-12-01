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
#include <QTimeEdit>
#include <QCheckBox>
#include <QUuid>
#include <QProcess>
#include <QListWidgetItem>
#include <algorithm>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("Govee Ultimate — Routines + Phone + Schedule");
    resize(1440, 960);

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
    refreshAction->setIcon(QIcon::fromTheme("view-refresh"));
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
            qDebug() << "Phone is home → running phone-triggered routines";
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

        if (run) {
            qDebug() << "Scheduled routine triggered:" << r.name;
            executeRoutine(r);
        }
    }
}

QString MainWindow::loadApiKey()
{
    QFile f(QDir::homePath() + "/.config/govee/key");
    if (f.open(QIODevice::ReadOnly)) {
        QString k = f.readAll().trimmed();
        f.close();
        return k;
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
    routines.clear();
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
                       act["type"].toString(), act["value"],
                       act["display"].toString() };
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
                             {"type", a.type}, {"value", QJsonValue::fromVariant(a.value)},
                             {"display", a.displayName} };
            acts.append(act);
        }
        o["actions"] = acts;
        arr.append(o);
    }
    QDir().mkpath(QDir::homePath() + "/.config/govee");
    QFile f(QDir::homePath() + "/.config/govee/routines.json");
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(arr).toJson());
}

void MainWindow::executeRoutine(const Routine &r)
{
    qDebug() << "Executing routine:" << r.name;
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

QWidget* MainWindow::createGroupControl(const QVector<QJsonObject>& devices, const QString& title)
{
    int onCount = 0;
    for (const auto &d : devices) {
        QString mac = d["device"].toString();
        if (deviceStates.contains(mac)) {
            for (const auto &c : deviceStates[mac]) {
                auto cap = c.toObject();
                if (cap["instance"].toString() == "powerSwitch" && cap["state"].toObject()["value"].toInt() == 1)
                    ++onCount;
            }
        }
    }
    bool allOn = (!devices.isEmpty() && onCount == devices.size());

    auto *box = new QGroupBox(title);
    box->setStyleSheet("QGroupBox { font-weight: bold; font-size: 18pt; }");
    auto *layout = new QVBoxLayout(box);

    auto *gc = new QGroupBox("Group Control");
    auto *gcl = new QVBoxLayout(gc);

    QPushButton *powerBtn = new QPushButton(allOn ? "Turn Group Off" : "Turn Group On");
    powerBtn->setMinimumHeight(55);
    powerBtn->setCheckable(true);
    powerBtn->setChecked(allOn);

    QSlider *briSlider = new QSlider(Qt::Horizontal);
    briSlider->setRange(1,100);
    briSlider->setValue(100);

    QSlider *tempSlider = new QSlider(Qt::Horizontal);
    tempSlider->setRange(2000,9000);
    tempSlider->setValue(6500);

    QPushButton *colorBtn = new QPushButton("Pick Group Color");

    connect(powerBtn, &QPushButton::toggled, this, [=](bool on) mutable {
        powerBtn->setText(on ? "Turn Group Off" : "Turn Group On");
        for (const auto &d : devices)
            sendCommand(d["device"].toString(), d["sku"].toString(),
                        "devices.capabilities.on_off", "powerSwitch", on);
        QTimer::singleShot(800, this, &MainWindow::refreshAllDeviceStates);
    });

    connect(briSlider, &QSlider::sliderReleased, this, [=]{
        for (const auto &d : devices)
            sendCommand(d["device"].toString(), d["sku"].toString(),
                        "devices.capabilities.range", "brightness", briSlider->value());
        QTimer::singleShot(600, this, &MainWindow::refreshAllDeviceStates);
    });

    connect(tempSlider, &QSlider::sliderReleased, this, [=]{
        for (const auto &d : devices)
            sendCommand(d["device"].toString(), d["sku"].toString(),
                        "devices.capabilities.color_setting", "colorTemperatureK", tempSlider->value());
        QTimer::singleShot(600, this, &MainWindow::refreshAllDeviceStates);
    });

    connect(colorBtn, &QPushButton::clicked, this, [=]{
        QColor c = QColorDialog::getColor(Qt::white, this);
        if (c.isValid()) {
            int rgb = (c.red()<<16)|(c.green()<<8)|c.blue();
            for (const auto &d : devices)
                sendCommand(d["device"].toString(), d["sku"].toString(),
                            "devices.capabilities.color_setting", "colorRgb", rgb);
            QTimer::singleShot(800, this, &MainWindow::refreshAllDeviceStates);
        }
    });

    gcl->addWidget(powerBtn);
    gcl->addWidget(briSlider);
    gcl->addWidget(tempSlider);
    gcl->addWidget(colorBtn);
    layout->addWidget(gc);
    return box;
}

QWidget* MainWindow::createLightWidget(const QJsonObject& dev)
{
    QString name = dev["deviceName"].toString("Light");
    QString mac  = dev["device"].toString();
    QString sku  = dev["sku"].toString();

    bool isOn = false;
    int bri = 100, temp = 6500;
    QColor col = Qt::white;

    if (deviceStates.contains(mac)) {
        for (const auto &cv : deviceStates[mac]) {
            auto cap = cv.toObject();
            QString i = cap["instance"].toString();
            auto s = cap["state"].toObject();
            if (i == "powerSwitch") isOn = s["value"].toInt();
            if (i == "brightness") bri = s["value"].toInt();
            if (i == "colorTemperatureK") temp = s["value"].toInt();
            if (i == "colorRgb") {
                int rgb = s["value"].toInt();
                col = QColor((rgb>>16)&255, (rgb>>8)&255, rgb&255);
            }
        }
    }

    auto *bulb = new QGroupBox(name);
    auto *bl = new QVBoxLayout(bulb);

    auto *info = new QFormLayout;
    info->addRow("MAC:", new QLabel(mac));
    info->addRow("Model:", new QLabel(sku));
    bl->addLayout(info);

    QPushButton *pwr = new QPushButton(isOn ? "Turn Off" : "Turn On");
    pwr->setCheckable(true);
    pwr->setChecked(isOn);
    pwr->setMinimumHeight(50);
    connect(pwr, &QPushButton::toggled, this, [=](bool on){
        pwr->setText(on ? "Turn Off" : "Turn On");
        sendCommand(mac, sku, "devices.capabilities.on_off", "powerSwitch", on);
        QTimer::singleShot(600, this, &MainWindow::refreshAllDeviceStates);
    });
    bl->addWidget(pwr);

    QSlider *bs = new QSlider(Qt::Horizontal);
    bs->setRange(1,100); bs->setValue(bri); bs->setEnabled(isOn);
    QLabel *blbl = new QLabel("Brightness: " + QString::number(bri) + "%");
    connect(bs, &QSlider::valueChanged, this, [=](int v){
        blbl->setText("Brightness: " + QString::number(v) + "%");
        sendCommand(mac, sku, "devices.capabilities.range", "brightness", v);
    });
    bl->addWidget(blbl);
    bl->addWidget(bs);

    QPushButton *cb = new QPushButton("Pick Color");
    cb->setEnabled(isOn);
    connect(cb, &QPushButton::clicked, this, [=]() mutable {
        QColor c = QColorDialog::getColor(col, this);
        if (c.isValid()) {
            int rgb = (c.red()<<16)|(c.green()<<8)|c.blue();
            sendCommand(mac, sku, "devices.capabilities.color_setting", "colorRgb", rgb);
            QTimer::singleShot(600, this, &MainWindow::refreshAllDeviceStates);
        }
    });
    bl->addWidget(cb);

    QSlider *ts = new QSlider(Qt::Horizontal);
    ts->setRange(2000,9000); ts->setValue(temp); ts->setEnabled(isOn);
    QLabel *tlbl = new QLabel("Temp: " + QString::number(temp) + "K");
    connect(ts, &QSlider::valueChanged, this, [=](int v){
        tlbl->setText("Temp: " + QString::number(v) + "K");
        sendCommand(mac, sku, "devices.capabilities.color_setting", "colorTemperatureK", v);
    });
    bl->addWidget(tlbl);
    bl->addWidget(ts);

    bulb->setStyleSheet("QGroupBox { font-weight: bold; margin-top: 8px; }");

    connect(bulb, &QGroupBox::clicked, this, [this, mac, name]{
        addActionToCurrentRoutine(mac, name);
    });

    return bulb;
}

void MainWindow::addActionToCurrentRoutine(const QString &mac, const QString &name)
{
    if (currentEditingRoutineId.isEmpty()) return;

    Action a;
    a.deviceMac = mac;
    a.sku = deviceList.toVariantList().indexOf([mac](const QVariant &v){ return v.toJsonObject()["device"].toString() == mac; }) != -1 ?
            deviceList.toVariantList().at(deviceList.toVariantList().indexOf([mac](const QVariant &v){ return v.toJsonObject()["device"].toString() == mac; })).toJsonObject()["sku"].toString() : "";
    a.type = "on_off";
    a.value = true;
    a.displayName = name + " → Turn On";
    currentRoutineActions.append(a);
}

QWidget* MainWindow::createRoutinesTab()
{
    QWidget *w = new QWidget;
    QVBoxLayout *lay = new QVBoxLayout(w);

    QHBoxLayout *top = new QHBoxLayout;
    QPushButton *add = new QPushButton("New Routine");
    QPushButton *edit = new QPushButton("Edit Selected");
    QPushButton *del = new QPushButton("Delete Selected");
    top->addWidget(add);
    top->addWidget(edit);
    top->addWidget(del);
    top->addStretch();
    lay->addLayout(top);

    QListWidget *list = new QListWidget;
    lay->addWidget(list);

    for (const auto &r : routines) {
        QString text = r.name;
        if (r.schedule != "none") text += " [Schedule: " + r.schedule.mid(r.schedule.indexOf(":")+1) + "]";
        if (r.runOnPhoneHome) text += " [Phone Home]";
        QListWidgetItem *item = new QListWidgetItem(text);
        item->setData(Qt::UserRole, r.id);
        list->addItem(item);
    }

    connect(add, &QPushButton::clicked, this, [this]{ openRoutineEditor(); });
    connect(edit, &QPushButton::clicked, this, [this, list]{
        if (list->currentItem())
            openRoutineEditor(list->currentItem()->data(Qt::UserRole).toString());
    });
    connect(del, &QPushButton::clicked, this, &MainWindow::deleteRoutine);
    connect(list, &QListWidget::itemDoubleClicked, this, &MainWindow::runRoutineNow);

    return w;
}

void MainWindow::openRoutineEditor(const QString &routineId)
{
    currentEditingRoutineId = routineId.isEmpty() ? QUuid::createUuid().toString() : routineId;
    currentRoutineActions.clear();

    Routine *r = nullptr;
    for (auto &rt : routines)
        if (rt.id == currentEditingRoutineId) { r = &rt; break; }
    if (r) currentRoutineActions = r->actions;

    QDialog d(this);
    d.setWindowTitle(routineId.isEmpty() ? "New Routine" : "Edit Routine");
    QVBoxLayout *lay = new QVBoxLayout(&d);

    QLineEdit *nameEdit = new QLineEdit(r ? r->name : "My Routine");
    lay->addWidget(new QLabel("Name:"));
    lay->addWidget(nameEdit);

    QComboBox *schedType = new QComboBox;
    schedType->addItems({"None", "Daily", "Weekdays", "Weekends"});
    QTimeEdit *timeEdit = new QTimeEdit;
    timeEdit->setDisplayFormat("HH:mm");
    if (r && r->schedule != "none") {
        schedType->setCurrentText(r->schedule.left(r->schedule.indexOf(":")).capitalize());
        timeEdit->setTime(QTime::fromString(r->schedule.mid(r->schedule.indexOf(":")+1), "HH:mm"));
    }
    QHBoxLayout *schedBox = new QHBoxLayout;
    schedBox->addWidget(new QLabel("Schedule:"));
    schedBox->addWidget(schedType);
    schedBox->addWidget(timeEdit);
    lay->addLayout(schedBox);

    QCheckBox *phoneBox = new QCheckBox("Run when phone comes home");
    phoneBox->setChecked(r ? r->runOnPhoneHome : false);
    lay->addWidget(phoneBox);

    lay->addWidget(new QLabel("Actions (double-click any light in the main tabs to add):"));
    QListWidget *actionList = new QListWidget;
    for (const auto &a : currentRoutineActions)
        actionList->addItem(a.displayName);
    lay->addWidget(actionList);

    QDialogButtonBox *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    lay->addWidget(btns);

    connect(btns, &QDialogButtonBox::accepted, this, [=]{
        if (r) routines.removeAll(*r);
        Routine nr;
        nr.id = currentEditingRoutineId;
        nr.name = nameEdit->text();
        nr.actions = currentRoutineActions;
        nr.runOnPhoneHome = phoneBox->isChecked();
        if (schedType->currentText() != "None") {
            nr.schedule = schedType->currentText().toLower() + ":" + timeEdit->time().toString("HH:mm");
        }
        routines.append(nr);
        saveRoutines();
        buildUI();
        d.accept();
    });
    connect(btns, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    d.exec();
}

void MainWindow::deleteRoutine()
{
    auto *list = tabWidget->widget(tabWidget->indexOf(tabWidget->findChild<QListWidget*>()))
                 ->findChild<QListWidget*>();
    if (list && list->currentItem()) {
        QString id = list->currentItem()->data(Qt::UserRole).toString();
        routines.removeIf([id](const Routine &r){ return r.id == id; });
        saveRoutines();
        buildUI();
    }
}

void MainWindow::runRoutineNow(QListWidgetItem *item)
{
    if (!item) return;
    QString id = item->data(Qt::UserRole).toString();
    for (const auto &r : routines)
        if (r.id == id) { executeRoutine(r); break; }
}

void MainWindow::refreshAll()
{
    refreshAction->setEnabled(false);
    loadDevices();
}

void MainWindow::buildUI()
{
    while (tabWidget->count()) {
        QWidget *w = tabWidget->widget(0);
        tabWidget->removeTab(0);
        delete w;
    }

    QMap<QString, QVector<QJsonObject>> groups;
    for (const auto &v : deviceList) {
        QJsonObject dev = v.toObject();
        QString name = dev["deviceName"].toString("Light");
        QString key = name.split(' ', Qt::SkipEmptyParts).value(0, "Other");
        groups[key] << dev;
    }

    // All Lights
    {
        QScrollArea *sa = new QScrollArea;
        sa->setWidgetResizable(true);
        QWidget *page = new QWidget;
        QVBoxLayout *lay = new QVBoxLayout(page);
        lay->setContentsMargins(20,20,20,20);
        lay->setSpacing(25);

        QVector<QJsonObject> all;
        for (const auto &v : deviceList) all << v.toObject();
        lay->addWidget(createGroupControl(all, "Control ALL Lights"));

        for (const auto &v : deviceList)
            lay->addWidget(createLightWidget(v.toObject()));

        lay->addStretch();
        sa->setWidget(page);
        tabWidget->insertTab(0, sa, "All Lights (" + QString::number(deviceList.size()) + ")");
    }

    QStringList rooms = groups.keys();
    std::sort(rooms.begin(), rooms.end());
    for (const QString &room : rooms) {
        auto &devices = groups[room];
        QScrollArea *sa = new QScrollArea;
        sa->setWidgetResizable(true);
        QWidget *page = new QWidget;
        QVBoxLayout *lay = new QVBoxLayout(page);
        lay->setContentsMargins(20,20,20,20);
        lay->setSpacing(25);

        lay->addWidget(createGroupControl(devices, room + " Lights"));
        for (const auto &dev : devices)
            lay->addWidget(createLightWidget(dev));

        lay->addStretch();
        sa->setWidget(page);
        tabWidget->addTab(sa, room + " (" + QString::number(devices.size()) + ")");
    }

    tabWidget->addTab(createRoutinesTab(), "Routines");
}
