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

class QNetworkAccessManager;
class QPushButton;

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
    QList<int> days; // Qt dayOfWeek: 1=Mon ... 7=Sun
    QList<RoutineDeviceSetting> settings;
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
    QWidget* createGroupControl(const QVector<QJsonObject> &devices, const QString &title);
    QWidget* createRoutinesTab();

    bool loadApiKey();
    void promptForApiKey();
    void loadRoutines();
    void saveRoutines() const;
    void refreshRoutineList();
    bool openRoutineEditor(Routine &routine, const QString &title);

private:
    QNetworkAccessManager *nam = nullptr;
    QTabWidget *tabWidget = nullptr;

    // NEW refresh button
    QPushButton *refreshBtn = nullptr;

    QTimer *presenceTimer = nullptr;
    QTimer *routineTimer = nullptr;

    QProcess *pingProcess = nullptr;
    QString phoneHost = "192.168.42.2";
    bool phoneWasOnline = false;
    bool firstCheckDone = false;

    QString apiKey;

    QJsonArray deviceList;
    QMap<QString, QJsonArray> deviceStates;
    int pendingStates = 0;

    QList<Routine> routines;
    QListWidget *routineList = nullptr;
};

#endif
