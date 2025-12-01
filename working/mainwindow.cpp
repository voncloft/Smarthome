#include "mainwindow.h"
#include <QApplication>
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
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QTimeEdit>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QDir>
#include <QFile>
#include <QTabWidget>
#include <QTimer>
#include <QProcess>
#include <QDebug>
#include <QUuid>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("Govee Lights — Auto + Routines");
    resize(1450, 950);

    tabWidget = new QTabWidget(this);
    tabWidget->setTabPosition(QTabWidget::North);
    tabWidget->setDocumentMode(true);
    tabWidget->setMovable(true);
    setCentralWidget(tabWidget);

    nam = new QNetworkAccessManager(this);

    pingProcess = new QProcess(this);
    presenceTimer = new QTimer(this);
    connect(presenceTimer, &QTimer::timeout, this, &MainWindow::checkPhonePresence);
    presenceTimer->start(8000);

    routineTimer = new QTimer(this);
    connect(routineTimer, &QTimer::timeout, this, &MainWindow::checkRoutines);
    routineTimer->start(60000); // every minute

    if (!loadApiKey()) promptForApiKey();
    if (apiKey.isEmpty()) {
        QMessageBox::critical(this, "Error", "API key required");
        QApplication::quit();
        return;
    }

    loadDevices();
}

bool MainWindow::loadApiKey()
{
    QFile f(QDir::homePath() + "/.config/govee/api-key");
    if (f.open(QIODevice::ReadOnly)) {
        apiKey = f.readAll().trimmed();
        f.close();
        return true;
    }
    return false;
}

void MainWindow::promptForApiKey()
{
    bool ok;
    QString key = QInputDialog::getText(this, "Govee API Key", "Enter your API key:",
                                        QLineEdit::Password, "", &ok);
    if (ok && !key.isEmpty()) {
        apiKey = key.trimmed();
        QDir().mkpath(QDir::homePath() + "/.config/govee");
        QFile f(QDir::homePath() + "/.config/govee/api-key");
        if (f.open(QIODevice::WriteOnly)) f.write(apiKey.toUtf8());
    }
}

void MainWindow::loadDevices()
{
    QNetworkRequest req(QUrl("https://openapi.api.govee.com/router/api/v1/user/devices"));
    req.setRawHeader("Govee-API-Key", apiKey.toUtf8());

    QNetworkReply *reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]{
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (doc.object()["code"].toInt() != 200) return;

        deviceList = doc.object()["data"].toArray();
        pendingStates = deviceList.size();
        deviceStates.clear();

        if (pendingStates == 0) { buildUI(); return; }

        for (const QJsonValue &v : deviceList) {
            QJsonObject dev = v.toObject();
            QString mac = dev["device"].toString();
            QString sku = dev["sku"].toString();

            QJsonObject payloadObj{ {"sku", sku}, {"device", mac} };
            QJsonObject body{ {"requestId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                             {"payload", payloadObj} };

            QNetworkRequest r(QUrl("https://openapi.api.govee.com/router/api/v1/device/state"));
            r.setRawHeader("Govee-API-Key", apiKey.toUtf8());
            r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

            QNetworkReply *sr = nam->post(r, QJsonDocument(body).toJson());
            connect(sr, &QNetworkReply::finished, this, &MainWindow::onStateFinished);
        }
    });
}

void MainWindow::onStateFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    if (reply->error() == QNetworkReply::NoError) {
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (doc.object()["code"].toInt() == 200) {
            QJsonObject payload = doc.object()["payload"].toObject();
            QString mac = payload["device"].toString();
            deviceStates[mac] = payload["capabilities"].toArray();
        }
    }
    reply->deleteLater();
    if (--pendingStates <= 0) buildUI();
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
    nam->post(req, QJsonDocument(root).toJson());
}

