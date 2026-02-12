#include "mainwindow.h"
#include <QApplication>
#include <QToolBar>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
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
#include <QCheckBox>
#include <QSpinBox>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QDebug>
#include <QUuid>
#include <QSignalBlocker>

static bool parsePowerState(const QJsonValue &value)
{
    if (value.isBool()) return value.toBool();
    if (value.isDouble()) return value.toInt() != 0;

    const QString text = value.toString().trimmed().toLower();
    return text == "1" || text == "on" || text == "true";
}

static QString weekdayLabel(int day)
{
    static const QStringList labels = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    if (day < 1 || day > 7) return "?";
    return labels[day - 1];
}

static QString formatDays(const QList<int> &days)
{
    if (days.size() == 7) return "Every day";
    if (days.isEmpty()) return "No days";

    QStringList out;
    for (int day : days) out << weekdayLabel(day);
    return out.join(", ");
}

static QString formatRoutineTargets(const QList<RoutineDeviceSetting> &settings)
{
    QStringList names;
    for (const RoutineDeviceSetting &s : settings) {
        const QString label = (s.group == "__all__" || s.group.isEmpty()) ? "All Lights" : s.group;
        if (!names.contains(label)) names << label;
    }
    return names.isEmpty() ? "No targets" : names.join(", ");
}

static QString roomForDevice(const QJsonObject &dev)
{
    return dev["deviceName"].toString().split(' ', Qt::SkipEmptyParts).value(0, "Other");
}

// ==========================================================================
// MAIN WINDOW Constructor
// ==========================================================================

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("Govee Lights — Auto + Routines");
    resize(1450, 950);

    // ----------------------------
    // Central widget: Tabs
    // ----------------------------
    QWidget *mainWidget = new QWidget;
    QVBoxLayout *mainLayout = new QVBoxLayout(mainWidget);

    // Top bar: refresh on left, clock on right
    refreshBtn = new QPushButton("Refresh Devices");
    refreshBtn->setFixedHeight(40);
    refreshBtn->setStyleSheet("font-size:16px;");

    QLabel *timeLabel = new QLabel;
    timeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    timeLabel->setStyleSheet("font-size:18pt; font-weight:600; padding-right:6px;");

    QHBoxLayout *topBar = new QHBoxLayout;
    topBar->addWidget(refreshBtn, 0, Qt::AlignLeft);
    topBar->addStretch();
    topBar->addWidget(timeLabel, 0, Qt::AlignRight);
    mainLayout->addLayout(topBar);

    QTimer *clockTimer = new QTimer(this);
    connect(clockTimer, &QTimer::timeout, this, [timeLabel] {
        timeLabel->setText(QDateTime::currentDateTime().toString("ddd MMM d  h:mm:ss AP"));
    });
    clockTimer->start(1000);
    timeLabel->setText(QDateTime::currentDateTime().toString("ddd MMM d  h:mm:ss AP"));

    tabWidget = new QTabWidget(this);
    tabWidget->setTabPosition(QTabWidget::North);
    tabWidget->setDocumentMode(true);
    tabWidget->setMovable(true);

    mainLayout->addWidget(tabWidget);
    setCentralWidget(mainWidget);

    connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::refreshDevices);

    // ----------------------------
    // Networking, timers
    // ----------------------------
    nam = new QNetworkAccessManager(this);

    pingProcess = new QProcess(this);
    presenceTimer = new QTimer(this);
    routineTimer = new QTimer(this);

    connect(presenceTimer, &QTimer::timeout, this, &MainWindow::checkPhonePresence);
    connect(routineTimer, &QTimer::timeout, this, &MainWindow::checkRoutines);

    presenceTimer->start(8000);     // unchanged
    routineTimer->start(60000);     // unchanged

    if (!loadApiKey()) promptForApiKey();
    if (apiKey.isEmpty()) {
        QMessageBox::critical(this, "Error", "API key required");
        QApplication::quit();
        return;
    }

    loadRoutines();
    loadDevices();
}

