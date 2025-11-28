// mainwindow.cpp
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
#include <QUuid>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("Govee Lights");
    resize(1150, 1000);

    QWidget *central = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(10, 10, 10, 10);

    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    panelWidget = new QWidget;
    panelLayout = new QVBoxLayout(panelWidget);
    panelLayout->setSpacing(25);
    panelLayout->setContentsMargins(15, 15, 15, 15);
    scroll->setWidget(panelWidget);
    mainLayout->addWidget(scroll);
    setCentralWidget(central);

    nam = new QNetworkAccessManager(this);
    loadDevices();
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
            QJsonObject body{ {"requestId", "uuid"}, {"payload", payloadObj} };

            QNetworkRequest r(QUrl("https://openapi.api.govee.com/router/api/v1/device/state"));
            r.setRawHeader("Govee-API-Key", apiKey.toUtf8());
            r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

            QNetworkReply *stateReply = nam->post(r, QJsonDocument(body).toJson(QJsonDocument::Compact));
            connect(stateReply, &QNetworkReply::finished, this, &MainWindow::onStateFinished);
        }
    });
}

void MainWindow::onStateFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply && reply->error() == QNetworkReply::NoError) {
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
    nam->post(req, QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void MainWindow::buildUI()
{
    // Clear previous UI
    while (auto item = panelLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    // Group devices by first word of their name
    QMap<QString, QVector<QJsonObject>> groups;
    for (const QJsonValue &v : deviceList) {
        QJsonObject dev = v.toObject();
        QString name = dev["deviceName"].toString("Light");
        QString key = name.split(" ", Qt::SkipEmptyParts).value(0, "Other");
        groups[key].append(dev);
    }

    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
        const QVector<QJsonObject> &devices = it.value();
        QString groupName = it.key() + " Lights (" + QString::number(devices.size()) + ")";

        QGroupBox *groupBox = new QGroupBox(groupName);
        groupBox->setStyleSheet("QGroupBox { font-weight: bold; font-size: 16pt; }");
        QVBoxLayout *groupLayout = new QVBoxLayout(groupBox);

        // === GROUP CONTROL ===
        QGroupBox *groupCtrl = new QGroupBox("Group Control");
        groupCtrl->setStyleSheet("QGroupBox { font-weight: bold; }");
        QVBoxLayout *gcLayout = new QVBoxLayout(groupCtrl);

        int onCount = 0;
        for (const auto &d : devices) {
            QString mac = d["device"].toString();
            if (deviceStates.contains(mac)) {
                for (const QJsonValue &c : deviceStates[mac]) {
                    QJsonObject cap = c.toObject();
                    if (cap["instance"].toString() == "powerSwitch" && cap["state"].toObject()["value"].toInt() == 1) {
                        onCount++;
                        break;
                    }
                }
            }
        }

        bool allOn = (onCount == devices.size() && !devices.isEmpty());
        bool someOn = (onCount > 0 && onCount < devices.size());

        QPushButton *groupPower = new QPushButton();
        groupPower->setCheckable(true);
        groupPower->setChecked(allOn);
        groupPower->setMinimumHeight(48);

        auto updateGroupPower = [groupPower, &allOn, &someOn]() {
            if (someOn) {
                groupPower->setText("Partial On");
                groupPower->setProperty("color", "warning");
            } else {
                groupPower->setText(allOn ? "Turn Group Off" : "Turn Group On");
                groupPower->setProperty("color", allOn ? "positive" : "destructive");
            }
            groupPower->setStyleSheet("");
        };
        updateGroupPower();

        QSlider *groupBri = new QSlider(Qt::Horizontal);
        groupBri->setRange(1, 100);
        groupBri->setValue(100);
        groupBri->setEnabled(allOn);
        QLabel *groupBriLabel = new QLabel("Group Brightness: 100%");

        QSlider *groupTemp = new QSlider(Qt::Horizontal);
        groupTemp->setRange(2000, 9000);
        groupTemp->setValue(4000);
        groupTemp->setEnabled(allOn);
        QLabel *groupTempLabel = new QLabel("Group Temp: 4000K");

        QPushButton *groupColor = new QPushButton("Pick Group Color");
        groupColor->setEnabled(allOn);

        // --- Connections ---
        connect(groupPower, &QPushButton::toggled, this, [=, &allOn, &someOn](bool on){
            allOn = on;
            someOn = false;
            updateGroupPower();
            groupBri->setEnabled(on);
            groupTemp->setEnabled(on);
            groupColor->setEnabled(on);
            for (const auto &dev : devices) {
                sendCommand(dev["device"].toString(), dev["sku"].toString(),
                            "devices.capabilities.on_off", "powerSwitch", on ? 1 : 0);
            }
        });

        connect(groupBri, &QSlider::valueChanged, this, [=](int v){
            groupBriLabel->setText(QString("Group Brightness: %1%").arg(v));
            for (const auto &dev : devices)
                sendCommand(dev["device"].toString(), dev["sku"].toString(),
                            "devices.capabilities.range", "brightness", v);
        });

        connect(groupTemp, &QSlider::valueChanged, this, [=](int v){
            groupTempLabel->setText(QString("Group Temp: %1K").arg(v));
            for (const auto &dev : devices)
                sendCommand(dev["device"].toString(), dev["sku"].toString(),
                            "devices.capabilities.color_setting", "colorTemperatureK", v);
        });

        connect(groupColor, &QPushButton::clicked, this, [=]() mutable {
            QColor c = QColorDialog::getColor(Qt::white, this, "Pick Group Color");
            if (!c.isValid()) return;
            for (const auto &dev : devices) {
                sendCommand(dev["device"].toString(), dev["sku"].toString(),
                            "devices.capabilities.color_setting", "colorRgb",
                            (c.red() << 16) | (c.green() << 8) | c.blue());
            }
        });

        // Layout group controls
        QHBoxLayout *powerRow = new QHBoxLayout;
        powerRow->addWidget(new QLabel("Power:"));
        powerRow->addWidget(groupPower);
        powerRow->addStretch();
        gcLayout->addLayout(powerRow);
        gcLayout->addWidget(groupBriLabel);
        gcLayout->addWidget(groupBri);
        gcLayout->addWidget(groupTempLabel);
        gcLayout->addWidget(groupTemp);
        gcLayout->addWidget(groupColor);

        groupLayout->addWidget(groupCtrl);

        // === INDIVIDUAL LIGHTS ===
        for (const QJsonObject &dev : devices) {
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
                        col = QColor((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
                    }
                }
            }

            QGroupBox *bulb = new QGroupBox(name);
            QVBoxLayout *bl = new QVBoxLayout(bulb);

            QFormLayout *info = new QFormLayout;
            info->addRow("MAC:", new QLabel(mac.mid(0,17)));
            info->addRow("Model:", new QLabel(sku));
            bl->addLayout(info);

            QPushButton *pwr = new QPushButton(isOn ? "Turn Off" : "Turn On");
            pwr->setCheckable(true);
            pwr->setChecked(isOn);
            pwr->setMinimumHeight(48);
            pwr->setProperty("color", isOn ? "positive" : "destructive");
            pwr->setStyleSheet("");

            connect(pwr, &QPushButton::toggled, [=](bool on){
                pwr->setText(on ? "Turn Off" : "Turn On");
                pwr->setProperty("color", on ? "positive" : "destructive");
                pwr->setStyleSheet("");
                sendCommand(mac, sku, "devices.capabilities.on_off", "powerSwitch", on ? 1 : 0);
            });
            bl->addWidget(pwr);

            QSlider *bs = new QSlider(Qt::Horizontal);
            bs->setRange(1,100); bs->setValue(bri); bs->setEnabled(isOn);
            QLabel *blbl = new QLabel(QString("Brightness: %1%").arg(bri));
            connect(bs, &QSlider::valueChanged, [=](int v){
                blbl->setText(QString("Brightness: %1%").arg(v));
                sendCommand(mac, sku, "devices.capabilities.range", "brightness", v);
            });
            bl->addWidget(blbl); bl->addWidget(bs);

            QPushButton *cb = new QPushButton("Pick Color");
            cb->setEnabled(isOn);
            connect(cb, &QPushButton::clicked, [=]() mutable {
                QColor c = QColorDialog::getColor(col, this);
                if (c.isValid())
                    sendCommand(mac, sku, "devices.capabilities.color_setting", "colorRgb",
                                (c.red()<<16)|(c.green()<<8)|c.blue());
            });
            bl->addWidget(cb);

            QSlider *ts = new QSlider(Qt::Horizontal);
            ts->setRange(2000,9000); ts->setValue(temp); ts->setEnabled(isOn);
            QLabel *tlbl = new QLabel(QString("Temp: %1K").arg(temp));
            connect(ts, &QSlider::valueChanged, [=](int v){
                tlbl->setText(QString("Temp: %1K").arg(v));
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