QWidget* MainWindow::createLightWidget(const QJsonObject &dev)
{
    QString name = dev["deviceName"].toString("Light");
    QString mac  = dev["device"].toString();
    QString sku  = dev["sku"].toString();

    bool isOn = false;
    int bri = 100, temp = 4000;
    QColor col = Qt::white;

    if (deviceStates.contains(mac)) {
        for (const QJsonValue &cval : deviceStates[mac]) {
            QJsonObject cap = cval.toObject();
            QString i = cap["instance"].toString();
            QJsonObject s = cap["state"].toObject();
            if (i == "powerSwitch") isOn = s["value"].toInt();
            if (i == "brightness") bri = s["value"].toInt();
            if (i == "colorTemperatureK") temp = s["value"].toInt();
            if (i == "colorRgb") {
                int rgb = s["value"].toInt();
                col = QColor((rgb >> 16)&255, (rgb >> 8)&255, rgb&255);
            }
        }
    }

    QGroupBox *box = new QGroupBox(name);
    QVBoxLayout *l = new QVBoxLayout(box);

    QFormLayout *info = new QFormLayout;
    info->addRow("MAC:", new QLabel(mac.left(17)));
    info->addRow("Model:", new QLabel(sku));
    l->addLayout(info);

    QPushButton *power = new QPushButton(isOn ? "Turn Off" : "Turn On");
    power->setCheckable(true);
    power->setChecked(isOn);
    power->setMinimumHeight(50);
    connect(power, &QPushButton::toggled, [=](bool on){
        power->setText(on ? "Turn Off" : "Turn On");
        sendCommand(mac, sku, "devices.capabilities.on_off", "powerSwitch", on ? 1 : 0);
    });
    l->addWidget(power);

    QSlider *bs = new QSlider(Qt::Horizontal);
    bs->setRange(1,100); bs->setValue(bri); bs->setEnabled(isOn);
    QLabel *blbl = new QLabel(QString("Brightness: %1%").arg(bri));
    connect(bs, &QSlider::valueChanged, [=](int v){
        blbl->setText(QString("Brightness: %1%").arg(v));
        sendCommand(mac, sku, "devices.capabilities.range", "brightness", v);
    });
    l->addWidget(blbl); l->addWidget(bs);

    QPushButton *cb = new QPushButton("Pick Color");
    cb->setEnabled(isOn);
    connect(cb, &QPushButton::clicked, [=]() mutable {
        QColor c = QColorDialog::getColor(col, this);
        if (c.isValid())
            sendCommand(mac, sku, "devices.capabilities.color_setting", "colorRgb",
                        (c.red()<<16)|(c.green()<<8)|c.blue());
    });
    l->addWidget(cb);

    QSlider *ts = new QSlider(Qt::Horizontal);
    ts->setRange(2000,9000); ts->setValue(temp); ts->setEnabled(isOn);
    QLabel *tlbl = new QLabel(QString("Temp: %1K").arg(temp));
    connect(ts, &QSlider::valueChanged, [=](int v){
        tlbl->setText(QString("Temp: %1K").arg(v));
        sendCommand(mac, sku, "devices.capabilities.color_setting", "colorTemperatureK", v);
    });
    l->addWidget(tlbl); l->addWidget(ts);

    return box;
}

QWidget* MainWindow::createGroupControl(const QVector<QJsonObject> &devices, const QString &title)
{
    QGroupBox *box = new QGroupBox(title);
    QVBoxLayout *l = new QVBoxLayout(box);

    QPushButton *groupPower = new QPushButton("Turn Group Off");
    groupPower->setCheckable(true);
    groupPower->setMinimumHeight(55);
    connect(groupPower, &QPushButton::toggled, [=](bool on){
        groupPower->setText(on ? "Turn Group Off" : "Turn Group On");
        for (const auto &d : devices)
            sendCommand(d["device"].toString(), d["sku"].toString(),
                        "devices.capabilities.on_off", "powerSwitch", on ? 1 : 0);
    });
    l->addWidget(groupPower);

    QSlider *bri = new QSlider(Qt::Horizontal);
    bri->setRange(1,100);
    bri->setValue(100);
    QLabel *briLbl = new QLabel("Group Brightness: 100%");
    connect(bri, &QSlider::valueChanged, [=](int v){
        briLbl->setText(QString("Group Brightness: %1%").arg(v));
        for (const auto &d : devices)
            sendCommand(d["device"].toString(), d["sku"].toString(),
                        "devices.capabilities.range", "brightness", v);
    });
    l->addWidget(briLbl); l->addWidget(bri);

    QSlider *temp = new QSlider(Qt::Horizontal);
    temp->setRange(2000,9000);
    temp->setValue(4000);
    QLabel *tempLbl = new QLabel("Group Temp: 4000K");
    connect(temp, &QSlider::valueChanged, [=](int v){
        tempLbl->setText(QString("Group Temp: %1K").arg(v));
        for (const auto &d : devices)
            sendCommand(d["device"].toString(), d["sku"].toString(),
                        "devices.capabilities.color_setting", "colorTemperatureK", v);
    });
    l->addWidget(tempLbl); l->addWidget(temp);

    QPushButton *color = new QPushButton("Pick Group Color");
    connect(color, &QPushButton::clicked, [=]{
        QColor c = QColorDialog::getColor(Qt::white, this);
        if (c.isValid()) {
            int rgb = (c.red()<<16)|(c.green()<<8)|c.blue();
            for (const auto &d : devices)
                sendCommand(d["device"].toString(), d["sku"].toString(),
                            "devices.capabilities.color_setting", "colorRgb", rgb);
        }
    });
    l->addWidget(color);

    return box;
}

void MainWindow::checkPhonePresence()
{
    if (pingProcess->state() != QProcess::NotRunning) return;

    pingProcess->start("ping", QStringList() << "-c" << "1" << "-W" << "2" << phoneHost);
    connect(pingProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [=](int exitCode, QProcess::ExitStatus){
                bool nowOnline = (exitCode == 0);
                if (!firstCheckDone) { phoneWasOnline = nowOnline; firstCheckDone = true; return; }
                if (nowOnline != phoneWasOnline) {
                    phoneWasOnline = nowOnline;
                    int state = nowOnline ? 1 : 0;
                    qDebug() << (nowOnline ? "Phone HOME" : "Phone GONE");
                    for (const QJsonValue &v : std::as_const(deviceList)) {
                        QJsonObject dev = v.toObject();
                        sendCommand(dev["device"].toString(), dev["sku"].toString(),
                                    "devices.capabilities.on_off", "powerSwitch", state);
                    }
                }
            });
}