// ==========================================================================
// API KEY LOADING — unchanged
// ==========================================================================

bool MainWindow::loadApiKey()
{
    QFile f(QDir::homePath() + "/.config/govee/api-key");
    if (f.open(QIODevice::ReadOnly)) {
        apiKey = QString::fromUtf8(f.readAll()).trimmed();
        f.close();
        return true;
    }
    return false;
}

void MainWindow::promptForApiKey()
{
    bool ok = false;
    QString key = QInputDialog::getText(
        this, "Govee API Key",
        "Enter your API key:",
        QLineEdit::Password, "",
        &ok
    );

    if (ok && !key.isEmpty()) {
        apiKey = key.trimmed();
        QDir().mkpath(QDir::homePath() + "/.config/govee");
        QFile f(QDir::homePath() + "/.config/govee/api-key");
        if (f.open(QIODevice::WriteOnly)) f.write(apiKey.toUtf8());
    }
}

void MainWindow::loadRoutines()
{
    routines.clear();

    QFile f(QDir::homePath() + "/.config/govee/routines.json");
    if (!f.open(QIODevice::ReadOnly))
        return;

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject())
        return;

    const QJsonArray routineArray = doc.object().value("routines").toArray();
    for (const QJsonValue &rv : routineArray) {
        const QJsonObject ro = rv.toObject();

        Routine r;
        r.name = ro.value("name").toString("Schedule");
        r.time = QTime::fromString(ro.value("time").toString(), "HH:mm");
        if (!r.time.isValid())
            continue;

        const QJsonArray days = ro.value("days").toArray();
        for (const QJsonValue &dv : days) {
            const int day = dv.toInt();
            if (day >= 1 && day <= 7 && !r.days.contains(day))
                r.days << day;
        }
        if (r.days.isEmpty()) {
            for (int day = 1; day <= 7; ++day) r.days << day;
        }

        const QJsonArray settings = ro.value("settings").toArray();
        for (const QJsonValue &sv : settings) {
            const QJsonObject so = sv.toObject();
            RoutineDeviceSetting s;
            s.group = so.value("group").toString("__all__");
            if (s.group.isEmpty()) s.group = "__all__";

            s.usePower = so.value("usePower").toBool(false);
            s.power = so.value("power").toInt(1) ? 1 : 0;

            s.useBrightness = so.value("useBrightness").toBool(false);
            s.brightness = qBound(1, so.value("brightness").toInt(100), 100);

            s.useTemp = so.value("useTemp").toBool(false);
            s.temperature = qBound(2000, so.value("temperature").toInt(4000), 9000);

            s.useColor = so.value("useColor").toBool(false);
            const QColor parsed = QColor(so.value("color").toString("#ffffff"));
            s.color = parsed.isValid() ? parsed : QColor(Qt::white);

            r.settings.append(s);
        }

        if (!r.settings.isEmpty())
            routines.append(r);
    }
}

void MainWindow::saveRoutines() const
{
    const QString configDir = QDir::homePath() + "/.config/govee";
    QDir().mkpath(configDir);

    QJsonArray routineArray;
    for (const Routine &r : routines) {
        QJsonObject ro;
        ro["name"] = r.name;
        ro["time"] = r.time.toString("HH:mm");

        QJsonArray days;
        for (int day : r.days) days.append(day);
        ro["days"] = days;

        QJsonArray settings;
        for (const RoutineDeviceSetting &s : r.settings) {
            QJsonObject so;
            so["group"] = s.group;
            so["usePower"] = s.usePower;
            so["power"] = s.power;
            so["useBrightness"] = s.useBrightness;
            so["brightness"] = s.brightness;
            so["useTemp"] = s.useTemp;
            so["temperature"] = s.temperature;
            so["useColor"] = s.useColor;
            so["color"] = s.color.name(QColor::HexRgb);
            settings.append(so);
        }
        ro["settings"] = settings;
        routineArray.append(ro);
    }

    QJsonObject root;
    root["version"] = 1;
    root["routines"] = routineArray;

    QSaveFile f(configDir + "/routines.json");
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to open routines file for writing";
        return;
    }

    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit())
        qWarning() << "Failed to commit routines file";
}

