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
#include <QFileInfo>
#include <QSaveFile>
#include <QDebug>
#include <QMenuBar>
#include <QAction>
#include <QUuid>
#include <QSignalBlocker>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <algorithm>
#include <cmath>
#include <cstring>

static bool parsePowerState(const QJsonValue &value)
{
    if (value.isBool()) return value.toBool();
    if (value.isDouble()) return value.toInt() != 0;

    const QString text = value.toString().trimmed().toLower();
    return text == "1" || text == "on" || text == "true";
}

static bool colorsClose(const QColor &a, const QColor &b, int tolerance = 4)
{
    return qAbs(a.red() - b.red()) <= tolerance
           && qAbs(a.green() - b.green()) <= tolerance
           && qAbs(a.blue() - b.blue()) <= tolerance;
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

static QString runCommandCaptureStdout(const QString &program, const QStringList &args, int timeoutMs = 1500)
{
    QProcess p;
    p.start(program, args);
    if (!p.waitForStarted(timeoutMs))
        return QString();
    p.closeWriteChannel();
    if (!p.waitForFinished(timeoutMs))
        return QString();
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

static QString detectPulseMonitorSource()
{
    const QString defaultSink = runCommandCaptureStdout("pactl", {"get-default-sink"});
    if (!defaultSink.isEmpty())
        return defaultSink + ".monitor";

    const QString sourceLines = runCommandCaptureStdout("pactl", {"list", "short", "sources"});
    const QStringList lines = sourceLines.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList parts = line.split('\t', Qt::SkipEmptyParts);
        if (parts.size() >= 2 && parts[1].contains(".monitor"))
            return parts[1].trimmed();
    }
    return QString();
}

static QString formatRoutineTargets(const QList<RoutineDeviceSetting> &settings)
{
    QStringList names;
    for (const RoutineDeviceSetting &s : settings) {
        const QString label = (s.group == "__all__" || s.group.isEmpty()) ? "all lights" : s.group;
        if (!names.contains(label)) names << label;
    }
    return names.isEmpty() ? "No targets" : names.join(", ");
}

static QString formatRoutineEffects(const QList<RoutineDeviceSetting> &settings)
{
    bool power = false;
    bool brightness = false;
    bool temp = false;
    bool color = false;
    bool sawPowerOn = false;
    bool sawPowerOff = false;
    QStringList colorValues;

    for (const RoutineDeviceSetting &s : settings) {
        power = power || s.usePower;
        brightness = brightness || s.useBrightness;
        temp = temp || s.useTemp;
        color = color || s.useColor;
        if (s.usePower) {
            if (s.power) sawPowerOn = true;
            else sawPowerOff = true;
        }
        if (s.useColor) {
            const QString hex = s.color.name(QColor::HexRgb).toUpper();
            if (!colorValues.contains(hex)) colorValues << hex;
        }
    }

    QStringList effects;
    if (power) {
        if (sawPowerOn && sawPowerOff) effects << "Power Mixed";
        else if (sawPowerOn) effects << "Power On";
        else effects << "Power Off";
    }
    if (brightness) effects << "Brightness";
    if (temp) effects << "Temp";
    if (color) {
        if (colorValues.size() == 1) effects << ("Color " + colorValues.first());
        else if (colorValues.size() > 1) effects << ("Color Mixed (" + colorValues.join("/") + ")");
        else effects << "Color";
    }
    return effects.isEmpty() ? "No effects" : effects.join(", ");
}

static QString roomForDevice(const QJsonObject &dev)
{
    return dev["deviceName"].toString().split(' ', Qt::SkipEmptyParts).value(0, "Other");
}

static QStringList configDirCandidates()
{
    const QString base = QDir::homePath() + "/.config";
    return {base + "/govee", base + "/Govee"};
}

static QString findConfigFileForRead(const QString &fileName)
{
    const QStringList dirs = configDirCandidates();
    for (const QString &dir : dirs) {
        const QString path = dir + "/" + fileName;
        if (QFileInfo::exists(path))
            return path;
    }
    return QString();
}

static QString resolveConfigDirForWrite()
{
    const QStringList dirs = configDirCandidates();
    const QStringList markerFiles = {"Lights.json", "app-settings.json", "routines.json", "api-key"};

    for (const QString &marker : markerFiles) {
        for (const QString &dir : dirs) {
            if (QFileInfo::exists(dir + "/" + marker))
                return dir;
        }
    }

    for (const QString &dir : dirs) {
        if (QFileInfo(dir).exists())
            return dir;
    }

    // Default for fresh installs.
    return dirs.first();
}

// ==========================================================================
// MAIN WINDOW Constructor
// ==========================================================================

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("Govee Lights — Auto + Routines");
    resize(1450, 950);

    QMenu *fileMenu = menuBar()->addMenu("File");
    QAction *changeApiKeyAction = fileMenu->addAction("Change API Key");
    connect(changeApiKeyAction, &QAction::triggered, this, &MainWindow::changeApiKey);

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
    routineVerifyTimer = new QTimer(this);
    audioReactiveTimer = new QTimer(this);
    routineVerifyTimer->setInterval(3000);
    audioReactiveTimer->setInterval(audioReactiveIntervalMs);

    connect(presenceTimer, &QTimer::timeout, this, &MainWindow::checkPhonePresence);
    connect(routineTimer, &QTimer::timeout, this, &MainWindow::checkRoutines);
    connect(routineVerifyTimer, &QTimer::timeout, this, &MainWindow::processRoutineVerificationTick);
    connect(audioReactiveTimer, &QTimer::timeout, this, &MainWindow::processAudioReactiveTick);

    presenceTimer->start(8000);     // unchanged
    routineTimer->start(60000);     // unchanged

    if (!loadApiKey()) promptForApiKey();
    if (apiKey.isEmpty()) {
        QMessageBox::critical(this, "Error", "API key required");
        QApplication::quit();
        return;
    }

    loadPresenceSettings();
    if (audioReactiveTimer)
        audioReactiveTimer->setInterval(audioReactiveIntervalMs);
    ensureAudioReactiveRunning();
    loadRoutines();
    buildUI(); // Show baseline tabs immediately, then populate device data asynchronously.
    loadDevices();
}

// ==========================================================================
// API KEY LOADING — unchanged
// ==========================================================================

bool MainWindow::loadApiKey()
{
    const QString settingsPath = findConfigFileForRead("Lights.json");
    QFile f(settingsPath);
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        f.close();
        if (doc.isObject()) {
            apiKey = doc.object().value("apiKey").toString().trimmed();
            if (!apiKey.isEmpty())
                return true;
        }
    }

    const QString legacyPath = findConfigFileForRead("api-key");
    QFile legacy(legacyPath);
    if (legacy.open(QIODevice::ReadOnly)) {
        apiKey = QString::fromUtf8(legacy.readAll()).trimmed();
        legacy.close();
        if (!apiKey.isEmpty())
            return true;
    }

    return false;
}

void MainWindow::promptForApiKey()
{
    changeApiKey();
}

void MainWindow::changeApiKey()
{
    bool ok = false;
    QString key = QInputDialog::getText(
        this,
        "Change API Key",
        "Enter API key:",
        QLineEdit::Password,
        apiKey,
        &ok
    );

    if (ok) {
        key = key.trimmed();
        if (key.isEmpty()) {
            QMessageBox::warning(this, "API Key", "API key cannot be empty.");
            return;
        }
        apiKey = key;
        QDir().mkpath(QDir::homePath() + "/.config/govee");
        savePresenceSettings();
        refreshDevices();
    }
}

void MainWindow::loadPresenceSettings()
{
    presenceAutoOnAllGroups = true;
    presenceAutoOffAllGroups = false;
    audioReactiveAllGroups = false;
    audioReactiveIntervalMs = 100;
    audioReactivePerDeviceMinMs = 7000;
    audioReactiveGlobalMinMs = 500;
    audioReactiveMaxCommandsPerTick = 1;
    audioReactiveBrightnessDeadband = 2;
    presenceAutoOnGroupEnabled.clear();
    presenceAutoOffGroupEnabled.clear();
    audioReactiveGroupEnabled.clear();
    groupTabSettings.clear();

    QFile f(findConfigFileForRead("Lights.json"));
    bool opened = f.open(QIODevice::ReadOnly);
    if (!opened) {
        f.setFileName(findConfigFileForRead("app-settings.json"));
        opened = f.open(QIODevice::ReadOnly);
    }
    if (opened) {
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        f.close();
        if (doc.isObject()) {
            const QJsonObject root = doc.object();
            if (apiKey.trimmed().isEmpty())
                apiKey = root.value("apiKey").toString().trimmed();
            phoneHost = root.value("phoneHost").toString(phoneHost).trimmed();
            if (phoneHost.isEmpty())
                phoneHost = "192.168.42.2";

            presenceAutoOnAllGroups = root.value("pingAutoOnAllGroups").toBool(true);
            presenceAutoOffAllGroups = root.value("pingAutoOffAllGroups").toBool(false);

            const QJsonObject onGroups = root.value("pingAutoOnGroups").toObject();
            for (auto it = onGroups.begin(); it != onGroups.end(); ++it)
                presenceAutoOnGroupEnabled[it.key()] = it.value().toBool(true);
            if (onGroups.contains("__all__"))
                presenceAutoOnAllGroups = onGroups.value("__all__").toBool(presenceAutoOnAllGroups);
            else if (onGroups.contains("ALL"))
                presenceAutoOnAllGroups = onGroups.value("ALL").toBool(presenceAutoOnAllGroups);

            const QJsonObject offGroups = root.value("pingAutoOffGroups").toObject();
            for (auto it = offGroups.begin(); it != offGroups.end(); ++it)
                presenceAutoOffGroupEnabled[it.key()] = it.value().toBool(false);
            if (offGroups.contains("__all__"))
                presenceAutoOffAllGroups = offGroups.value("__all__").toBool(presenceAutoOffAllGroups);
            else if (offGroups.contains("ALL"))
                presenceAutoOffAllGroups = offGroups.value("ALL").toBool(presenceAutoOffAllGroups);

            audioReactiveAllGroups = root.value("audioReactiveAllGroups").toBool(false);
            const QJsonObject audioGroups = root.value("audioReactiveGroups").toObject();
            for (auto it = audioGroups.begin(); it != audioGroups.end(); ++it)
                audioReactiveGroupEnabled[it.key()] = it.value().toBool(false);
            if (audioGroups.contains("__all__"))
                audioReactiveAllGroups = audioGroups.value("__all__").toBool(audioReactiveAllGroups);
            else if (audioGroups.contains("ALL"))
                audioReactiveAllGroups = audioGroups.value("ALL").toBool(audioReactiveAllGroups);
            audioReactiveIntervalMs = qBound(1, root.value("audioReactiveIntervalMs").toInt(100), 5000);
            audioReactivePerDeviceMinMs = qBound(100, root.value("audioReactivePerDeviceMinMs").toInt(7000), 60000);
            audioReactiveGlobalMinMs = qBound(10, root.value("audioReactiveGlobalMinMs").toInt(500), 10000);
            audioReactiveMaxCommandsPerTick = qBound(1, root.value("audioReactiveMaxCommandsPerTick").toInt(1), 20);
            audioReactiveBrightnessDeadband = qBound(1, root.value("audioReactiveBrightnessDeadband").toInt(2), 25);

            const QJsonObject tabSettings = root.value("tabSettings").toObject();
            for (auto it = tabSettings.begin(); it != tabSettings.end(); ++it) {
                const QJsonObject tab = it.value().toObject();
                GroupTabSetting s;
                s.brightness = qBound(1, tab.value("brightness").toInt(100), 100);
                s.temperature = qBound(2000, tab.value("temperature").toInt(4000), 9000);
                const QColor parsed = QColor(tab.value("color").toString("#ffffff"));
                s.color = parsed.isValid() ? parsed : QColor(Qt::white);
                groupTabSettings[it.key()] = s;
            }
            if (!groupTabSettings.contains("__all__") && groupTabSettings.contains("ALL"))
                groupTabSettings["__all__"] = groupTabSettings.value("ALL");
        }
    }
}

