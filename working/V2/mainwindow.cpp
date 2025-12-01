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
    setWindowTitle("Govee Lights - Auto On When Home");
    resize(1450, 950);

    tabWidget = new QTabWidget(this);
    tabWidget->setTabPosition(QTabWidget::North);
    tabWidget->setDocumentMode(true);
    tabWidget->setMovable(true);
    setCentralWidget(tabWidget);

    nam = new QNetworkAccessManager(this);

    // Phone presence detection setup
    pingProcess = new QProcess(this);
    presenceTimer = new QTimer(this);
    connect(presenceTimer, &QTimer::timeout, this, &MainWindow::checkPhonePresence);
    presenceTimer->start(8000); // Every 8 seconds

    if (!loadApiKey()) {
        promptForApiKey();
    }

    if (apiKey.isEmpty()) {
        QMessageBox::critical(this, "Error", "API key required\nPut it in ~/.config/govee/api-key");
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
        if (f.open(QIODevice::WriteOnly)) {
            f.write(apiKey.toUtf8());
        }
    }
}

void MainWindow::loadDevices()
{
    QNetworkRequest req(QUrl("https://openapi.api.govee.com/router/api/v1/user/devices"));
    req.setRawHeader("Govee-API-Key", apiKey.toUtf8());

    QNetworkReply *reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]{
        reply->deleteLater();

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);

        if (reply->error() != QNetworkReply::NoError || doc.object()["code"].toInt() != 200) {
            QMessageBox::critical(this, "Error", "Invalid API key or network error");
            return;
        }

        deviceList = doc.object()["data"].toArray();
        pendingStates = deviceList.size();
        deviceStates.clear();

        if (pendingStates == 0) {
            buildUI();
            return;
        }

        for (const QJsonValue &v : deviceList) {
            QJsonObject dev = v.toObject();
            QString mac = dev["device"].toString();
            QString sku = dev["sku"].toString();

            QJsonObject payloadObj;
            payloadObj.insert("sku", sku);
            payloadObj.insert("device", mac);

            QJsonObject body;
            body.insert("requestId", QUuid::createUuid().toString(QUuid::WithoutBraces));
            body.insert("payload", payloadObj);

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
    QJsonObject cap;
    cap.insert("type", type);
    cap.insert("instance", instance);
    cap.insert("value", QJsonValue::fromVariant(value));

    QJsonObject payload;
    payload.insert("sku", sku);
    payload.insert("device", device);
    payload.insert("capability", cap);

    QJsonObject root;
    root.insert("requestId", QUuid::createUuid().toString(QUuid::WithoutBraces));
    root.insert("payload", payload);

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
            if (i == "powerSwitch")        isOn = s["value"].toInt();
            if (i == "brightness")         bri = s["value"].toInt();
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

    QPushButton *btn = new QPushButton("Turn Group Off");
    btn->setCheckable(true);
    btn->setMinimumHeight(50);
    connect(btn, &QPushButton::toggled, [=](bool on){
        btn->setText(on ? "Turn Group Off" : "Turn Group On");
        for (const auto &d : devices)
            sendCommand(d["device"].toString(), d["sku"].toString(),
                        "devices.capabilities.on_off", "powerSwitch", on ? 1 : 0);
    });
    l->addWidget(btn);
    return box;
}

void MainWindow::checkPhonePresence()
{
    if (pingProcess->state() != QProcess::NotRunning)
        return;

    pingProcess->start("ping", QStringList() << "-c" << "1" << "-W" << "2" << phoneHost);

    connect(pingProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [=](int exitCode, QProcess::ExitStatus){
                bool nowOnline = (exitCode == 0);

                if (!firstCheckDone) {
                    phoneWasOnline = nowOnline;
                    firstCheckDone = true;
                    return;
                }

                if (nowOnline != phoneWasOnline) {
                    phoneWasOnline = nowOnline;
                    int state = nowOnline ? 1 : 0;

                    qDebug() << (nowOnline ? "Phone HOME — ALL LIGHTS ON" : "Phone GONE — ALL LIGHTS OFF");

                    for (const QJsonValue &v : std::as_const(deviceList)) {
                        QJsonObject dev = v.toObject();
                        sendCommand(dev["device"].toString(), dev["sku"].toString(),
                                    "devices.capabilities.on_off", "powerSwitch", state);
                    }
                }
            });
}

void MainWindow::buildUI()
{
    while (tabWidget->count()) delete tabWidget->widget(0);

    QMap<QString, QVector<QJsonObject>> groups;
    QVector<QJsonObject> allLights;

    for (const QJsonValue &v : deviceList) {
        QJsonObject dev = v.toObject();
        allLights << dev;
        QString room = dev["deviceName"].toString().split(' ').value(0, "Other");
        groups[room] << dev;
    }

    // All Lights tab
    {
        QScrollArea *sa = new QScrollArea;
        sa->setWidgetResizable(true);
        QWidget *w = new QWidget;
        QVBoxLayout *lay = new QVBoxLayout(w);
        lay->setContentsMargins(20,20,20,20);
        lay->setSpacing(25);
        lay->addWidget(createGroupControl(allLights, "ALL LIGHTS"));
        for (const auto &d : allLights)
            lay->addWidget(createLightWidget(d));
        lay->addStretch();
        sa->setWidget(w);
        tabWidget->addTab(sa, "All Lights (" + QString::number(allLights.size()) + ")");
    }

    // Room tabs
    QStringList rooms = groups.keys();
    std::sort(rooms.begin(), rooms.end());
    for (const QString &room : rooms) {
        QScrollArea *sa = new QScrollArea;
        sa->setWidgetResizable(true);
        QWidget *w = new QWidget;
        QVBoxLayout *lay = new QVBoxLayout(w);
        lay->setContentsMargins(20,20,20,20);
        lay->setSpacing(25);
        lay->addWidget(createGroupControl(groups[room], room + " — Group"));
        for (const auto &d : groups[room])
            lay->addWidget(createLightWidget(d));
        lay->addStretch();
        sa->setWidget(w);
        tabWidget->addTab(sa, room + " (" + QString::number(groups[room].size()) + ")");
    }
}
