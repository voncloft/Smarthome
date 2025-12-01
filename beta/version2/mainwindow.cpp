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
#include <QScrollArea>
#include <QInputDialog>
#include <QMessageBox>
#include <QMenuBar>
#include <QToolBar>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <QTabWidget>
#include <QUuid>
#include <algorithm>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("Govee Lights Controller");
    resize(1280, 900);

    QWidget *central = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(0);

    tabWidget = new QTabWidget(this);
    tabWidget->setTabPosition(QTabWidget::North);
    tabWidget->setMovable(true);                    // drag & drop reorder
    tabWidget->setUsesScrollButtons(true);
    tabWidget->setDocumentMode(true);
    tabWidget->setStyleSheet("QTabBar::tab { height: 36px; width: 160px; }");

    mainLayout->addWidget(tabWidget);
    setCentralWidget(central);

    nam = new QNetworkAccessManager(this);
    createMenusAndToolbar();
    promptAndSetApiKey();
}

void MainWindow::createMenusAndToolbar()
{
    auto *menu = menuBar()->addMenu("&Settings");
    auto *changeKey = menu->addAction("Change &API Key...");
    connect(changeKey, &QAction::triggered, this, [this]{ promptAndSetApiKey(true); });

    QToolBar *tb = addToolBar("Main");
    tb->setMovable(false);
    tb->setIconSize(QSize(24,24));

    refreshAction = tb->addAction(QIcon::fromTheme("view-refresh"), "Refresh");
    refreshAction->setShortcut(Qt::Key_F5);
    refreshAction->setToolTip("Refresh all lights (F5)");
    connect(refreshAction, &QAction::triggered, this, &MainWindow::refreshAll);
}

// —————————————————————— API KEY → ~/.config/govee/key ——————————————————————
QString MainWindow::loadApiKey()
{
    QString path = QDir::homePath() + "/.config/govee";
    QDir().mkpath(path);
    QFile f(path + "/key");
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString key = f.readAll().trimmed();
        f.close();
        return key;
    }
    return {};
}

bool MainWindow::saveApiKey(const QString &key)
{
    QString path = QDir::homePath() + "/.config/govee";
    QDir().mkpath(path);
    QFile f(path + "/key");
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
        "Enter your Govee API key from https://app.govee.com", QLineEdit::Password, "", &ok);

    if (!ok || key.trimmed().isEmpty()) {
        QMessageBox::critical(this, "Error", "API key is required to continue.");
        close();
        return;
    }

    apiKey = key.trimmed();
    if (saveApiKey(apiKey))
        QMessageBox::information(this, "Saved", "API key saved to ~/.config/govee/key");
    else
        QMessageBox::warning(this, "Warning", "Key accepted but could not save.");

    loadDevices();
}

// —————————————————————— NETWORK ——————————————————————
void MainWindow::refreshAll()
{
    refreshAction->setEnabled(false);
    loadDevices();
}