void MainWindow::checkRoutines()
{
    QTime now = QTime::currentTime();
    for (const Routine &r : routines) {
        if (r.time.hour() == now.hour() && r.time.minute() == now.minute()) {
            qDebug() << "ROUTINE:" << r.name;
            for (const QJsonValue &v : std::as_const(deviceList)) {
                QJsonObject dev = v.toObject();
                sendCommand(dev["device"].toString(), dev["sku"].toString(),
                            "devices.capabilities.on_off", "powerSwitch", r.turnOn ? 1 : 0);
            }
        }
    }
}

void MainWindow::addRoutine()
{
    QDialog d(this);
    d.setWindowTitle("New Routine");
    QVBoxLayout *l = new QVBoxLayout(&d);

    QTimeEdit *time = new QTimeEdit; time->setDisplayFormat("HH:mm");
    QComboBox *action = new QComboBox; action->addItems({"Turn All ON", "Turn All OFF"});
    QLineEdit *name = new QLineEdit("My Routine");

    l->addWidget(new QLabel("Time:")); l->addWidget(time);
    l->addWidget(new QLabel("Action:")); l->addWidget(action);
    l->addWidget(new QLabel("Name:")); l->addWidget(name);

    QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    l->addWidget(bb);

    if (d.exec() == QDialog::Accepted) {
        Routine r;
        r.time = time->time();
        r.turnOn = (action->currentIndex() == 0);
        r.name = name->text().isEmpty() ? (r.turnOn ? "Turn ON" : "Turn OFF") : name->text();
        routines.append(r);
        routineList->addItem(QString("%1 → %2 — %3")
                                 .arg(r.time.toString("HH:mm"))
                                 .arg(r.turnOn ? "ON" : "OFF")
                                 .arg(r.name));
    }
}

void MainWindow::removeRoutine()
{
    auto item = routineList->currentItem();
    if (!item) return;
    int row = routineList->row(item);
    routines.removeAt(row);
    delete item;
}

QWidget* MainWindow::createRoutinesTab()
{
    QWidget *w = new QWidget;
    QVBoxLayout *l = new QVBoxLayout(w);

    l->addWidget(new QLabel("<h2>Daily Routines</h2>"));

    QPushButton *add = new QPushButton("Add Routine");
    add->setMinimumHeight(50);
    connect(add, &QPushButton::clicked, this, &MainWindow::addRoutine);
    l->addWidget(add);

    routineList = new QListWidget;
    l->addWidget(routineList);

    QPushButton *rem = new QPushButton("Remove Selected");
    rem->setStyleSheet("background:#ff5555;color:white;");
    connect(rem, &QPushButton::clicked, this, &MainWindow::removeRoutine);
    l->addWidget(rem);

    l->addStretch();
    return w;
}

void MainWindow::buildUI()
{
    tabWidget->clear();

    QMap<QString, QVector<QJsonObject>> groups;
    QVector<QJsonObject> all;

    for (const QJsonValue &v : deviceList) {
        QJsonObject dev = v.toObject();
        all << dev;
        QString room = dev["deviceName"].toString().split(' ').value(0, "Other");
        groups[room] << dev;
    }

    // All Lights
    {
        QScrollArea *sa = new QScrollArea; sa->setWidgetResizable(true);
        QWidget *w = new QWidget; QVBoxLayout *lay = new QVBoxLayout(w);
        lay->setContentsMargins(20,20,20,20); lay->setSpacing(25);
        lay->addWidget(createGroupControl(all, "ALL LIGHTS"));
        for (const auto &d : all) lay->addWidget(createLightWidget(d));
        lay->addStretch(); sa->setWidget(w);
        tabWidget->addTab(sa, "All Lights (" + QString::number(all.size()) + ")");
    }

    QStringList rooms = groups.keys(); std::sort(rooms.begin(), rooms.end());
    for (const QString &room : rooms) {
        QScrollArea *sa = new QScrollArea; sa->setWidgetResizable(true);
        QWidget *w = new QWidget; QVBoxLayout *lay = new QVBoxLayout(w);
        lay->setContentsMargins(20,20,20,20); lay->setSpacing(25);
        lay->addWidget(createGroupControl(groups[room], room + " — Group"));
        for (const auto &d : groups[room]) lay->addWidget(createLightWidget(d));
        lay->addStretch(); sa->setWidget(w);
        tabWidget->addTab(sa, room + " (" + QString::number(groups[room].size()) + ")");
    }

    tabWidget->addTab(createRoutinesTab(), "Routines");
}