// ==========================================================================
// REFRESH DEVICES
// ==========================================================================

void MainWindow::refreshDevices()
{
    deviceStates.clear();
    pendingStates = 0;

    tabWidget->clear();
    loadDevices();
}

// ==========================================================================
// DEVICE LOADING — UNCHANGED EXCEPT AUTO-REFRESH SAFETY
// ==========================================================================

void MainWindow::loadDevices()
{
    QNetworkRequest req(QUrl("https://openapi.api.govee.com/router/api/v1/user/devices"));
    req.setRawHeader("Govee-API-Key", apiKey.toUtf8());

    QNetworkReply *reply = nam->get(req);

    connect(reply, &QNetworkReply::finished, this, [=]{
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "Device list error:" << reply->errorString();
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (doc.object()["code"].toInt() != 200) return;

        deviceList = doc.object()["data"].toArray();

        pendingStates = deviceList.size();
        deviceStates.clear();

        if (pendingStates == 0) {
            buildUI();
            return;
        }

        // Request each device's state
        for (const QJsonValue &v : deviceList) {
            QJsonObject dev = v.toObject();

            QString mac = dev["device"].toString();
            QString sku = dev["sku"].toString();

            QJsonObject payload{
                {"sku", sku},
                {"device", mac}
            };

            QJsonObject root{
                {"requestId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {"payload", payload}
            };

            QNetworkRequest stateReq(QUrl("https://openapi.api.govee.com/router/api/v1/device/state"));
            stateReq.setRawHeader("Govee-API-Key", apiKey.toUtf8());
            stateReq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

            QNetworkReply *stateReply = nam->post(stateReq, QJsonDocument(root).toJson());

            connect(stateReply, &QNetworkReply::finished, this, &MainWindow::onStateFinished);
        }
    });
}

// ==========================================================================
// PER-DEVICE STATE RESULTS — unchanged
// ==========================================================================

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

    if (--pendingStates <= 0)
        buildUI();
}

// ==========================================================================
// SEND COMMAND — unchanged
// ==========================================================================