void MainWindow::loadDevices()
{
    QNetworkRequest req(QUrl("https://openapi.api.govee.com/router/api/v1/user/devices"));
    req.setRawHeader("Govee-API-Key", apiKey.toUtf8());

    auto *reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]{
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            QMessageBox::warning(this, "Network Error", reply->errorString());
            refreshAction->setEnabled(true);
            return;
        }

        auto doc = QJsonDocument::fromJson(reply->readAll()).object();
        if (doc["code"].toInt() != 200) {
            QMessageBox::warning(this, "API Error", doc["message"].toString());
            refreshAction->setEnabled(true);
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
    if (pendingStates == 0) {
        buildUI();
        return;
    }

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

// —————————————————————— UI HELPERS ——————————————————————
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
    box->setStyleSheet("QGroupBox { font-weight: bold; font-size: 18pt; margin-top: 10px; }");
    auto *layout = new QVBoxLayout(box);

    auto *gc = new QGroupBox("Group Control");
    gc->setStyleSheet("font-weight: bold;");
    auto *gcl = new QVBoxLayout(gc);

    QPushButton *powerBtn = new QPushButton(allOn ? "Turn Group Off" : "Turn Group On");
    powerBtn->setCheckable(true);
    powerBtn->setChecked(allOn);
    powerBtn->setMinimumHeight(55);

    QSlider *briSlider = new QSlider(Qt::Horizontal);
    briSlider->setRange(1,100);
    briSlider->setValue(100);

    QSlider *tempSlider = new QSlider(Qt::Horizontal);
    tempSlider->setRange(2000,9000);
    tempSlider->setValue(6500);

    QPushButton *colorBtn = new QPushButton("Pick Group Color");
    QLabel *briLabel = new QLabel("Group Brightness: 100%");
    QLabel *tempLabel = new QLabel("Group Temp: 6500K");

    connect(powerBtn, &QPushButton::toggled, this, [=](bool on) mutable {
        powerBtn->setText(on ? "Turn Group Off" : "Turn Group On");
        briSlider->setEnabled(on);
        tempSlider->setEnabled(on);
        colorBtn->setEnabled(on);
        for (const auto &d : devices)
            sendCommand(d["device"].toString(), d["sku"].toString(),
                        "devices.capabilities.on_off", "powerSwitch", on);
        QTimer::singleShot(800, this, &MainWindow::refreshAllDeviceStates);
    });

    connect(briSlider, &QSlider::sliderReleased, this, [=]{
        briLabel->setText("Group Brightness: " + QString::number(briSlider->value()) + "%");
        for (const auto &d : devices)
            sendCommand(d["device"].toString(), d["sku"].toString(),
                        "devices.capabilities.range", "brightness", briSlider->value());
        QTimer::singleShot(600, this, &MainWindow::refreshAllDeviceStates);
    });

    connect(tempSlider, &QSlider::sliderReleased, this, [=]{
        tempLabel->setText("Group Temp: " + QString::number(tempSlider->value()) + "K");
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
    gcl->addWidget(briLabel);
    gcl->addWidget(briSlider);
    gcl->addWidget(tempLabel);
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

    return bulb->setStyleSheet("QGroupBox { font-weight: bold; margin-top: 8px; }");
    return bulb;
}

// —————————————————————— BUILD UI ——————————————————————
void MainWindow::buildUI()
{
    while (tabWidget->count()) {
        QWidget *w = tabWidget->widget(0);
        tabWidget->removeTab(0);
        w->deleteLater();
    }

    QMap<QString, QVector<QJsonObject>> groups;
    for (const auto &v : deviceList) {
        QJsonObject dev = v.toObject();
        QString name = dev["deviceName"].toString("Light");
        QString key = name.split(' ', Qt::SkipEmptyParts).value(0, "Other");
        groups[key] << dev;
    }

    // === ALL LIGHTS TAB ===
    {
        QScrollArea *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        QWidget *page = new QWidget;
        QVBoxLayout *layout = new QVBoxLayout(page);
        layout->setContentsMargins(20,20,20,20);
        layout->setSpacing(25);

        QVector<QJsonObject> all;
        for (const auto &v : deviceList) all << v.toObject();
        layout->addWidget(createGroupControl(all, "Control ALL Lights"));

        for (const auto &v : deviceList)
            layout->addWidget(createLightWidget(v.toObject()));

        layout->addStretch();
        scroll->setWidget(page);
        tabWidget->insertTab(0, scroll, QString::fromUtf8("All Lights (%1)").arg(deviceList.size()));
    }

    // === ROOM TABS ===
    QStringList rooms = groups.keys();
    std::sort(rooms.begin(), rooms.end());

    for (const QString &room : rooms) {
        const auto &devices = groups[room];

        QScrollArea *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        QWidget *page = new QWidget;
        QVBoxLayout *layout = new QVBoxLayout(page);
        layout->setContentsMargins(20,20,20,20);
        layout->setSpacing(25);

        layout->addWidget(createGroupControl(devices, room + " Lights"));

        for (const auto &dev : devices)
            layout->addWidget(createLightWidget(dev));

        layout->addStretch();
        scroll->setWidget(page);
        tabWidget->addTab(scroll, room + QString(" (%1)").arg(devices.size()));
    }
}