void MainWindow::savePresenceSettings() const
{
    const QString configDir = resolveConfigDirForWrite();
    QDir().mkpath(configDir);

    QJsonObject onGroups;
    for (auto it = presenceAutoOnGroupEnabled.begin(); it != presenceAutoOnGroupEnabled.end(); ++it)
        onGroups[it.key()] = it.value();
    QJsonObject offGroups;
    for (auto it = presenceAutoOffGroupEnabled.begin(); it != presenceAutoOffGroupEnabled.end(); ++it)
        offGroups[it.key()] = it.value();
    QJsonObject audioGroups;
    for (auto it = audioReactiveGroupEnabled.begin(); it != audioReactiveGroupEnabled.end(); ++it)
        audioGroups[it.key()] = it.value();

    // Persist explicit values for ALL and each current tab/group so Lights.json reflects current UI state.
    onGroups["__all__"] = presenceAutoOnAllGroups;
    onGroups["ALL"] = presenceAutoOnAllGroups;
    offGroups["__all__"] = presenceAutoOffAllGroups;
    offGroups["ALL"] = presenceAutoOffAllGroups;
    audioGroups["__all__"] = audioReactiveAllGroups;
    audioGroups["ALL"] = audioReactiveAllGroups;

    QStringList groups;
    for (const QJsonValue &v : std::as_const(deviceList)) {
        const QString group = roomForDevice(v.toObject());
        if (!group.isEmpty() && !groups.contains(group))
            groups << group;
    }
    for (const QString &group : std::as_const(groups)) {
        onGroups[group] = isPresenceAutoOnEnabled(group);
        offGroups[group] = isPresenceAutoOffEnabled(group);
        audioGroups[group] = isAudioReactiveEnabled(group);
    }

    QJsonObject tabSettings;
    auto writeTabSettings = [&tabSettings](const QString &key, const GroupTabSetting &s) {
        QJsonObject tab;
        tab["brightness"] = qBound(1, s.brightness, 100);
        tab["temperature"] = qBound(2000, s.temperature, 9000);
        tab["color"] = s.color.name(QColor::HexRgb);
        tabSettings[key] = tab;
    };

    const GroupTabSetting allSettings = groupTabSettings.value("__all__", GroupTabSetting{});
    writeTabSettings("__all__", allSettings);
    writeTabSettings("ALL", allSettings);
    for (const QString &group : std::as_const(groups))
        writeTabSettings(group, groupTabSettings.value(group, GroupTabSetting{}));

    QJsonObject root;
    root["version"] = 2;
    root["apiKey"] = apiKey.trimmed();
    root["phoneHost"] = phoneHost.trimmed();
    root["pingAutoOnAllGroups"] = presenceAutoOnAllGroups;
    root["pingAutoOffAllGroups"] = presenceAutoOffAllGroups;
    root["pingAutoOnGroups"] = onGroups;
    root["pingAutoOffGroups"] = offGroups;
    root["audioReactiveAllGroups"] = audioReactiveAllGroups;
    root["audioReactiveGroups"] = audioGroups;
    root["audioReactiveIntervalMs"] = qBound(1, audioReactiveIntervalMs, 5000);
    root["audioReactivePerDeviceMinMs"] = qBound(100, audioReactivePerDeviceMinMs, 60000);
    root["audioReactiveGlobalMinMs"] = qBound(10, audioReactiveGlobalMinMs, 10000);
    root["audioReactiveMaxCommandsPerTick"] = qBound(1, audioReactiveMaxCommandsPerTick, 20);
    root["audioReactiveBrightnessDeadband"] = qBound(1, audioReactiveBrightnessDeadband, 25);
    root["tabSettings"] = tabSettings;

    QSaveFile f(configDir + "/Lights.json");
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to open Lights settings file for writing";
        return;
    }

    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit())
        qWarning() << "Failed to commit Lights settings file";
}

bool MainWindow::isPresenceAutoOnEnabled(const QString &groupKey) const
{
    if (groupKey == "__all__")
        return presenceAutoOnAllGroups;
    if (presenceAutoOnGroupEnabled.contains(groupKey))
        return presenceAutoOnGroupEnabled.value(groupKey);
    return presenceAutoOnAllGroups;
}

bool MainWindow::isPresenceAutoOffEnabled(const QString &groupKey) const
{
    if (groupKey == "__all__")
        return presenceAutoOffAllGroups;
    if (presenceAutoOffGroupEnabled.contains(groupKey))
        return presenceAutoOffGroupEnabled.value(groupKey);
    return presenceAutoOffAllGroups;
}

bool MainWindow::isAudioReactiveEnabled(const QString &groupKey) const
{
    if (groupKey == "__all__")
        return audioReactiveAllGroups;
    if (audioReactiveGroupEnabled.contains(groupKey))
        return audioReactiveGroupEnabled.value(groupKey);
    return audioReactiveAllGroups;
}

bool MainWindow::hasAnyAudioReactiveEnabled() const
{
    if (audioReactiveAllGroups)
        return true;
    for (auto it = audioReactiveGroupEnabled.begin(); it != audioReactiveGroupEnabled.end(); ++it) {
        if (it.value())
            return true;
    }
    return false;
}

void MainWindow::ensureAudioReactiveRunning()
{
    if (hasAnyAudioReactiveEnabled())
        startAudioReactiveCapture();
    else
        stopAudioReactiveCapture();
}

void MainWindow::startAudioReactiveCapture()
{
    if ((audioSource && audioInputStream)
        || (usePulseMonitorProcess && pulseMonitorProcess && pulseMonitorProcess->state() == QProcess::Running)) {
        if (audioReactiveTimer && !audioReactiveTimer->isActive())
            audioReactiveTimer->start();
        return;
    }

    stopAudioReactiveCapture();

    const QList<QAudioDevice> devices = QMediaDevices::audioInputs();
    if (devices.isEmpty()) {
        const QString monitorSource = detectPulseMonitorSource();
        if (monitorSource.isEmpty()) {
            addRoutineVerifyRecent("[PULSEAUDIO]",
                                   "capture start failed: no Qt input devices and no Pulse monitor source");
            return;
        }

        pulseMonitorProcess = new QProcess(this);
        pulseMonitorProcess->setProcessChannelMode(QProcess::SeparateChannels);
        const QStringList args = {
            "--device", monitorSource,
            "--format=s16le",
            "--rate=48000",
            "--channels=2",
            "--latency-msec=20"
        };
        pulseMonitorProcess->start("parec", args);
        if (!pulseMonitorProcess->waitForStarted(2000)) {
            const QString err = QString::fromUtf8(pulseMonitorProcess->readAllStandardError()).trimmed();
            addRoutineVerifyRecent("[PULSEAUDIO]",
                                   QString("capture start failed via parec on '%1'%2")
                                       .arg(monitorSource)
                                       .arg(err.isEmpty() ? QString() : QString(" (%1)").arg(err)));
            pulseMonitorProcess->deleteLater();
            pulseMonitorProcess = nullptr;
            return;
        }

        usePulseMonitorProcess = true;
        pulseMonitorSource = monitorSource;
        audioReactiveSmoothedLevel = 0.0;
        audioReactiveLastBrightness.clear();
        audioReactiveLastHeartbeatMs = 0;
        addRoutineVerifyRecent("[PULSEAUDIO]",
                               QString("capture started via parec: source='%1', interval=%2ms")
                                   .arg(pulseMonitorSource)
                                   .arg(audioReactiveTimer ? audioReactiveTimer->interval() : -1));
        if (audioReactiveTimer && !audioReactiveTimer->isActive())
            audioReactiveTimer->start();
        return;
    }

    QAudioDevice selected;
    for (const QAudioDevice &dev : devices) {
        const QString desc = dev.description();
        if (desc.contains("monitor", Qt::CaseInsensitive)
            || desc.contains("loopback", Qt::CaseInsensitive)
            || desc.contains("stereo mix", Qt::CaseInsensitive)
            || desc.contains("what u hear", Qt::CaseInsensitive)) {
            selected = dev;
            break;
        }
    }
    if (selected.isNull())
        selected = QMediaDevices::defaultAudioInput();
    if (selected.isNull())
        selected = devices.first();

    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Int16);
    if (!selected.isFormatSupported(format))
        format = selected.preferredFormat();

    audioSource = new QAudioSource(selected, format, this);
    audioInputStream = audioSource->start();
    if (!audioInputStream) {
        addRoutineVerifyRecent("[PULSEAUDIO]",
                               QString("capture start failed on '%1'").arg(selected.description()));
        stopAudioReactiveCapture();
        return;
    }

    audioReactiveSmoothedLevel = 0.0;
    audioReactiveNoiseFloor = 0.01;
    audioReactivePeakLevel = 0.08;
    audioReactiveLastBrightness.clear();
    audioReactiveLastCommandMs.clear();
    audioReactiveDispatchOffset = 0;
    audioReactiveLastGlobalCommandMs = 0;
    audioReactiveLastHeartbeatMs = 0;
    addRoutineVerifyRecent("[PULSEAUDIO]",
                           QString("capture started: device='%1', interval=%2ms")
                               .arg(selected.description())
                               .arg(audioReactiveTimer ? audioReactiveTimer->interval() : -1));
    if (audioReactiveTimer && !audioReactiveTimer->isActive())
        audioReactiveTimer->start();
}

