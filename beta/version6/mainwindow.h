#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QJsonArray>
#include <QMap>
#include <QJsonObject>
#include <QTabWidget>
#include <QAction>
#include <QTimer>
#include <QProcess>
#include <QDateTime>

QT_FORWARD_DECLARE_CLASS(QNetworkAccessManager)

struct Action {
    QString deviceMac;
    QString sku;
    QString type;           // "on_off", "brightness", "colorRgb", "colorTemperatureK"
    QVariant value;
    QString displayName;    // e.g. "Kitchen Light → On"
};

struct Routine {
    QString id;
    QString name;
    QVector<Action> actions;
    QString schedule = "none";  // "daily:16:00", "weekdays:07:30", etc.
    bool runOnPhoneHome = false;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void refreshAll();
    void checkPhonePresence();
    void checkScheduledRoutines();
    void openRoutineEditor(const QString &routineId = QString());
    void deleteRoutine();
    void runRoutineNow(QListWidgetItem *item);
    void addActionToCurrentRoutine(const QString &mac, const QString &name);

private:
    void createMenusAndToolbar();
    void loadDevices();
    void refreshAllDeviceStates();
    void onStateFinished();
    void sendCommand(const QString &device, const QString &sku,
                     const QString &type, const QString &instance,
                     const QVariant &value);
    void buildUI();

    QWidget* createGroupControl(const QVector<QJsonObject>& devices, const QString& title);
    QWidget* createLightWidget(const QJsonObject& dev);
    QWidget* createRoutinesTab();

    void loadRoutines();
    void saveRoutines();
    void executeRoutine(const Routine& r);

    void startPhoneMonitoring();

    QString loadApiKey();
    bool saveApiKey(const QString &key);
    void promptAndSetApiKey(bool force = false);

    QNetworkAccessManager *nam = nullptr;
    QTabWidget            *tabWidget = nullptr;
    QAction               *refreshAction = nullptr;

    QString               apiKey;
    QJsonArray            deviceList;
    QMap<QString, QJsonArray> deviceStates;
    int                   pendingStates = 0;

    QVector<Routine>      routines;
    QTimer               *routineTimer = nullptr;
    QTimer               *phoneTimer = nullptr;
    QString               phoneHostname = "phone";  // CHANGE TO YOUR PHONE (e.g. iPhone.local)
    bool                  phoneWasOnline = true;

    // Routine editor state
    QString               currentEditingRoutineId;
    QVector<Action>       currentRoutineActions;
};

#endif // MAINWINDOW_H
