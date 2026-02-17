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

class QNetworkAccessManager;
class QPushButton;
class QLabel;

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
    bool expectPower = false;
    int power = 1;
    bool expectBrightness = false;
    int brightness = 100;
    bool expectTemp = false;
    int temperature = 4000;
    bool expectColor = false;
    QColor color = Qt::white;
    int retriesRemaining = 12;
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
    void loadRoutines();
    void saveRoutines() const;
    void refreshRoutineList();
    void refreshRoutineVerifyDiagnostics();
    void addRoutineVerifyRecent(const QString &entry);
    bool openRoutineEditor(Routine &routine, const QString &title);
    void enqueueRoutineVerification(const QString &mac, const QString &sku,
                                    bool expectPower, int power,
                                    bool expectBrightness, int brightness,
                                    bool expectTemp, int temperature,
                                    bool expectColor, const QColor &color);
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

    QProcess *pingProcess = nullptr;
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
    QMap<QString, bool> presenceAutoOnGroupEnabled;
    QMap<QString, bool> presenceAutoOffGroupEnabled;
    QMap<QString, GroupTabSetting> groupTabSettings;
    QMap<QString, RoutineVerifyTarget> routineVerifyTargets;
    QSet<QString> routineVerifyInFlight;
    QStringList routineVerifyRecentEntries;
};

#endif
