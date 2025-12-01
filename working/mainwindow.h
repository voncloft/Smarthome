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

class QNetworkAccessManager;

struct Routine {
    QTime time;
    bool turnOn = true;
    QString name;
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
    void removeRoutine();

private:
    void loadDevices();
    void onStateFinished();
    void sendCommand(const QString &device, const QString &sku,
                     const QString &type, const QString &instance,
                     const QVariant &value);
    void buildUI();

    QWidget* createLightWidget(const QJsonObject &dev);
    QWidget* createGroupControl(const QVector<QJsonObject> &devices, const QString &title);
    QWidget* createRoutinesTab();

    bool loadApiKey();
    void promptForApiKey();

    QNetworkAccessManager *nam = nullptr;
    QTabWidget            *tabWidget = nullptr;

    QTimer    *presenceTimer = nullptr;
    QTimer    *routineTimer = nullptr;
    QProcess  *pingProcess = nullptr;
    QString    phoneHost = "phone";  // CHANGE TO YOUR PHONE IP OR HOSTNAME
    bool       phoneWasOnline = false;
    bool       firstCheckDone = false;

    QString               apiKey;
    QJsonArray            deviceList;
    QMap<QString, QJsonArray> deviceStates;
    int                   pendingStates = 0;

    QList<Routine>        routines;
    QListWidget          *routineList = nullptr;
};

#endif // MAINWINDOW_H
