#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QJsonArray>
#include <QMap>
#include <QJsonObject>
#include <QTabWidget>
#include <QTimer>
#include <QProcess>
#include <QListWidget>
#include <QColor>
#include <QSet>
#include <QtGlobal>

class QNetworkAccessManager;
class QPushButton;
class QLabel;
class QAudioSource;
class QIODevice;

struct RoutineDeviceSetting {
    QString group; // "__all__" or room key from device name prefix

    bool usePower = false;
    int power = 1;

    bool useBrightness = false;
    int brightness = 100;

    bool useTemp = false;
    int temperature = 4000;

    bool useColor = false;
    QColor color = Qt::white;
};

struct Routine {
    QTime time;
    QString name;
    bool enabled = true;
    QList<int> days; // Qt dayOfWeek: 1=Mon ... 7=Sun
    int phoneCondition = 0; // 0=Any, 1=Pingable, 2=Not Pingable
    QList<RoutineDeviceSetting> settings;
};

struct GroupTabSetting {
    int brightness = 100;
    int temperature = 4000;
    QColor color = Qt::white;
};

struct RoutineVerifyTarget {
    QString mac;
    QString sku;
    QString sourceTag = "[MANUAL]";
    bool expectPower = false;
    int power = 1;
    bool expectBrightness = false;
    int brightness = 100;
    bool expectTemp = false;
    int temperature = 4000;
    bool expectColor = false;
    QColor color = Qt::white;
    int retriesRemaining = 12;

    bool hasObservedPower = false;
    bool observedPowerOn = false;
    bool hasObservedBrightness = false;
    int observedBrightness = -1;
    bool hasObservedTemp = false;
    int observedTemp = -1;
    bool hasObservedColor = false;
    QColor observedColor = Qt::black;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void checkPhonePresence();
    void checkRoutines();
    void addRoutine();
    void editRoutine();
    void removeRoutine();
    void changeApiKey();

    // NEW:
    void refreshDevices();

private:
    void loadDevices();
    void onStateFinished();
    void sendCommand(const QString &device, const QString &sku,
                     const QString &type, const QString &instance,
                     const QVariant &value);

    void buildUI();
    bool isDeviceOn(const QString &mac) const;
    void setDevicePowerState(const QString &mac, bool on);
    void refreshPowerButtons();
    GroupTabSetting effectiveGroupTabSetting(const QString &groupKey) const;
    void applyGroupTabSettingToDevice(const QString &mac, const QString &sku, const QString &groupKey);

    QWidget* createLightWidget(const QJsonObject &dev);
    QWidget* createGroupControl(const QVector<QJsonObject> &devices, const QString &title, const QString &groupKey);
    QWidget* createRoutinesTab();
    QWidget* createDiagnosticsTab();
    QWidget* createConfigTab();

    bool loadApiKey();
    void promptForApiKey();
    void loadPresenceSettings();
    void savePresenceSettings() const;
    bool isPresenceAutoOnEnabled(const QString &groupKey) const;
    bool isPresenceAutoOffEnabled(const QString &groupKey) const;
    bool isAudioReactiveEnabled(const QString &groupKey) const;
    void showWarningMessage(const QString &title, const QString &text) const;
    void showCriticalMessage(const QString &title, const QString &text) const;
    void showInformationMessage(const QString &title, const QString &text);
    bool hasAnyAudioReactiveEnabled() const;
    void ensureAudioReactiveRunning();
    void startAudioReactiveCapture();
    void stopAudioReactiveCapture();
    void processAudioReactiveTick();
    void loadRoutines();
    void saveRoutines() const;
    void refreshRoutineList();
    void refreshRoutineVerifyDiagnostics();
    QString lightLabelForMac(const QString &mac) const;
    void addRoutineVerifyRecent(const QString &sourceTag, const QString &entry);
    bool openRoutineEditor(Routine &routine, const QString &title);
    void enqueueRoutineVerification(const QString &mac, const QString &sku,
                                    bool expectPower, int power,
                                    bool expectBrightness, int brightness,
                                    bool expectTemp, int temperature,
                                    bool expectColor, const QColor &color,
                                    const QString &sourceTag = QString("[MANUAL]"));
    void processRoutineVerificationTick();
    void verifyRoutineTargetNow(const QString &mac);

private:
    QNetworkAccessManager *nam = nullptr;
    QTabWidget *tabWidget = nullptr;

    // NEW refresh button
    QPushButton *refreshBtn = nullptr;

    QTimer *presenceTimer = nullptr;
    QTimer *routineTimer = nullptr;
    QTimer *routineVerifyTimer = nullptr;
    QTimer *audioReactiveTimer = nullptr;

    QProcess *pingProcess = nullptr;
    QProcess *pulseMonitorProcess = nullptr;
    QAudioSource *audioSource = nullptr;
    QIODevice *audioInputStream = nullptr;
    bool usePulseMonitorProcess = false;
    QString pulseMonitorSource;
    QString phoneHost = "192.168.42.2";
    bool phoneWasOnline = false;
    bool firstCheckDone = false;
    int presenceOnlineStreak = 0;
    int presenceOfflineStreak = 0;

    QString apiKey;

    QJsonArray deviceList;
    QMap<QString, QJsonArray> deviceStates;
    int pendingStates = 0;

    QList<Routine> routines;
    QListWidget *routineList = nullptr;
    QLabel *routineVerifySummaryLabel = nullptr;
    QListWidget *routineVerifyList = nullptr;
    bool presenceAutoOnAllGroups = true;
    bool presenceAutoOffAllGroups = false;
    bool audioReactiveAllGroups = false;
    bool hideMessageBoxes = false;
    QMap<QString, bool> presenceAutoOnGroupEnabled;
    QMap<QString, bool> presenceAutoOffGroupEnabled;
    QMap<QString, bool> audioReactiveGroupEnabled;
    QMap<QString, GroupTabSetting> groupTabSettings;
    QMap<QString, int> audioReactiveLastBrightness;
    QMap<QString, qint64> audioReactiveLastCommandMs;
    int audioReactiveIntervalMs = 100;
    int audioReactivePerDeviceMinMs = 7000;
    int audioReactiveGlobalMinMs = 500;
    int audioReactiveMaxCommandsPerTick = 1;
    int audioReactiveBrightnessDeadband = 2;
    int audioReactiveDispatchOffset = 0;
    double audioReactiveSmoothedLevel = 0.0;
    double audioReactiveNoiseFloor = 0.01;
    double audioReactivePeakLevel = 0.08;
    qint64 audioReactiveLastDiagnosticsMs = 0;
    qint64 audioReactiveLastHeartbeatMs = 0;
    qint64 audioReactiveLastGlobalCommandMs = 0;
    QMap<QString, RoutineVerifyTarget> routineVerifyTargets;
    QSet<QString> routineVerifyInFlight;
    QStringList routineVerifyRecentEntries;
};

#endif