void MainWindow::sendCommand(const QString &device, const QString &sku,
                             const QString &type, const QString &instance,
                             const QVariant &value)
{
    QJsonObject cap{
        {"type", type},
        {"instance", instance},
        {"value", QJsonValue::fromVariant(value)}
    };

    QJsonObject payload{
        {"sku", sku},
        {"device", device},
        {"capability", cap}
    };

    QJsonObject root{
        {"requestId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"payload", payload}
    };

    QNetworkRequest req(QUrl("https://openapi.api.govee.com/router/api/v1/device/control"));
    req.setRawHeader("Govee-API-Key", apiKey.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    nam->post(req, QJsonDocument(root).toJson());
}

// ==========================================================================
// INDIVIDUAL LIGHT WIDGET — UNCHANGED
// ==========================================================================


// ==========================================================================
// GROUP CONTROL — UNCHANGED
// (also fully included in PART 2)
// ==========================================================================


// ==========================================================================
// PHONE PRESENCE — UNCHANGED
// (also included in PART 2)
// ==========================================================================


// ==========================================================================
// ========== ROUTINES SECTION (PART 2 FOLLOWS) ==========
// ==========================================================================

// ==========================================================================
// INDIVIDUAL LIGHT WIDGET
// ==========================================================================

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
            if (i == "powerSwitch") isOn = parsePowerState(s["value"]);
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
    power->setProperty("role", "lightPower");
    power->setProperty("deviceMac", mac);
    power->setMinimumHeight(50);
    connect(power, &QPushButton::toggled, this, [=](bool on){
        power->setText(on ? "Turn Off" : "Turn On");
        sendCommand(mac, sku, "devices.capabilities.on_off", "powerSwitch", on ? 1 : 0);
        setDevicePowerState(mac, on);
        refreshPowerButtons();
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

// ==========================================================================
// GROUP CONTROL WIDGET
// ==========================================================================

QWidget* MainWindow::createGroupControl(const QVector<QJsonObject> &devices, const QString &title)
{
    QGroupBox *box = new QGroupBox(title);
    QVBoxLayout *l = new QVBoxLayout(box);

    bool groupIsOn = false;
    for (const QJsonObject &d : devices) {
        const QString mac = d["device"].toString();
        if (!deviceStates.contains(mac)) continue;

        for (const QJsonValue &cval : deviceStates[mac]) {
            const QJsonObject cap = cval.toObject();
            if (cap["instance"].toString() != "powerSwitch") continue;

            const QJsonObject state = cap["state"].toObject();
            if (parsePowerState(state["value"])) {
                groupIsOn = true;
                break;
            }
        }
        if (groupIsOn) break;
    }

    QPushButton *groupPower = new QPushButton(groupIsOn ? "Turn Group Off" : "Turn Group On");
    groupPower->setCheckable(true);
    groupPower->setChecked(groupIsOn);
    groupPower->setProperty("role", "groupPower");
    QStringList groupMacs;
    for (const QJsonObject &d : devices) groupMacs << d["device"].toString();
    groupPower->setProperty("deviceMacs", groupMacs);
    groupPower->setMinimumHeight(55);
    connect(groupPower, &QPushButton::toggled, this, [=](bool on){
        groupPower->setText(on ? "Turn Group Off" : "Turn Group On");
        for (const auto &d : devices) {
            sendCommand(d["device"].toString(), d["sku"].toString(),
                        "devices.capabilities.on_off", "powerSwitch", on ? 1 : 0);
            setDevicePowerState(d["device"].toString(), on);
        }
        refreshPowerButtons();
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

// ==========================================================================
// ROUTINES TAB — ADVANCED
// ==========================================================================

QWidget* MainWindow::createRoutinesTab()
{
    QWidget *w = new QWidget;
    QVBoxLayout *l = new QVBoxLayout(w);

    l->addWidget(new QLabel("<h2>Daily Routines</h2>"));

    QPushButton *add = new QPushButton("Add Routine");
    add->setMinimumHeight(50);
    connect(add, &QPushButton::clicked, this, &MainWindow::addRoutine);
    l->addWidget(add);

    QPushButton *edit = new QPushButton("Edit Selected");
    edit->setMinimumHeight(44);
    connect(edit, &QPushButton::clicked, this, &MainWindow::editRoutine);
    l->addWidget(edit);

    routineList = new QListWidget;
    l->addWidget(routineList);
    refreshRoutineList();

    QPushButton *rem = new QPushButton("Remove Selected");
    rem->setStyleSheet("background:#ff5555;color:white;");
    connect(rem, &QPushButton::clicked, this, &MainWindow::removeRoutine);
    l->addWidget(rem);

    l->addStretch();
    return w;
}

// ==========================================================================
// ADD ROUTINE — SELECT DEVICES / GROUP / SETTINGS
// ==========================================================================

void MainWindow::addRoutine()
{
    Routine r;
    r.name = "My Schedule";
    r.time = QTime::currentTime();
    for (int day = 1; day <= 7; ++day) r.days << day;

    if (!openRoutineEditor(r, "Add Schedule"))
        return;

    routines.append(r);
    saveRoutines();
    refreshRoutineList();
}

void MainWindow::editRoutine()
{
    if (!routineList) return;

    const int row = routineList->currentRow();
    if (row < 0 || row >= routines.size()) return;

    Routine edited = routines[row];
    if (!openRoutineEditor(edited, "Edit Schedule"))
        return;

    routines[row] = edited;
    saveRoutines();
    refreshRoutineList();
    routineList->setCurrentRow(row);
}

bool MainWindow::openRoutineEditor(Routine &routine, const QString &title)
{
    QDialog d(this);
    d.setWindowTitle(title);
    d.resize(920, 560);
    QVBoxLayout *l = new QVBoxLayout(&d);

    QLineEdit *name = new QLineEdit(routine.name.trimmed().isEmpty() ? "Schedule" : routine.name);
    QTimeEdit *time = new QTimeEdit(routine.time.isValid() ? routine.time : QTime::currentTime());
    time->setDisplayFormat("HH:mm");

    l->addWidget(new QLabel("Schedule Name:"));
    l->addWidget(name);
    l->addWidget(new QLabel("Time:"));
    l->addWidget(time);

    l->addWidget(new QLabel("Days:"));
    QHBoxLayout *daysLayout = new QHBoxLayout;
    QList<QCheckBox*> dayChecks;
    for (int day = 1; day <= 7; ++day) {
        QCheckBox *cb = new QCheckBox(weekdayLabel(day));
        cb->setChecked(routine.days.isEmpty() || routine.days.contains(day));
        dayChecks << cb;
        daysLayout->addWidget(cb);
    }
    daysLayout->addStretch();
    l->addLayout(daysLayout);

    l->addWidget(new QLabel("Group Actions:"));
    QVBoxLayout *rowsLayout = new QVBoxLayout;
    l->addLayout(rowsLayout);

    struct RowWidgets {
        QWidget *container = nullptr;
        QComboBox *group = nullptr;
        QComboBox *power = nullptr;
        QCheckBox *useBrightness = nullptr;
        QSpinBox *brightness = nullptr;
        QCheckBox *useTemp = nullptr;
        QSpinBox *temperature = nullptr;
        QCheckBox *useColor = nullptr;
        QPushButton *color = nullptr;
    };

    QList<RowWidgets*> rows;

    auto setColorButton = [](QPushButton *btn, const QColor &color) {
        btn->setProperty("selectedColor", color);
        btn->setStyleSheet(QString("background:%1; color:%2;")
                               .arg(color.name(QColor::HexRgb))
                               .arg(color.lightness() < 128 ? "#ffffff" : "#000000"));
        btn->setText(color.name(QColor::HexRgb).toUpper());
    };

    auto addRow = [&](const RoutineDeviceSetting *preset = nullptr) {
        RowWidgets *row = new RowWidgets;
        row->container = new QWidget;
        QHBoxLayout *rowLayout = new QHBoxLayout(row->container);
        rowLayout->setContentsMargins(0, 0, 0, 0);

        row->group = new QComboBox;
        row->group->addItem("All Lights", "__all__");
        QStringList groups;
        for (const QJsonValue &v : std::as_const(deviceList))
            groups << roomForDevice(v.toObject());
        groups.removeDuplicates();
        groups.sort(Qt::CaseInsensitive);
        for (const QString &group : std::as_const(groups))
            row->group->addItem(group, group);

        row->power = new QComboBox;
        row->power->addItems({"No Power Change", "Turn On", "Turn Off"});

        row->useBrightness = new QCheckBox("Brightness");
        row->brightness = new QSpinBox;
        row->brightness->setRange(1, 100);
        row->brightness->setValue(100);
        row->brightness->setEnabled(false);
        connect(row->useBrightness, &QCheckBox::toggled, row->brightness, &QSpinBox::setEnabled);

        row->useTemp = new QCheckBox("Temp");
        row->temperature = new QSpinBox;
        row->temperature->setRange(2000, 9000);
        row->temperature->setSingleStep(100);
        row->temperature->setValue(4000);
        row->temperature->setEnabled(false);
        connect(row->useTemp, &QCheckBox::toggled, row->temperature, &QSpinBox::setEnabled);

        row->useColor = new QCheckBox("Color");
        row->color = new QPushButton("Pick Color");
        row->color->setEnabled(false);
        setColorButton(row->color, Qt::white);
        connect(row->useColor, &QCheckBox::toggled, row->color, &QPushButton::setEnabled);
        connect(row->color, &QPushButton::clicked, this, [=]() {
            const QColor current = row->color->property("selectedColor").value<QColor>();
            QColor chosen = QColorDialog::getColor(current, this, "Select Schedule Color");
            if (chosen.isValid()) setColorButton(row->color, chosen);
        });

        QPushButton *remove = new QPushButton("-");
        remove->setFixedWidth(36);
        connect(remove, &QPushButton::clicked, &d, [&, row]() {
            if (rows.size() == 1) return;
            rows.removeOne(row);
            rowsLayout->removeWidget(row->container);
            row->container->deleteLater();
            delete row;
        });

        rowLayout->addWidget(new QLabel("Group"));
        rowLayout->addWidget(row->group, 2);
        rowLayout->addWidget(row->power);
        rowLayout->addWidget(row->useBrightness);
        rowLayout->addWidget(row->brightness);
        rowLayout->addWidget(row->useTemp);
        rowLayout->addWidget(row->temperature);
        rowLayout->addWidget(row->useColor);
        rowLayout->addWidget(row->color);
        rowLayout->addWidget(remove);

        if (preset) {
            int idx = row->group->findData(preset->group);
            if (idx < 0) {
                const QString fallback = preset->group.isEmpty() ? "__all__" : preset->group;
                row->group->addItem(fallback == "__all__" ? "All Lights" : fallback, fallback);
                idx = row->group->count() - 1;
            }
            row->group->setCurrentIndex(idx);

            if (preset->usePower)
                row->power->setCurrentIndex(preset->power ? 1 : 2);

            row->useBrightness->setChecked(preset->useBrightness);
            row->brightness->setValue(preset->brightness);
            row->brightness->setEnabled(preset->useBrightness);

            row->useTemp->setChecked(preset->useTemp);
            row->temperature->setValue(preset->temperature);
            row->temperature->setEnabled(preset->useTemp);

            row->useColor->setChecked(preset->useColor);
            setColorButton(row->color, preset->color.isValid() ? preset->color : QColor(Qt::white));
            row->color->setEnabled(preset->useColor);
        }

        rowsLayout->addWidget(row->container);
        rows << row;
    };

    QPushButton *addTarget = new QPushButton("+ Add Group");
    connect(addTarget, &QPushButton::clicked, &d, [&, addRow]() { addRow(nullptr); });
    l->addWidget(addTarget, 0, Qt::AlignLeft);

    if (routine.settings.isEmpty()) {
        addRow(nullptr);
    } else {
        for (const RoutineDeviceSetting &setting : std::as_const(routine.settings))
            addRow(&setting);
    }

    QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    l->addWidget(bb);

    if (d.exec() != QDialog::Accepted) {
        qDeleteAll(rows);
        return false;
    }

    Routine updated;
    updated.time = time->time();
    updated.name = name->text().trimmed().isEmpty() ? "Schedule" : name->text().trimmed();

    for (int i = 0; i < dayChecks.size(); ++i) {
        if (dayChecks[i]->isChecked())
            updated.days << (i + 1);
    }
    if (updated.days.isEmpty()) {
        QMessageBox::warning(this, "Schedule", "Pick at least one day.");
        qDeleteAll(rows);
        return false;
    }

    for (RowWidgets *row : std::as_const(rows)) {
        if (row->group->currentIndex() < 0) continue;

        RoutineDeviceSetting s;
        s.group = row->group->currentData().toString();

        if (row->power->currentIndex() > 0) {
            s.usePower = true;
            s.power = (row->power->currentIndex() == 1) ? 1 : 0;
        }
        s.useBrightness = row->useBrightness->isChecked();
        s.brightness = row->brightness->value();
        s.useTemp = row->useTemp->isChecked();
        s.temperature = row->temperature->value();
        s.useColor = row->useColor->isChecked();
        s.color = row->color->property("selectedColor").value<QColor>();

        updated.settings.append(s);
    }

    qDeleteAll(rows);

    if (updated.settings.isEmpty()) {
        QMessageBox::warning(this, "Schedule", "Add at least one group action.");
        return false;
    }

    routine = updated;
    return true;
}

void MainWindow::refreshRoutineList()
{
    if (!routineList) return;

    routineList->clear();
    for (const Routine &r : routines) {
        routineList->addItem(QString("%1 | %2 | %3 | %4")
                                 .arg(r.time.toString("HH:mm"))
                                 .arg(formatDays(r.days))
                                 .arg(formatRoutineTargets(r.settings))
                                 .arg(r.name));
    }
}

// ==========================================================================
// EXECUTE ROUTINES — SEND ALL DEVICE SETTINGS
// ==========================================================================

void MainWindow::checkRoutines()
{
    const QDate today = QDate::currentDate();
    const int day = today.dayOfWeek();
    QTime now = QTime::currentTime();
    bool routineExecuted = false;
    for (const Routine &r : routines) {
        if (!r.days.contains(day)) continue;
        if (r.time.hour()==now.hour() && r.time.minute()==now.minute()) {
            routineExecuted = true;
            qDebug() << "Executing routine:" << r.name;
            for (const RoutineDeviceSetting &s : r.settings) {
                for (const QJsonValue &v : std::as_const(deviceList)) {
                    const QJsonObject dev = v.toObject();
                    if (s.group != "__all__" && roomForDevice(dev) != s.group) continue;

                    const QString mac = dev["device"].toString();
                    const QString sku = dev["sku"].toString();

                    if (s.usePower) sendCommand(mac, sku, "devices.capabilities.on_off", "powerSwitch", s.power);
                    if (s.usePower) setDevicePowerState(mac, s.power != 0);
                    if (s.useBrightness) sendCommand(mac, sku, "devices.capabilities.range", "brightness", s.brightness);
                    if (s.useTemp) sendCommand(mac, sku, "devices.capabilities.color_setting", "colorTemperatureK", s.temperature);
                    if (s.useColor) {
                        int rgb = (s.color.red()<<16)|(s.color.green()<<8)|s.color.blue();
                        sendCommand(mac, sku, "devices.capabilities.color_setting", "colorRgb", rgb);
                    }
                }
            }
        }
    }

    if (routineExecuted) {
        refreshPowerButtons();
        // Pull fresh device state shortly after routine commands so UI text matches real hardware state.
        QTimer::singleShot(1500, this, &MainWindow::refreshDevices);
    }
}

void MainWindow::removeRoutine()
{
    if (!routineList) return;

    const int row = routineList->currentRow();
    if (row < 0 || row >= routines.size()) return;

    routines.removeAt(row);
    saveRoutines();
    refreshRoutineList();
}

void MainWindow::checkPhonePresence()
{
    if (!pingProcess || pingProcess->state() != QProcess::NotRunning) return;

    connect(pingProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus) {
                const bool nowOnline = (exitCode == 0);

                if (!firstCheckDone) {
                    phoneWasOnline = nowOnline;
                    firstCheckDone = true;
                    return;
                }

                if (nowOnline == phoneWasOnline) return;

                phoneWasOnline = nowOnline;
                qDebug() << (nowOnline ? "Phone HOME" : "Phone AWAY");
                for (const QJsonValue &v : std::as_const(deviceList)) {
                    const QJsonObject dev = v.toObject();
                    sendCommand(dev["device"].toString(), dev["sku"].toString(),
                                "devices.capabilities.on_off", "powerSwitch",
                                nowOnline ? 1 : 0);
                    setDevicePowerState(dev["device"].toString(), nowOnline);
                }
                refreshPowerButtons();
            },
            Qt::SingleShotConnection);

    pingProcess->start("ping", QStringList() << "-c" << "1" << "-W" << "2" << phoneHost);
}

void MainWindow::buildUI()
{
    tabWidget->clear();

    QVector<QJsonObject> allLights;
    QMap<QString, QVector<QJsonObject>> groups;

    for (const QJsonValue &v : std::as_const(deviceList)) {
        const QJsonObject dev = v.toObject();
        allLights << dev;

        const QString room = dev["deviceName"]
                                 .toString()
                                 .split(' ', Qt::SkipEmptyParts)
                                 .value(0, "Other");
        groups[room] << dev;
    }

    {
        auto *sa = new QScrollArea;
        sa->setWidgetResizable(true);
        auto *content = new QWidget;
        auto *lay = new QVBoxLayout(content);
        lay->setContentsMargins(20, 20, 20, 20);
        lay->setSpacing(20);

        lay->addWidget(createGroupControl(allLights, "ALL LIGHTS"));
        for (const QJsonObject &d : std::as_const(allLights))
            lay->addWidget(createLightWidget(d));

        lay->addStretch();
        sa->setWidget(content);
        tabWidget->addTab(sa, "All Lights (" + QString::number(allLights.size()) + ")");
    }

    QStringList rooms = groups.keys();
    rooms.sort(Qt::CaseInsensitive);

    for (const QString &room : std::as_const(rooms)) {
        auto *sa = new QScrollArea;
        sa->setWidgetResizable(true);
        auto *content = new QWidget;
        auto *lay = new QVBoxLayout(content);
        lay->setContentsMargins(20, 20, 20, 20);
        lay->setSpacing(20);

        lay->addWidget(createGroupControl(groups[room], room + " - Group"));
        for (const QJsonObject &d : std::as_const(groups[room]))
            lay->addWidget(createLightWidget(d));

        lay->addStretch();
        sa->setWidget(content);
        tabWidget->addTab(sa, room + " (" + QString::number(groups[room].size()) + ")");
    }

    tabWidget->addTab(createRoutinesTab(), "Routines");
    refreshPowerButtons();
}

bool MainWindow::isDeviceOn(const QString &mac) const
{
    if (!deviceStates.contains(mac)) return false;

    const QJsonArray caps = deviceStates.value(mac);
    for (const QJsonValue &cval : caps) {
        const QJsonObject cap = cval.toObject();
        if (cap["instance"].toString() != "powerSwitch") continue;
        return parsePowerState(cap["state"].toObject()["value"]);
    }
    return false;
}

void MainWindow::setDevicePowerState(const QString &mac, bool on)
{
    QJsonArray caps = deviceStates.value(mac);
    bool found = false;

    for (int i = 0; i < caps.size(); ++i) {
        QJsonObject cap = caps[i].toObject();
        if (cap["instance"].toString() != "powerSwitch") continue;

        QJsonObject state = cap["state"].toObject();
        state["value"] = on ? 1 : 0;
        cap["state"] = state;
        caps[i] = cap;
        found = true;
        break;
    }

    if (!found) {
        QJsonObject cap;
        cap["instance"] = "powerSwitch";
        QJsonObject state;
        state["value"] = on ? 1 : 0;
        cap["state"] = state;
        caps.append(cap);
    }

    deviceStates[mac] = caps;
}

void MainWindow::refreshPowerButtons()
{
    const QList<QPushButton*> buttons = findChildren<QPushButton*>();
    for (QPushButton *btn : buttons) {
        const QString role = btn->property("role").toString();
        if (role == "lightPower") {
            const QString mac = btn->property("deviceMac").toString();
            const bool on = isDeviceOn(mac);
            QSignalBlocker blocker(btn);
            btn->setChecked(on);
            btn->setText(on ? "Turn Off" : "Turn On");
        } else if (role == "groupPower") {
            const QStringList macs = btn->property("deviceMacs").toStringList();
            bool anyOn = false;
            for (const QString &mac : macs) {
                if (isDeviceOn(mac)) {
                    anyOn = true;
                    break;
                }
            }
            QSignalBlocker blocker(btn);
            btn->setChecked(anyOn);
            btn->setText(anyOn ? "Turn Group Off" : "Turn Group On");
        }
    }
}
