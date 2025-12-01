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
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QMenuBar>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QUuid>
#include <QTimer>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("Govee Lights Controller");
    resize(1200, 1000);

    QWidget *central = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(10, 10, 10, 10);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    panelWidget = new QWidget;
    panelLayout = new QVBoxLayout(panelWidget);
    panelLayout->setSpacing(30);
    panelLayout->setContentsMargins(20, 20, 20, 20);
    scroll->setWidget(panelWidget);
    mainLayout->addWidget(scroll);
    setCentralWidget(central);

    nam = new QNetworkAccessManager(this);
    createMenus();
    promptAndSetApiKey();
}

void MainWindow::createMenus()
{
    auto *menu = menuBar()->addMenu("&Settings");
    auto *act = new QAction("Change &API Key...", this);
    menu->addAction(act);
    connect(act, &QAction::triggered, this, [this]() { promptAndSetApiKey(true); });
}

QString MainWindow::loadApiKey()
{
    QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(path);
    QFile f(QDir(path).filePath("key"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString key = f.readAll().trimmed();
        f.close();
        return key;
    }
    return {};
}

bool MainWindow::saveApiKey(const QString &key)
{
    QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(path);
    QFile f(QDir(path).filePath("key"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
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
    QString key = QInputDialog::getText(this, "Govee API Key", 
        "Enter your Govee API key from https://app.govee.com", 
        QLineEdit::Password, "", &ok);

    if (!ok || key.trimmed().isEmpty()) {
        QMessageBox::critical(this, "Nope", "Need an API key to work.");
        QTimer::singleShot(0, this, &QWidget::close);
        return;
    }

    apiKey = key.trimmed();
    if (saveApiKey(apiKey))
        QMessageBox::information(this, "Saved", "API key saved for next time.");
    else
        QMessageBox::warning(this, "Warning", "Key accepted but not saved.");

    loadDevices();
}

void MainWindow::loadDevices()
{
    QNetworkRequest req(QUrl("https://openapi.api.govee.com/router/api/v1/user/devices"));
    req.setRawHeader("Govee-API-Key", apiKey.toUtf8());
    auto *reply = nam->get(req);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QMessageBox::warning(this, "Error", reply->errorString());
            return;
        }

        auto doc = QJsonDocument::fromJson(reply->readAll()).object();
        if (doc["code"].toInt() != 200) {
            QMessageBox::warning(this, "API Error", doc["message"].toString());
            return;
        }

        deviceList = doc["data"].toArray();
        refreshAllDeviceStates();
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
        QJsonObject body{ {"requestId", "uuid"}, {"payload", payload} };

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
    nam->post(req, QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void MainWindow::buildUI()
{
    // Clear UI
    while (auto *item = panelLayout->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    QMap<QString, QVector<QJsonObject>> groups;
    for (const auto &v : deviceList) {
        auto dev = v.toObject();
        QString name = dev["deviceName"].toString("Light");
        QString key = name.split(' ', Qt::SkipEmptyParts).value(0, "Other");
        groups[key] << dev;
    }

    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        const auto &devices = it.value();
        auto *groupBox = new QGroupBox(it.key() + " Lights (" + QString::number(devices.size()) + ")");
        groupBox->setStyleSheet("QGroupBox { font-weight: bold; font-size: 18pt; }");
        auto *groupLayout = new QVBoxLayout(groupBox);

        // Group power state
        int onCount = 0;
        for (const auto &d : devices) {
            QString mac = d["device"].toString();
            if (deviceStates.contains(mac)) {
                for (const auto &c : deviceStates[mac]) {
                    auto cap = c.toObject();
                    if (cap["instance"].toString() == "powerSwitch" && cap["state"].toObject()["value"].toInt())
                        onCount++;
                }
            }
        }
        bool allOn = (onCount == devices.size() && !devices.isEmpty());

        // Group controls
        auto *gc = new QGroupBox("Group Control");
        gc->setStyleSheet("QGroupBox { font-weight: bold; }");
        auto *gcl = new QVBoxLayout(gc);

        auto *powerBtn = new QPushButton(allOn ? "Turn Group Off" : "Turn Group On");
        powerBtn->setCheckable(true);
        powerBtn->setChecked(allOn);
        powerBtn->setMinimumHeight(55);

        auto *briSlider = new QSlider(Qt::Horizontal); briSlider->setRange(1,100); briSlider->setValue(100);
        auto *tempSlider = new QSlider(Qt::Horizontal); tempSlider->setRange(2000,9000); tempSlider->setValue(6500);
        auto *colorBtn = new QPushButton("Pick Group Color");

        auto *briLabel = new QLabel("Group Brightness: 100%");
        auto *tempLabel = new QLabel("Group Temp: 6500K");

        connect(powerBtn, &QPushButton::toggled, this, [=](bool on) {
            powerBtn->setText(on ? "Turn Group Off" : "Turn Group On");
            briSlider->setEnabled(on);
            tempSlider->setEnabled(on);
            colorBtn->setEnabled(on);
            for (const auto &d : devices)
                sendCommand(d["device"].toString(), d["sku"].toString(), "devices.capabilities.on_off", "powerSwitch", on);
            QTimer::singleShot(800, this, &MainWindow::refreshAllDeviceStates);
        });

        connect(briSlider, &QSlider::sliderReleased, this, [=]() {
            briLabel->setText("Group Brightness: " + QString::number(briSlider->value()) + "%");
            for (const auto &d : devices)
                sendCommand(d["device"].toString(), d["sku"].toString(), "devices.capabilities.range", "brightness", briSlider->value());
            QTimer::singleShot(600, this, &MainWindow::refreshAllDeviceStates);
        });

        connect(tempSlider, &QSlider::sliderReleased, this, [=]() {
            tempLabel->setText("Group Temp: " + QString::number(tempSlider->value()) + "K");
            for (const auto &d : devices)
                sendCommand(d["device"].toString(), d["sku"].toString(), "devices.capabilities.color_setting", "colorTemperatureK", tempSlider->value());
            QTimer::singleShot(600, this, &MainWindow::refreshAllDeviceStates);
        });

        connect(colorBtn, &QPushButton::clicked, this, [=]() {
            QColor c = QColorDialog::getColor(Qt::white, this);
            if (c.isValid()) {
                int rgb = (c.red()<<16) | (c.green()<<8) | c.blue();
                for (const auto &d : devices)
                    sendCommand(d["device"].toString(), d["sku"].toString(), "devices.capabilities.color_setting", "colorRgb", rgb);
                QTimer::singleShot(800, this, &MainWindow::refreshAllDeviceStates);
            }
        });

        gcl->addWidget(powerBtn);
        gcl->addWidget(briLabel);
        gcl->addWidget(briSlider);
        gcl->addWidget(tempLabel);
        gcl->addWidget(tempSlider);
        gcl->addWidget(colorBtn);
        groupLayout->addWidget(gc);

        // Individual bulbs
        for (const auto &dev : devices) {
            QString name = dev["deviceName"].toString("Light");
            QString mac = dev["device"].toString();
            QString sku = dev["sku"].toString();

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

            auto *pwr = new QPushButton(isOn ? "Turn Off" : "Turn On");
            pwr->setCheckable(true);
            pwr->setChecked(isOn);
            pwr->setMinimumHeight(50);
            connect(pwr, &QPushButton::toggled, [=](bool on) {
                pwr->setText(on ? "Turn Off" : "Turn On");
                sendCommand(mac, sku, "devices.capabilities.on_off", "powerSwitch", on);
                QTimer::singleShot(600, this, &MainWindow::refreshAllDeviceStates);
            });
            bl->addWidget(pwr);

            auto *bs = new QSlider(Qt::Horizontal); bs->setRange(1,100); bs->setValue(bri); bs->setEnabled(isOn);
            auto *blbl = new QLabel("Brightness: " + QString::number(bri) + "%");
            connect(bs, &QSlider::valueChanged, [=](int v) {
                blbl->setText("Brightness: " + QString::number(v) + "%");
                sendCommand(mac, sku, "devices.capabilities.range", "brightness", v);
            });
            bl->addWidget(blbl); bl->addWidget(bs);

            auto *cb = new QPushButton("Pick Color"); cb->setEnabled(isOn);
            connect(cb, &QPushButton::clicked, [=]() mutable {
                QColor c = QColorDialog::getColor(col, this);
                if (c.isValid()) {
                    int rgb = (c.red()<<16)|(c.green()<<8)|c.blue();
                    sendCommand(mac, sku, "devices.capabilities.color_setting", "colorRgb", rgb);
                    QTimer::singleShot(600, this, &MainWindow::refreshAllDeviceStates);
                }
            });
            bl->addWidget(cb);

            auto *ts = new QSlider(Qt::Horizontal); ts->setRange(2000,9000); ts->setValue(temp); ts->setEnabled(isOn);
            auto *tlbl = new QLabel("Temp: " + QString::number(temp) + "K");
            connect(ts, &QSlider::valueChanged, [=](int v) {
                tlbl->setText("Temp: " + QString::number(v) + "K");
                sendCommand(mac, sku, "devices.capabilities.color_setting", "colorTemperatureK", v);
            });
            bl->addWidget(tlbl); bl->addWidget(ts);

            groupLayout->addWidget(bulb);
        }

        groupLayout->addStretch();
        panelLayout->addWidget(groupBox);
    }
    panelLayout->addStretch();
}