void MainWindow::stopAudioReactiveCapture()
{
    if (audioReactiveTimer && audioReactiveTimer->isActive())
        audioReactiveTimer->stop();
    if (pulseMonitorProcess) {
        if (pulseMonitorProcess->state() != QProcess::NotRunning) {
            pulseMonitorProcess->terminate();
            if (!pulseMonitorProcess->waitForFinished(500))
                pulseMonitorProcess->kill();
        }
        pulseMonitorProcess->deleteLater();
        pulseMonitorProcess = nullptr;
    }
    if (audioSource) {
        audioSource->stop();
        audioSource->deleteLater();
        audioSource = nullptr;
    }
    audioInputStream = nullptr;
    usePulseMonitorProcess = false;
    pulseMonitorSource.clear();
    audioReactiveSmoothedLevel = 0.0;
    audioReactiveNoiseFloor = 0.01;
    audioReactivePeakLevel = 0.08;
    audioReactiveLastBrightness.clear();
    audioReactiveLastCommandMs.clear();
    audioReactiveDispatchOffset = 0;
    audioReactiveLastGlobalCommandMs = 0;
    audioReactiveLastDiagnosticsMs = 0;
    audioReactiveLastHeartbeatMs = 0;
}

void MainWindow::processAudioReactiveTick()
{
    if (!hasAnyAudioReactiveEnabled()) {
        stopAudioReactiveCapture();
        return;
    }
    QByteArray data;
    int channels = 2;
    int bytesPerSample = 2;
    QAudioFormat::SampleFormat sampleFormat = QAudioFormat::Int16;

    if (usePulseMonitorProcess) {
        if (!pulseMonitorProcess || pulseMonitorProcess->state() != QProcess::Running)
            return;
        data = pulseMonitorProcess->readAllStandardOutput();
    } else {
        if (!audioSource || !audioInputStream)
            return;
        data = audioInputStream->readAll();
        const QAudioFormat format = audioSource->format();
        channels = qMax(1, format.channelCount());
        sampleFormat = format.sampleFormat();
        switch (sampleFormat) {
        case QAudioFormat::UInt8:
            bytesPerSample = 1;
            break;
        case QAudioFormat::Int16:
            bytesPerSample = 2;
            break;
        case QAudioFormat::Int32:
        case QAudioFormat::Float:
            bytesPerSample = 4;
            break;
        default:
            return;
        }
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (data.isEmpty()) {
        if ((nowMs - audioReactiveLastHeartbeatMs) >= 1500) {
            addRoutineVerifyRecent("[PULSEAUDIO]",
                                   usePulseMonitorProcess
                                       ? QString("tick: no audio data from source '%1'").arg(pulseMonitorSource)
                                       : "tick: no audio data available");
            audioReactiveLastHeartbeatMs = nowMs;
        }
        return;
    }

    const int frameBytes = bytesPerSample * channels;
    if (frameBytes <= 0 || data.size() < frameBytes)
        return;

    const int frames = data.size() / frameBytes;
    if (frames <= 0)
        return;

    const char *ptr = data.constData();
    double sumSquares = 0.0;
    int sampleCount = 0;
    for (int i = 0; i < frames * channels; ++i) {
        double sample = 0.0;
        switch (sampleFormat) {
        case QAudioFormat::UInt8:
            sample = (static_cast<unsigned char>(ptr[i]) - 128.0) / 128.0;
            break;
        case QAudioFormat::Int16: {
            qint16 v = 0;
            std::memcpy(&v, ptr + (i * bytesPerSample), sizeof(v));
            sample = static_cast<double>(v) / 32768.0;
            break;
        }
        case QAudioFormat::Int32: {
            qint32 v = 0;
            std::memcpy(&v, ptr + (i * bytesPerSample), sizeof(v));
            sample = static_cast<double>(v) / 2147483648.0;
            break;
        }
        case QAudioFormat::Float: {
            float v = 0.0f;
            std::memcpy(&v, ptr + (i * bytesPerSample), sizeof(v));
            sample = qBound(-1.0, static_cast<double>(v), 1.0);
            break;
        }
        default:
            break;
        }
        sumSquares += sample * sample;
        sampleCount++;
    }
    if (sampleCount == 0)
        return;

    const double rms = std::sqrt(sumSquares / static_cast<double>(sampleCount));

    // Adaptive dynamic range tracking: follow quiet/loud envelope to react to song changes.
    if (rms < audioReactiveNoiseFloor)
        audioReactiveNoiseFloor = (audioReactiveNoiseFloor * 0.92) + (rms * 0.08);
    else
        audioReactiveNoiseFloor = (audioReactiveNoiseFloor * 0.998) + (rms * 0.002);

    if (rms > audioReactivePeakLevel)
        audioReactivePeakLevel = (audioReactivePeakLevel * 0.85) + (rms * 0.15);
    else
        audioReactivePeakLevel = (audioReactivePeakLevel * 0.997) + (rms * 0.003);

    if (audioReactivePeakLevel < audioReactiveNoiseFloor + 0.01)
        audioReactivePeakLevel = audioReactiveNoiseFloor + 0.01;

    const double dynamicRange = qMax(0.01, audioReactivePeakLevel - audioReactiveNoiseFloor);
    const double normalized = qBound(0.0, (rms - audioReactiveNoiseFloor) / dynamicRange, 1.0);
    const double curved = std::pow(normalized, 0.70);

    // Faster attack/release than previous smoothing so verse/chorus changes are visible.
    audioReactiveSmoothedLevel = (audioReactiveSmoothedLevel * 0.55) + (curved * 0.45);
    const double factor = 0.03 + (0.97 * audioReactiveSmoothedLevel);

    struct PendingBrightness {
        QString mac;
        QString sku;
        int target = 1;
    };

    QVector<PendingBrightness> pending;
    int changedDevices = 0;
    int eligibleDevices = 0;
    int minTarget = 100;
    int maxTarget = 1;
    int sentMinTarget = 100;
    int sentMaxTarget = 1;
    for (const QJsonValue &v : std::as_const(deviceList)) {
        const QJsonObject dev = v.toObject();
        const QString room = roomForDevice(dev);
        if (!isAudioReactiveEnabled(room))
            continue;

        const QString mac = dev.value("device").toString();
        if (!isDeviceOn(mac))
            continue;
        eligibleDevices++;

        const QString sku = dev.value("sku").toString();
        const int base = qBound(1, effectiveGroupTabSetting(room).brightness, 100);
        const int target = qBound(1, static_cast<int>(std::lround(base * factor)), 100);
        const int previous = audioReactiveLastBrightness.value(mac, -1);
        if (previous >= 0 && qAbs(previous - target) < qBound(1, audioReactiveBrightnessDeadband, 25))
            continue;

        PendingBrightness p;
        p.mac = mac;
        p.sku = sku;
        p.target = target;
        pending.push_back(p);
        minTarget = qMin(minTarget, target);
        maxTarget = qMax(maxTarget, target);
    }

    // Safety dispatcher: keep audio-reactive updates responsive without flooding cloud API.
    const qint64 kPerDeviceMinMs = qBound(100, audioReactivePerDeviceMinMs, 60000);
    const qint64 kGlobalMinMs = qBound(10, audioReactiveGlobalMinMs, 10000);
    const int kMaxCommandsPerTick = qBound(1, audioReactiveMaxCommandsPerTick, 20);

    if (!pending.isEmpty()) {
        if (audioReactiveDispatchOffset >= pending.size())
            audioReactiveDispatchOffset = 0;

        int sentThisTick = 0;
        for (int i = 0; i < pending.size() && sentThisTick < kMaxCommandsPerTick; ++i) {
            const int idx = (audioReactiveDispatchOffset + i) % pending.size();
            const PendingBrightness &p = pending[idx];
            const qint64 lastPerDevice = audioReactiveLastCommandMs.value(p.mac, 0);
            if ((nowMs - lastPerDevice) < kPerDeviceMinMs)
                continue;
            if ((nowMs - audioReactiveLastGlobalCommandMs) < kGlobalMinMs)
                continue;

            sendCommand(p.mac, p.sku, "devices.capabilities.range", "brightness", p.target);
            audioReactiveLastBrightness[p.mac] = p.target;
            audioReactiveLastCommandMs[p.mac] = nowMs;
            audioReactiveLastGlobalCommandMs = nowMs;
            changedDevices++;
            sentMinTarget = qMin(sentMinTarget, p.target);
            sentMaxTarget = qMax(sentMaxTarget, p.target);
            sentThisTick++;
        }

        audioReactiveDispatchOffset = (audioReactiveDispatchOffset + 1) % pending.size();
    }

    if (changedDevices > 0) {
        if ((nowMs - audioReactiveLastDiagnosticsMs) >= 1200) {
            addRoutineVerifyRecent("[PULSEAUDIO]",
                                   QString("rms=%1 norm=%2 level=%3 -> adjusted %4 device(s), brightness %5-%6")
                                       .arg(QString::number(rms, 'f', 3))
                                       .arg(QString::number(normalized, 'f', 3))
                                       .arg(QString::number(audioReactiveSmoothedLevel, 'f', 3))
                                       .arg(changedDevices)
                                       .arg(sentMinTarget)
                                       .arg(sentMaxTarget));
            audioReactiveLastDiagnosticsMs = nowMs;
        }
    } else if ((nowMs - audioReactiveLastHeartbeatMs) >= 1500) {
        addRoutineVerifyRecent("[PULSEAUDIO]",
                               QString("tick: rms=%1 norm=%2 level=%3 floor=%4 peak=%5 eligible=%6, no brightness changes")
                                   .arg(QString::number(rms, 'f', 3))
                                   .arg(QString::number(normalized, 'f', 3))
                                   .arg(QString::number(audioReactiveSmoothedLevel, 'f', 3))
                                   .arg(QString::number(audioReactiveNoiseFloor, 'f', 3))
                                   .arg(QString::number(audioReactivePeakLevel, 'f', 3))
                                   .arg(eligibleDevices));
        audioReactiveLastHeartbeatMs = nowMs;
    }
}

void MainWindow::loadRoutines()
{
    routines.clear();

    QFile f(findConfigFileForRead("routines.json"));
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
        r.enabled = ro.value("enabled").toBool(true);
        r.phoneCondition = qBound(0, ro.value("phoneCondition").toInt(0), 2);
        if (r.phoneCondition == 0 && ro.value("requirePhoneOnline").toBool(false))
            r.phoneCondition = 1;
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
    const QString configDir = resolveConfigDirForWrite();
    QDir().mkpath(configDir);

    QJsonArray routineArray;
    for (const Routine &r : routines) {
        QJsonObject ro;
        ro["name"] = r.name;
        ro["time"] = r.time.toString("HH:mm");
        ro["enabled"] = r.enabled;
        ro["phoneCondition"] = qBound(0, r.phoneCondition, 2);

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
            // Keep UI usable even if fetch fails; show existing/empty tabs instead of a blank tab bar.
            pendingStates = 0;
            buildUI();
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject root = doc.object();
        const int code = root.contains("code") ? root.value("code").toInt(-1) : 200;
        if (code != 200) {
            qWarning() << "Device list API error code:" << code
                       << "message:" << root.value("message").toString();
            pendingStates = 0;
            buildUI();
            return;
        }

        auto extractDevices = [](const QJsonValue &value) -> QJsonArray {
            if (value.isArray())
                return value.toArray();
            if (!value.isObject())
                return {};

            const QJsonObject obj = value.toObject();
            if (obj.value("devices").isArray()) return obj.value("devices").toArray();
            if (obj.value("data").isArray()) return obj.value("data").toArray();
            if (obj.value("list").isArray()) return obj.value("list").toArray();
            if (obj.value("items").isArray()) return obj.value("items").toArray();
            return {};
        };

        deviceList = extractDevices(root.value("data"));
        if (deviceList.isEmpty())
            deviceList = extractDevices(root.value("payload"));
        if (deviceList.isEmpty())
            deviceList = extractDevices(root.value("devices"));

        pendingStates = deviceList.size();
        deviceStates.clear();

        if (pendingStates == 0) {
            qWarning() << "Device list parsed but empty.";
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
    const QString bulb = lightLabelForMac(device);
    QString valueText;
    if (value.typeId() == QMetaType::Bool)
        valueText = value.toBool() ? "true" : "false";
    else
        valueText = value.toString();

    addRoutineVerifyRecent("[COMMAND]",
                           QString("%1 -> %2=%3 -> %4/%5 -> queued")
                               .arg(bulb)
                               .arg(instance)
                               .arg(valueText)
                               .arg(type)
                               .arg(sku));

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

    QNetworkReply *reply = nam->post(req, QJsonDocument(root).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply, bulb, instance, valueText]() {
        const QString payload = QString("%1=%2").arg(instance, valueText);
        if (reply->error() != QNetworkReply::NoError) {
            addRoutineVerifyRecent("[COMMAND]",
                                   QString("%1 -> %2 -> failed (%3)")
                                       .arg(bulb)
                                       .arg(payload)
                                       .arg(reply->errorString()));
            reply->deleteLater();
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject root = doc.object();
        const int code = root.contains("code") ? root.value("code").toInt(-1) : 200;
        if (code != 200) {
            addRoutineVerifyRecent("[COMMAND]",
                                   QString("%1 -> %2 -> API code %3 (%4)")
                                       .arg(bulb)
                                       .arg(payload)
                                       .arg(code)
                                       .arg(root.value("message").toString()));
        } else {
            addRoutineVerifyRecent("[COMMAND]",
                                   QString("%1 -> %2 -> sent")
                                       .arg(bulb)
                                       .arg(payload));
        }
        reply->deleteLater();
    });
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
    const QString groupKey = roomForDevice(dev);

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
        if (on)
            applyGroupTabSettingToDevice(mac, sku, groupKey);
        addRoutineVerifyRecent("[MANUAL]",
                               QString("%1 -> power=%2 -> group=%3 -> command sent")
                                   .arg(name)
                                   .arg(on ? "on" : "off")
                                   .arg(groupKey));
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
        if (c.isValid()) {
            sendCommand(mac, sku, "devices.capabilities.color_setting", "colorRgb",
                        (c.red()<<16)|(c.green()<<8)|c.blue());
            if (!power->isChecked())
                power->setChecked(true);
            setDevicePowerState(mac, true);
            refreshPowerButtons();
        }
    });
    l->addWidget(cb);

    QSlider *ts = new QSlider(Qt::Horizontal);
    ts->setRange(2000,9000); ts->setValue(temp); ts->setEnabled(isOn);
    QLabel *tlbl = new QLabel(QString("Temp: %1K").arg(temp));
    connect(ts, &QSlider::valueChanged, [=](int v){
        tlbl->setText(QString("Temp: %1K").arg(v));
        sendCommand(mac, sku, "devices.capabilities.color_setting", "colorTemperatureK", v);
    });
    connect(power, &QPushButton::toggled, bs, &QWidget::setEnabled);
    connect(power, &QPushButton::toggled, cb, &QWidget::setEnabled);
    connect(power, &QPushButton::toggled, ts, &QWidget::setEnabled);
    l->addWidget(tlbl); l->addWidget(ts);

    return box;
}

// ==========================================================================
// GROUP CONTROL WIDGET
// ==========================================================================

QWidget* MainWindow::createGroupControl(const QVector<QJsonObject> &devices, const QString &title, const QString &groupKey)
{
    QGroupBox *box = new QGroupBox(title);
    QVBoxLayout *l = new QVBoxLayout(box);
    GroupTabSetting tabSetting = groupTabSettings.value(groupKey, GroupTabSetting{});

    QGroupBox *automationFrame = new QGroupBox("Automation");
    QVBoxLayout *automationLayout = new QVBoxLayout(automationFrame);

    QCheckBox *autoOnWhenPingable = new QCheckBox(QString("Turn ON when %1 is pingable").arg(phoneHost));
    autoOnWhenPingable->setChecked(isPresenceAutoOnEnabled(groupKey));
    connect(autoOnWhenPingable, &QCheckBox::toggled, this, [this, groupKey](bool enabled) {
        if (groupKey == "__all__")
            presenceAutoOnAllGroups = enabled;
        else
            presenceAutoOnGroupEnabled[groupKey] = enabled;
        savePresenceSettings();
    });
    automationLayout->addWidget(autoOnWhenPingable);

    QCheckBox *autoOffWhenNotPingable = new QCheckBox(QString("Turn OFF when %1 is NOT pingable").arg(phoneHost));
    autoOffWhenNotPingable->setChecked(isPresenceAutoOffEnabled(groupKey));
    connect(autoOffWhenNotPingable, &QCheckBox::toggled, this, [this, groupKey](bool enabled) {
        if (groupKey == "__all__")
            presenceAutoOffAllGroups = enabled;
        else
            presenceAutoOffGroupEnabled[groupKey] = enabled;
        savePresenceSettings();
    });
    automationLayout->addWidget(autoOffWhenNotPingable);

    QCheckBox *audioReactiveOverride = new QCheckBox("Pulse Audio Override");
    audioReactiveOverride->setChecked(isAudioReactiveEnabled(groupKey));
    connect(audioReactiveOverride, &QCheckBox::toggled, this, [this, groupKey](bool enabled) {
        if (groupKey == "__all__")
            audioReactiveAllGroups = enabled;
        else
            audioReactiveGroupEnabled[groupKey] = enabled;
        savePresenceSettings();
        ensureAudioReactiveRunning();
    });
    automationLayout->addWidget(audioReactiveOverride);

    QLabel *audioHint = new QLabel("Keeps color/temp scheme and adjusts only brightness from Pulse audio loudness.");
    audioHint->setWordWrap(true);
    automationLayout->addWidget(audioHint);

    l->addWidget(automationFrame);

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
            const QString mac = d["device"].toString();
            const QString sku = d["sku"].toString();
            sendCommand(mac, sku,
                        "devices.capabilities.on_off", "powerSwitch", on ? 1 : 0);
            setDevicePowerState(mac, on);
            if (on)
                applyGroupTabSettingToDevice(mac, sku, groupKey);
        }
        addRoutineVerifyRecent("[MANUAL]",
                               QString("%1 -> power=%2 -> devices=%3 -> command sent")
                                   .arg(groupKey == "__all__" ? "ALL LIGHTS" : groupKey)
                                   .arg(on ? "on" : "off")
                                   .arg(devices.size()));
        refreshPowerButtons();
    });
    l->addWidget(groupPower);

    QSlider *bri = new QSlider(Qt::Horizontal);
    bri->setRange(1,100);
    bri->setValue(tabSetting.brightness);
    QLabel *briLbl = new QLabel(QString("Group Brightness: %1%").arg(tabSetting.brightness));
    connect(bri, &QSlider::valueChanged, [=](int v){
        briLbl->setText(QString("Group Brightness: %1%").arg(v));
        for (const auto &d : devices)
            sendCommand(d["device"].toString(), d["sku"].toString(),
                        "devices.capabilities.range", "brightness", v);
        groupTabSettings[groupKey].brightness = v;
        savePresenceSettings();
    });
    l->addWidget(briLbl); l->addWidget(bri);

    QSlider *temp = new QSlider(Qt::Horizontal);
    temp->setRange(2000,9000);
    temp->setValue(tabSetting.temperature);
    QLabel *tempLbl = new QLabel(QString("Group Temp: %1K").arg(tabSetting.temperature));
    connect(temp, &QSlider::valueChanged, [=](int v){
        tempLbl->setText(QString("Group Temp: %1K").arg(v));
        for (const auto &d : devices)
            sendCommand(d["device"].toString(), d["sku"].toString(),
                        "devices.capabilities.color_setting", "colorTemperatureK", v);
        groupTabSettings[groupKey].temperature = v;
        savePresenceSettings();
    });
    l->addWidget(tempLbl); l->addWidget(temp);

    QPushButton *color = new QPushButton("Pick Group Color");
    auto setGroupColorButton = [](QPushButton *btn, const QColor &c) {
        btn->setProperty("selectedColor", c);
        btn->setText("Group Color " + c.name(QColor::HexRgb).toUpper());
        btn->setStyleSheet(QString("background:%1; color:%2;")
                               .arg(c.name(QColor::HexRgb))
                               .arg(c.lightness() < 128 ? "#ffffff" : "#000000"));
    };
    setGroupColorButton(color, tabSetting.color.isValid() ? tabSetting.color : QColor(Qt::white));
    connect(color, &QPushButton::clicked, [=]{
        const QColor current = color->property("selectedColor").value<QColor>();
        QColor c = QColorDialog::getColor(current, this);
        if (c.isValid()) {
            setGroupColorButton(color, c);
            int rgb = (c.red()<<16)|(c.green()<<8)|c.blue();
            for (const auto &d : devices) {
                sendCommand(d["device"].toString(), d["sku"].toString(),
                            "devices.capabilities.color_setting", "colorRgb", rgb);
                setDevicePowerState(d["device"].toString(), true);
            }
            groupTabSettings[groupKey].color = c;
            savePresenceSettings();
            refreshPowerButtons();
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
    connect(routineList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (!item || !routineList) return;
        const int row = routineList->row(item);
        if (row < 0 || row >= routines.size()) return;

        const bool enabled = item->checkState() == Qt::Checked;
        if (routines[row].enabled == enabled) return;

        routines[row].enabled = enabled;
        saveRoutines();
    });
    l->addWidget(routineList);
    refreshRoutineList();

    QPushButton *rem = new QPushButton("Remove Selected");
    rem->setStyleSheet("background:#ff5555;color:white;");
    connect(rem, &QPushButton::clicked, this, &MainWindow::removeRoutine);
    l->addWidget(rem);

    l->addStretch();
    return w;
}

QWidget* MainWindow::createDiagnosticsTab()
{
    QWidget *w = new QWidget;
    QVBoxLayout *l = new QVBoxLayout(w);

    l->addWidget(new QLabel("<h2>Routine Verification Diagnostics</h2>"));

    routineVerifySummaryLabel = new QLabel("No active routine verification checks.");
    routineVerifySummaryLabel->setWordWrap(true);
    l->addWidget(routineVerifySummaryLabel);

    routineVerifyList = new QListWidget;
    routineVerifyList->setSelectionMode(QAbstractItemView::NoSelection);
    routineVerifyList->setFocusPolicy(Qt::NoFocus);
    routineVerifyList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    l->addWidget(routineVerifyList);
    refreshRoutineVerifyDiagnostics();
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
    time->setTimeRange(QTime(0, 0), QTime(23, 59));
    time->setWrapping(true);

    l->addWidget(new QLabel("Schedule Name:"));
    l->addWidget(name);
    QCheckBox *enabled = new QCheckBox("Enabled");
    enabled->setChecked(routine.enabled);
    l->addWidget(enabled);
    l->addWidget(new QLabel("Time:"));
    l->addWidget(time);
    l->addWidget(new QLabel("Ping Condition:"));
    QComboBox *pingCondition = new QComboBox;
    pingCondition->addItem("Run regardless of ping", 0);
    pingCondition->addItem(QString("Only run when %1 is pingable").arg(phoneHost), 1);
    pingCondition->addItem(QString("Only run when %1 is NOT pingable").arg(phoneHost), 2);
    int pingIndex = pingCondition->findData(qBound(0, routine.phoneCondition, 2));
    if (pingIndex < 0) pingIndex = 0;
    pingCondition->setCurrentIndex(pingIndex);
    l->addWidget(pingCondition);

    l->addWidget(new QLabel("Days:"));
    QHBoxLayout *daysLayout = new QHBoxLayout;
    QList<QCheckBox*> dayChecks;
    QList<int> dayOrder = {7, 1, 2, 3, 4, 5, 6}; // Sun, Mon, Tue, Wed, Thu, Fri, Sat
    for (int day : std::as_const(dayOrder)) {
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
    updated.enabled = enabled->isChecked();
    updated.phoneCondition = pingCondition->currentData().toInt();

    for (int i = 0; i < dayChecks.size(); ++i) {
        if (dayChecks[i]->isChecked())
            updated.days << dayOrder[i];
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

    std::sort(routines.begin(), routines.end(), [](const Routine &a, const Routine &b) {
        if (a.time != b.time) return a.time < b.time;
        return a.name.toLower() < b.name.toLower();
    });

    QSignalBlocker blocker(routineList);
    routineList->clear();
    for (const Routine &r : routines) {
        const QString pingLabel = (r.phoneCondition == 1) ? "Pingable"
                                  : (r.phoneCondition == 2) ? "Not Pingable"
                                                            : "Any Ping State";
        QListWidgetItem *item = new QListWidgetItem(QString("%1 | %2 | %3 | %4 | %5 | %6")
                                                        .arg(r.time.toString("HH:mm"))
                                                        .arg(formatDays(r.days))
                                                        .arg(formatRoutineTargets(r.settings))
                                                        .arg(formatRoutineEffects(r.settings))
                                                        .arg(pingLabel)
                                                        .arg(r.name));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(r.enabled ? Qt::Checked : Qt::Unchecked);
        routineList->addItem(item);
    }
}

void MainWindow::refreshRoutineVerifyDiagnostics()
{
    if (!routineVerifySummaryLabel || !routineVerifyList)
        return;

    if (routineVerifyTargets.isEmpty())
        routineVerifySummaryLabel->setText(
            QString("No active checks. Recent results kept: %1.").arg(routineVerifyRecentEntries.size()));
    else
        routineVerifySummaryLabel->setText(
            QString("Active checks: %1 device(s). Poll interval: 3s. Recent results: %2.")
                .arg(routineVerifyTargets.size())
                .arg(routineVerifyRecentEntries.size()));

    routineVerifyList->clear();

    auto currentStateText = [](const RoutineVerifyTarget &t) -> QString {
        QStringList parts;
        if (t.hasObservedPower) parts << QString("power=%1").arg(t.observedPowerOn ? "on" : "off");
        if (t.hasObservedBrightness) parts << QString("brightness=%1").arg(t.observedBrightness);
        if (t.hasObservedColor) parts << QString("color=%1").arg(t.observedColor.name(QColor::HexRgb).toUpper());
        else if (t.hasObservedTemp) parts << QString("temp=%1K").arg(t.observedTemp);
        return parts.isEmpty() ? "unknown" : parts.join(", ");
    };
    auto expectedStateText = [](const RoutineVerifyTarget &t) -> QString {
        QStringList parts;
        if (t.expectPower) parts << QString("power=%1").arg(t.power ? "on" : "off");
        if (t.expectBrightness) parts << QString("brightness=%1").arg(t.brightness);
        if (t.expectColor) parts << QString("color=%1").arg(t.color.name(QColor::HexRgb).toUpper());
        else if (t.expectTemp) parts << QString("temp=%1K").arg(t.temperature);
        return parts.isEmpty() ? "none" : parts.join(", ");
    };

    const QStringList macs = routineVerifyTargets.keys();
    for (const QString &mac : macs) {
        const RoutineVerifyTarget t = routineVerifyTargets.value(mac);
        const QString status = routineVerifyInFlight.contains(mac)
                                   ? QString("not verified (checking, retries=%1)").arg(t.retriesRemaining)
                                   : QString("not verified (retries=%1)").arg(t.retriesRemaining);
        const QString sourceTag = t.sourceTag.trimmed().isEmpty() ? QString("[MANUAL]") : t.sourceTag;
        routineVerifyList->addItem(QString("%1 %2 -> %3 -> %4 -> %5")
                                       .arg(sourceTag)
                                       .arg(lightLabelForMac(mac))
                                       .arg(currentStateText(t))
                                       .arg(expectedStateText(t))
                                       .arg(status));
    }

    if (!routineVerifyRecentEntries.isEmpty()) {
        routineVerifyList->addItem("----- Recent Results -----");
        for (const QString &line : routineVerifyRecentEntries)
            routineVerifyList->addItem(line);
    }
}

QString MainWindow::lightLabelForMac(const QString &mac) const
{
    for (const QJsonValue &v : deviceList) {
        const QJsonObject dev = v.toObject();
        if (dev.value("device").toString() != mac)
            continue;
        const QString name = dev.value("deviceName").toString().trimmed();
        if (!name.isEmpty())
            return name;
        break;
    }
    return mac;
}

void MainWindow::addRoutineVerifyRecent(const QString &sourceTag, const QString &entry)
{
    const QString tag = sourceTag.trimmed().isEmpty() ? QString("[MANUAL]") : sourceTag;
    const QString line = QString("[%1] %2 %3")
                             .arg(QTime::currentTime().toString("HH:mm:ss"))
                             .arg(tag)
                             .arg(entry);
    routineVerifyRecentEntries.prepend(line);
    while (routineVerifyRecentEntries.size() > 500)
        routineVerifyRecentEntries.removeLast();
    refreshRoutineVerifyDiagnostics();
}

// ==========================================================================
// EXECUTE ROUTINES — SEND ALL DEVICE SETTINGS
// ==========================================================================

void MainWindow::checkRoutines()
{
    struct DeferredSetting {
        QString mac;
        QString sku;
        QString sourceTag = "[MANUAL]";
        bool usePower = false;
        int power = 1;
        bool useBrightness = false;
        int brightness = 100;
        bool useTemp = false;
        int temperature = 4000;
        bool useColor = false;
        QColor color = Qt::white;
    };

    QList<DeferredSetting> deferred;
    QMap<QString, DeferredSetting> desiredFinal;
    QStringList triggeredRoutineNames;

    const QDate today = QDate::currentDate();
    const int day = today.dayOfWeek();
    QTime now = QTime::currentTime();
    bool routineExecuted = false;
    for (const Routine &r : routines) {
        if (!r.enabled) continue;
        if (!r.days.contains(day)) continue;
        if (r.phoneCondition == 1 && !phoneWasOnline) continue;
        if (r.phoneCondition == 2 && phoneWasOnline) continue;
        if (r.time.hour()==now.hour() && r.time.minute()==now.minute()) {
            routineExecuted = true;
            if (!triggeredRoutineNames.contains(r.name))
                triggeredRoutineNames << r.name;
            qDebug() << "Executing routine:" << r.name;
            for (const RoutineDeviceSetting &s : r.settings) {
                for (const QJsonValue &v : std::as_const(deviceList)) {
                    const QJsonObject dev = v.toObject();
                    if (s.group != "__all__" && roomForDevice(dev) != s.group) continue;

                    const QString mac = dev["device"].toString();
                    const QString sku = dev["sku"].toString();
                    const bool preferColor = s.useColor;
                    const bool applyTemp = s.useTemp && !preferColor;
                    DeferredSetting &final = desiredFinal[mac];
                    final.mac = mac;
                    final.sku = sku;
                    final.sourceTag = QString("[ROUTINE:%1]").arg(r.name);
                    if (s.usePower) {
                        final.usePower = true;
                        final.power = (s.power != 0) ? 1 : 0;
                        if (final.power == 0) {
                            final.useBrightness = false;
                            final.useTemp = false;
                            final.useColor = false;
                        }
                    }
                    if (s.useBrightness && (!final.usePower || final.power != 0)) {
                        final.useBrightness = true;
                        final.brightness = s.brightness;
                    }
                    if (!final.usePower || final.power != 0) {
                        if (preferColor) {
                            final.useColor = true;
                            final.color = s.color;
                            final.useTemp = false;
                        } else if (applyTemp) {
                            final.useTemp = true;
                            final.temperature = s.temperature;
                            final.useColor = false;
                        }
                    }

                    if (s.usePower) sendCommand(mac, sku, "devices.capabilities.on_off", "powerSwitch", s.power);
                    if (s.usePower) setDevicePowerState(mac, s.power != 0);

                    const bool hasNonPower = s.useBrightness || s.useTemp || s.useColor;
                    const bool turningOnNow = s.usePower && s.power != 0;
                    const bool turningOffNow = s.usePower && s.power == 0;

                    // If we just turned a light on, defer routine color/brightness/temp slightly
                    // so the device accepts the exact scheduled values.
                    if (turningOnNow && hasNonPower) {
                        DeferredSetting d;
                        d.mac = mac;
                        d.sku = sku;
                        d.useBrightness = s.useBrightness;
                        d.brightness = s.brightness;
                        d.useTemp = applyTemp;
                        d.temperature = s.temperature;
                        d.useColor = preferColor;
                        d.color = s.color;
                        deferred.append(d);
                    } else if (!turningOffNow) {
                        if (s.useBrightness) sendCommand(mac, sku, "devices.capabilities.range", "brightness", s.brightness);
                        if (preferColor) {
                            int rgb = (s.color.red()<<16)|(s.color.green()<<8)|s.color.blue();
                            sendCommand(mac, sku, "devices.capabilities.color_setting", "colorRgb", rgb);
                        } else if (applyTemp) {
                            sendCommand(mac, sku, "devices.capabilities.color_setting", "colorTemperatureK", s.temperature);
                        }
                    }
                }
            }
        }
    }

    if (!deferred.isEmpty()) {
        QTimer::singleShot(1200, this, [this, deferred]() {
            for (const DeferredSetting &d : deferred) {
                if (d.useBrightness)
                    sendCommand(d.mac, d.sku, "devices.capabilities.range", "brightness", d.brightness);
                if (d.useColor) {
                    const int rgb = (d.color.red() << 16) | (d.color.green() << 8) | d.color.blue();
                    sendCommand(d.mac, d.sku, "devices.capabilities.color_setting", "colorRgb", rgb);
                    // Some bulbs ignore the first color command right after power-on; retry once.
                    QTimer::singleShot(900, this, [this, d, rgb]() {
                        if (d.useColor)
                            sendCommand(d.mac, d.sku, "devices.capabilities.color_setting", "colorRgb", rgb);
                    });
                } else if (d.useTemp) {
                    sendCommand(d.mac, d.sku, "devices.capabilities.color_setting", "colorTemperatureK", d.temperature);
                }
            }
        });
    }

    if (!desiredFinal.isEmpty()) {
        QTimer::singleShot(2500, this, [this, desiredFinal]() {
            for (auto it = desiredFinal.begin(); it != desiredFinal.end(); ++it) {
                const DeferredSetting &d = it.value();
                enqueueRoutineVerification(d.mac, d.sku,
                                           d.usePower, d.power,
                                           d.useBrightness, d.brightness,
                                           d.useTemp, d.temperature,
                                           d.useColor, d.color,
                                           d.sourceTag);
            }
            processRoutineVerificationTick();
            if (!routineVerifyTargets.isEmpty() && routineVerifyTimer && !routineVerifyTimer->isActive())
                routineVerifyTimer->start();
        });
    }

    if (routineExecuted) {
        if (!triggeredRoutineNames.isEmpty()) {
            QMessageBox *popup = new QMessageBox(QMessageBox::Information,
                                                 "Routine Triggered",
                                                 QString("Triggered routine(s): %1").arg(triggeredRoutineNames.join(", ")),
                                                 QMessageBox::Ok,
                                                 this);
            popup->setAttribute(Qt::WA_DeleteOnClose);
            popup->show();
        }
        refreshPowerButtons();
        // Pull fresh device state shortly after routine commands so UI text matches real hardware state.
        QTimer::singleShot(1500, this, &MainWindow::refreshDevices);
    }
}

void MainWindow::enqueueRoutineVerification(const QString &mac, const QString &sku,
                                            bool expectPower, int power,
                                            bool expectBrightness, int brightness,
                                            bool expectTemp, int temperature,
                                            bool expectColor, const QColor &color,
                                            const QString &sourceTag)
{
    if (mac.isEmpty() || sku.isEmpty())
        return;

    RoutineVerifyTarget target;
    target.mac = mac;
    target.sku = sku;
    target.sourceTag = sourceTag.trimmed().isEmpty() ? QString("[MANUAL]") : sourceTag.trimmed();
    target.expectPower = expectPower;
    target.power = (power != 0) ? 1 : 0;
    target.expectBrightness = expectBrightness;
    target.brightness = qBound(1, brightness, 100);
    target.expectColor = expectColor;
    target.color = color.isValid() ? color : QColor(Qt::white);
    target.expectTemp = expectTemp && !target.expectColor;
    target.temperature = qBound(2000, temperature, 9000);
    target.retriesRemaining = 12;

    if (target.expectPower && target.power == 0) {
        target.expectBrightness = false;
        target.expectTemp = false;
        target.expectColor = false;
    }

    if (routineVerifyTargets.contains(mac)) {
        RoutineVerifyTarget merged = routineVerifyTargets.value(mac);
        merged.sku = target.sku;
        merged.sourceTag = target.sourceTag;
        if (target.expectPower) {
            merged.expectPower = true;
            merged.power = target.power;
            if (merged.power == 0) {
                merged.expectBrightness = false;
                merged.expectTemp = false;
                merged.expectColor = false;
            }
        }
        if ((!merged.expectPower || merged.power != 0) && target.expectBrightness) {
            merged.expectBrightness = true;
            merged.brightness = target.brightness;
        }
        if (!merged.expectPower || merged.power != 0) {
            if (target.expectColor) {
                merged.expectColor = true;
                merged.color = target.color;
                merged.expectTemp = false;
            } else if (target.expectTemp) {
                merged.expectTemp = true;
                merged.temperature = target.temperature;
                merged.expectColor = false;
            }
        }
        merged.retriesRemaining = qMax(merged.retriesRemaining, target.retriesRemaining);
        routineVerifyTargets[mac] = merged;
    } else {
        routineVerifyTargets.insert(mac, target);
    }
    refreshRoutineVerifyDiagnostics();
}

void MainWindow::processRoutineVerificationTick()
{
    if (routineVerifyTargets.isEmpty()) {
        if (routineVerifyTimer && routineVerifyTimer->isActive())
            routineVerifyTimer->stop();
        refreshRoutineVerifyDiagnostics();
        return;
    }

    const QStringList macs = routineVerifyTargets.keys();
    for (const QString &mac : macs) {
        if (routineVerifyInFlight.contains(mac))
            continue;
        verifyRoutineTargetNow(mac);
    }

    if (routineVerifyTargets.isEmpty() && routineVerifyTimer && routineVerifyTimer->isActive())
        routineVerifyTimer->stop();
    refreshRoutineVerifyDiagnostics();
}

void MainWindow::verifyRoutineTargetNow(const QString &mac)
{
    if (!routineVerifyTargets.contains(mac))
        return;

    const RoutineVerifyTarget target = routineVerifyTargets.value(mac);
    if (target.mac.isEmpty() || target.sku.isEmpty()) {
        addRoutineVerifyRecent(target.sourceTag, QString("%1 -> unknown -> unknown -> not verified")
                                   .arg(lightLabelForMac(target.mac)));
        routineVerifyTargets.remove(mac);
        return;
    }

    routineVerifyInFlight.insert(mac);
    refreshRoutineVerifyDiagnostics();

    QJsonObject payload{
        {"sku", target.sku},
        {"device", target.mac}
    };

    QJsonObject root{
        {"requestId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"payload", payload}
    };

    QNetworkRequest req(QUrl("https://openapi.api.govee.com/router/api/v1/device/state"));
    req.setRawHeader("Govee-API-Key", apiKey.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply *reply = nam->post(req, QJsonDocument(root).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, mac, target, reply]() {
        routineVerifyInFlight.remove(mac);
        reply->deleteLater();

        if (!routineVerifyTargets.contains(mac))
            return;

        RoutineVerifyTarget current = routineVerifyTargets.value(mac);

        if (reply->error() != QNetworkReply::NoError) {
            current.retriesRemaining--;
            if (current.retriesRemaining <= 0) {
                addRoutineVerifyRecent(current.sourceTag, QString("%1 -> unknown -> unknown -> not verified")
                                           .arg(lightLabelForMac(current.mac)));
                routineVerifyTargets.remove(mac);
            } else {
                addRoutineVerifyRecent(current.sourceTag, QString("%1 -> unknown -> unknown -> not verified")
                                           .arg(lightLabelForMac(current.mac)));
                routineVerifyTargets[mac] = current;
            }
            refreshRoutineVerifyDiagnostics();
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject rootObj = doc.object();
        if (rootObj.value("code").toInt(-1) != 200) {
            current.retriesRemaining--;
            if (current.retriesRemaining <= 0) {
                addRoutineVerifyRecent(current.sourceTag, QString("%1 -> unknown -> unknown -> not verified")
                                           .arg(lightLabelForMac(current.mac)));
                routineVerifyTargets.remove(mac);
            } else {
                addRoutineVerifyRecent(current.sourceTag, QString("%1 -> unknown -> unknown -> not verified")
                                           .arg(lightLabelForMac(current.mac)));
                routineVerifyTargets[mac] = current;
            }
            refreshRoutineVerifyDiagnostics();
            return;
        }

        const QJsonArray caps = rootObj.value("payload").toObject().value("capabilities").toArray();
        bool hasPower = false;
        bool powerOn = false;
        bool hasBrightness = false;
        int brightness = -1;
        bool hasTemp = false;
        int temperature = -1;
        bool hasColor = false;
        QColor color = Qt::black;

        for (const QJsonValue &cv : caps) {
            const QJsonObject cap = cv.toObject();
            const QString inst = cap.value("instance").toString();
            const QJsonObject state = cap.value("state").toObject();
            const QJsonValue val = state.value("value");

            if (inst == "powerSwitch") {
                hasPower = true;
                powerOn = parsePowerState(val);
            } else if (inst == "brightness") {
                hasBrightness = true;
                brightness = val.toInt();
            } else if (inst == "colorTemperatureK") {
                hasTemp = true;
                temperature = val.toInt();
            } else if (inst == "colorRgb") {
                hasColor = true;
                const int rgb = val.toInt();
                color = QColor((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
            }
        }

        current.hasObservedPower = hasPower;
        current.observedPowerOn = powerOn;
        current.hasObservedBrightness = hasBrightness;
        current.observedBrightness = brightness;
        current.hasObservedTemp = hasTemp;
        current.observedTemp = temperature;
        current.hasObservedColor = hasColor;
        current.observedColor = color;

        bool ok = true;
        if (current.expectPower && (!hasPower || powerOn != (current.power != 0)))
            ok = false;
        if (ok && current.expectPower && current.power == 0) {
            QStringList cur;
            if (current.hasObservedPower) cur << QString("power=%1").arg(current.observedPowerOn ? "on" : "off");
            if (current.hasObservedBrightness) cur << QString("brightness=%1").arg(current.observedBrightness);
            if (current.hasObservedColor) cur << QString("color=%1").arg(current.observedColor.name(QColor::HexRgb).toUpper());
            else if (current.hasObservedTemp) cur << QString("temp=%1K").arg(current.observedTemp);
            QStringList exp;
            if (current.expectPower) exp << QString("power=%1").arg(current.power ? "on" : "off");
            addRoutineVerifyRecent(current.sourceTag, QString("%1 -> %2 -> %3 -> verified")
                                       .arg(lightLabelForMac(current.mac))
                                       .arg(cur.isEmpty() ? "unknown" : cur.join(", "))
                                       .arg(exp.isEmpty() ? "none" : exp.join(", ")));
            routineVerifyTargets.remove(mac);
            refreshRoutineVerifyDiagnostics();
            return;
        }
        if (ok && current.expectBrightness && (!hasBrightness || brightness != current.brightness))
            ok = false;
        if (ok && current.expectColor && (!hasColor || !colorsClose(color, current.color)))
            ok = false;
        if (ok && current.expectTemp && (!hasTemp || temperature != current.temperature))
            ok = false;

        if (ok) {
            QStringList cur;
            if (current.hasObservedPower) cur << QString("power=%1").arg(current.observedPowerOn ? "on" : "off");
            if (current.hasObservedBrightness) cur << QString("brightness=%1").arg(current.observedBrightness);
            if (current.hasObservedColor) cur << QString("color=%1").arg(current.observedColor.name(QColor::HexRgb).toUpper());
            else if (current.hasObservedTemp) cur << QString("temp=%1K").arg(current.observedTemp);
            QStringList exp;
            if (current.expectPower) exp << QString("power=%1").arg(current.power ? "on" : "off");
            if (current.expectBrightness) exp << QString("brightness=%1").arg(current.brightness);
            if (current.expectColor) exp << QString("color=%1").arg(current.color.name(QColor::HexRgb).toUpper());
            else if (current.expectTemp) exp << QString("temp=%1K").arg(current.temperature);
            addRoutineVerifyRecent(current.sourceTag, QString("%1 -> %2 -> %3 -> verified")
                                       .arg(lightLabelForMac(current.mac))
                                       .arg(cur.isEmpty() ? "unknown" : cur.join(", "))
                                       .arg(exp.isEmpty() ? "none" : exp.join(", ")));
            routineVerifyTargets.remove(mac);
            refreshRoutineVerifyDiagnostics();
            return;
        }

        if (current.expectPower && hasPower && powerOn != (current.power != 0))
            sendCommand(current.mac, current.sku, "devices.capabilities.on_off", "powerSwitch", current.power);
        if (current.expectBrightness && (!hasBrightness || brightness != current.brightness))
            sendCommand(current.mac, current.sku, "devices.capabilities.range", "brightness", current.brightness);
        if (current.expectColor && (!hasColor || !colorsClose(color, current.color))) {
            const int rgb = (current.color.red() << 16) | (current.color.green() << 8) | current.color.blue();
            sendCommand(current.mac, current.sku, "devices.capabilities.color_setting", "colorRgb", rgb);
        } else if (current.expectTemp && (!hasTemp || temperature != current.temperature)) {
            sendCommand(current.mac, current.sku, "devices.capabilities.color_setting", "colorTemperatureK", current.temperature);
        }

        current.retriesRemaining--;
        if (current.retriesRemaining <= 0) {
            QStringList cur;
            if (current.hasObservedPower) cur << QString("power=%1").arg(current.observedPowerOn ? "on" : "off");
            if (current.hasObservedBrightness) cur << QString("brightness=%1").arg(current.observedBrightness);
            if (current.hasObservedColor) cur << QString("color=%1").arg(current.observedColor.name(QColor::HexRgb).toUpper());
            else if (current.hasObservedTemp) cur << QString("temp=%1K").arg(current.observedTemp);
            QStringList exp;
            if (current.expectPower) exp << QString("power=%1").arg(current.power ? "on" : "off");
            if (current.expectBrightness) exp << QString("brightness=%1").arg(current.brightness);
            if (current.expectColor) exp << QString("color=%1").arg(current.color.name(QColor::HexRgb).toUpper());
            else if (current.expectTemp) exp << QString("temp=%1K").arg(current.temperature);
            addRoutineVerifyRecent(current.sourceTag, QString("%1 -> %2 -> %3 -> not verified")
                                       .arg(lightLabelForMac(current.mac))
                                       .arg(cur.isEmpty() ? "unknown" : cur.join(", "))
                                       .arg(exp.isEmpty() ? "none" : exp.join(", ")));
            routineVerifyTargets.remove(mac);
        } else {
            addRoutineVerifyRecent(current.sourceTag, QString("%1 -> %2 -> %3 -> not verified")
                                       .arg(lightLabelForMac(current.mac))
                                       .arg(current.hasObservedPower ? QString("power=%1").arg(current.observedPowerOn ? "on" : "off") : "unknown")
                                       .arg(current.expectPower ? QString("power=%1").arg(current.power ? "on" : "off") : "unknown"));
            routineVerifyTargets[mac] = current;
        }
        refreshRoutineVerifyDiagnostics();
    });
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

QWidget* MainWindow::createConfigTab()
{
    QWidget *w = new QWidget;
    QVBoxLayout *l = new QVBoxLayout(w);

    l->addWidget(new QLabel("<h2>Config</h2>"));

    QLabel *hint = new QLabel("Set ping host and audio reactive safety/rate settings.");
    hint->setWordWrap(true);
    l->addWidget(hint);

    QFormLayout *form = new QFormLayout;
    QLineEdit *hostEdit = new QLineEdit(phoneHost);
    hostEdit->setPlaceholderText("192.168.42.2");
    form->addRow("Ping Host/IP:", hostEdit);
    QSpinBox *audioIntervalEdit = new QSpinBox;
    audioIntervalEdit->setRange(1, 5000);
    audioIntervalEdit->setValue(qBound(1, audioReactiveIntervalMs, 5000));
    audioIntervalEdit->setSuffix(" ms");
    form->addRow("Audio Reactive Interval:", audioIntervalEdit);
    QSpinBox *audioPerDeviceMinEdit = new QSpinBox;
    audioPerDeviceMinEdit->setRange(100, 60000);
    audioPerDeviceMinEdit->setValue(qBound(100, audioReactivePerDeviceMinMs, 60000));
    audioPerDeviceMinEdit->setSuffix(" ms");
    form->addRow("Per-Device Min Gap:", audioPerDeviceMinEdit);
    QSpinBox *audioGlobalMinEdit = new QSpinBox;
    audioGlobalMinEdit->setRange(10, 10000);
    audioGlobalMinEdit->setValue(qBound(10, audioReactiveGlobalMinMs, 10000));
    audioGlobalMinEdit->setSuffix(" ms");
    form->addRow("Global Min Gap:", audioGlobalMinEdit);
    QSpinBox *audioMaxPerTickEdit = new QSpinBox;
    audioMaxPerTickEdit->setRange(1, 20);
    audioMaxPerTickEdit->setValue(qBound(1, audioReactiveMaxCommandsPerTick, 20));
    form->addRow("Max Commands/Tick:", audioMaxPerTickEdit);
    QSpinBox *audioDeadbandEdit = new QSpinBox;
    audioDeadbandEdit->setRange(1, 25);
    audioDeadbandEdit->setValue(qBound(1, audioReactiveBrightnessDeadband, 25));
    form->addRow("Brightness Deadband:", audioDeadbandEdit);
    l->addLayout(form);

    QLabel *rateHint = new QLabel("Recommended for Govee cloud API: Per-Device Min Gap >= 6000 ms (10 commands/minute limit per device).");
    rateHint->setWordWrap(true);
    l->addWidget(rateHint);

    QPushButton *save = new QPushButton("Save Config");
    save->setMinimumHeight(40);
    connect(save, &QPushButton::clicked, this, [this, hostEdit, audioIntervalEdit, audioPerDeviceMinEdit, audioGlobalMinEdit, audioMaxPerTickEdit, audioDeadbandEdit]() {
        const QString newHost = hostEdit->text().trimmed();
        if (newHost.isEmpty()) {
            QMessageBox::warning(this, "Config", "Ping host/IP cannot be empty.");
            return;
        }
        phoneHost = newHost;
        audioReactiveIntervalMs = qBound(1, audioIntervalEdit->value(), 5000);
        audioReactivePerDeviceMinMs = qBound(100, audioPerDeviceMinEdit->value(), 60000);
        audioReactiveGlobalMinMs = qBound(10, audioGlobalMinEdit->value(), 10000);
        audioReactiveMaxCommandsPerTick = qBound(1, audioMaxPerTickEdit->value(), 20);
        audioReactiveBrightnessDeadband = qBound(1, audioDeadbandEdit->value(), 25);
        if (audioReactiveTimer)
            audioReactiveTimer->setInterval(audioReactiveIntervalMs);
        savePresenceSettings(); // Stored in Lights.json (main settings file).
        buildUI(); // Rebuild labels that show the host value.
    });
    l->addWidget(save, 0, Qt::AlignLeft);

    l->addStretch();
    return w;
}

void MainWindow::checkPhonePresence()
{
    if (!pingProcess || pingProcess->state() != QProcess::NotRunning) return;
    static constexpr int kPresenceConfirmSamples = 2;

    connect(pingProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus) {
                const bool nowOnline = (exitCode == 0);
                if (nowOnline) {
                    presenceOnlineStreak++;
                    presenceOfflineStreak = 0;
                } else {
                    presenceOfflineStreak++;
                    presenceOnlineStreak = 0;
                }

                if (!firstCheckDone) {
                    // Apply presence rules on initial check too, so startup state is enforced.
                    firstCheckDone = true;
                    phoneWasOnline = nowOnline;
                } else {
                    if (nowOnline == phoneWasOnline) return;
                    if (nowOnline && presenceOnlineStreak < kPresenceConfirmSamples) return;
                    if (!nowOnline && presenceOfflineStreak < kPresenceConfirmSamples) return;
                }

                phoneWasOnline = nowOnline;
                qDebug() << (nowOnline ? "Phone HOME" : "Phone AWAY");
                addRoutineVerifyRecent("[PING]",
                                       QString("%1 -> ping=%2 -> host=%3 -> state changed")
                                           .arg("Presence")
                                           .arg(nowOnline ? "online" : "offline")
                                           .arg(phoneHost));

                bool changed = false;
                int affectedDevices = 0;
                for (const QJsonValue &v : std::as_const(deviceList)) {
                    const QJsonObject dev = v.toObject();
                    const QString mac = dev["device"].toString();
                    const QString name = dev["deviceName"].toString(mac).trimmed();
                    const QString room = roomForDevice(dev);
                    // Online/offline behavior supports a global ALL LIGHTS override plus per-room toggles.
                    const bool shouldApply = nowOnline
                                             ? (presenceAutoOnAllGroups
                                                || presenceAutoOnGroupEnabled.value(room, false))
                                             : (presenceAutoOffAllGroups
                                                || presenceAutoOffGroupEnabled.value(room, false));
                    if (!shouldApply) continue;
                    if (isDeviceOn(mac) == nowOnline) continue;

                    sendCommand(mac, dev["sku"].toString(),
                                "devices.capabilities.on_off", "powerSwitch",
                                nowOnline ? 1 : 0);
                    setDevicePowerState(mac, nowOnline);
                    if (nowOnline)
                        applyGroupTabSettingToDevice(mac, dev["sku"].toString(), room);
                    changed = true;
                    affectedDevices++;
                    addRoutineVerifyRecent("[PING]",
                                           QString("%1 -> power=%2 -> group=%3 -> command sent")
                                               .arg(name)
                                               .arg(nowOnline ? "on" : "off")
                                               .arg(room));
                }
                if (changed) {
                    addRoutineVerifyRecent("[PING]",
                                           QString("Presence -> applied to %1 device(s) -> host=%2 -> done")
                                               .arg(affectedDevices)
                                               .arg(phoneHost));
                    refreshPowerButtons();
                } else {
                    addRoutineVerifyRecent("[PING]",
                                           QString("Presence -> no eligible device changes -> host=%1")
                                               .arg(phoneHost));
                }
            },
            Qt::SingleShotConnection);

    pingProcess->start("ping", QStringList() << "-c" << "1" << "-W" << "2" << phoneHost);
}

void MainWindow::buildUI()
{
    const int previousIndex = tabWidget->currentIndex();
    const QString previousTabId = (previousIndex >= 0)
                                      ? tabWidget->tabBar()->tabData(previousIndex).toString()
                                      : QString();

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

    auto sortedDevicesByName = [](QVector<QJsonObject> devices) {
        std::sort(devices.begin(), devices.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return a.value("deviceName").toString().compare(
                       b.value("deviceName").toString(), Qt::CaseInsensitive) < 0;
        });
        return devices;
    };

    auto addTabWithId = [this](QWidget *page, const QString &title, const QString &id) {
        const int idx = tabWidget->addTab(page, title);
        tabWidget->tabBar()->setTabData(idx, id);
    };

    {
        auto *sa = new QScrollArea;
        sa->setWidgetResizable(true);
        auto *content = new QWidget;
        auto *lay = new QVBoxLayout(content);
        lay->setContentsMargins(20, 20, 20, 20);
        lay->setSpacing(20);

        lay->addWidget(createGroupControl(allLights, "ALL LIGHTS", "__all__"));
        QStringList groupedRooms = groups.keys();
        groupedRooms.sort(Qt::CaseInsensitive);
        for (const QString &room : std::as_const(groupedRooms)) {
            auto *roomBox = new QGroupBox(room + " (" + QString::number(groups[room].size()) + ")");
            auto *roomLayout = new QVBoxLayout(roomBox);
            roomLayout->setContentsMargins(12, 12, 12, 12);
            roomLayout->setSpacing(12);
            const QVector<QJsonObject> roomDevices = sortedDevicesByName(groups.value(room));
            for (const QJsonObject &d : roomDevices)
                roomLayout->addWidget(createLightWidget(d));
            lay->addWidget(roomBox);
        }

        lay->addStretch();
        sa->setWidget(content);
        addTabWithId(sa, "All Lights (" + QString::number(allLights.size()) + ")", "__tab_all_lights__");
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

        const QVector<QJsonObject> roomDevices = sortedDevicesByName(groups.value(room));
        lay->addWidget(createGroupControl(roomDevices, room + " - Group", room));
        for (const QJsonObject &d : roomDevices)
            lay->addWidget(createLightWidget(d));

        lay->addStretch();
        sa->setWidget(content);
        addTabWithId(sa, room + " (" + QString::number(groups[room].size()) + ")", "__tab_room__:" + room);
    }

    addTabWithId(createRoutinesTab(), "Routines", "__tab_routines__");
    addTabWithId(createDiagnosticsTab(), "Diagnostics", "__tab_diagnostics__");
    addTabWithId(createConfigTab(), "Config", "__tab_config__");

    if (!previousTabId.isEmpty()) {
        for (int i = 0; i < tabWidget->count(); ++i) {
            if (tabWidget->tabBar()->tabData(i).toString() == previousTabId) {
                tabWidget->setCurrentIndex(i);
                break;
            }
        }
    }

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

GroupTabSetting MainWindow::effectiveGroupTabSetting(const QString &groupKey) const
{
    if (groupTabSettings.contains(groupKey))
        return groupTabSettings.value(groupKey);
    return groupTabSettings.value("__all__", GroupTabSetting{});
}

void MainWindow::applyGroupTabSettingToDevice(const QString &mac, const QString &sku, const QString &groupKey)
{
    if (mac.isEmpty() || sku.isEmpty())
        return;

    const GroupTabSetting tab = effectiveGroupTabSetting(groupKey);
    const int brightness = qBound(1, tab.brightness, 100);
    const int temperature = qBound(2000, tab.temperature, 9000);
    const QColor color = tab.color.isValid() ? tab.color : QColor(Qt::white);
    const int rgb = (color.red() << 16) | (color.green() << 8) | color.blue();

    // Apply shortly after power-on so bulbs follow the tab/group profile instead of prior bulb state.
    QTimer::singleShot(350, this, [this, mac, sku, brightness, temperature, rgb]() {
        sendCommand(mac, sku, "devices.capabilities.range", "brightness", brightness);
        sendCommand(mac, sku, "devices.capabilities.color_setting", "colorTemperatureK", temperature);
        sendCommand(mac, sku, "devices.capabilities.color_setting", "colorRgb", rgb);

        // Some bulbs drop the first color write immediately after power-on.
        QTimer::singleShot(800, this, [this, mac, sku, rgb]() {
            sendCommand(mac, sku, "devices.capabilities.color_setting", "colorRgb", rgb);
        });
    });
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
