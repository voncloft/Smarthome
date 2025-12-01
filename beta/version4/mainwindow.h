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

QT_FORWARD_DECLARE_CLASS(QNetworkAccessManager)

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void refreshAll();
    void checkPhonePresence();

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

    // Phone presence
    void startPhoneMonitoring();
    void turnAllLightsOff();
    void turnAllLightsOn();

    // API key
    QString loadApiKey();
    bool    saveApiKey(const QString &key);
    void    promptAndSetApiKey(bool force = false);

    QNetworkAccessManager *nam = nullptr;
    QTabWidget            *tabWidget = nullptr;
    QAction               *refreshAction = nullptr;

    QString               apiKey;
    QJsonArray            deviceList;
    QMap<QString, QJsonArray> deviceStates;
    int                   pendingStates = 0;

    // Phone presence
    QTimer                *phoneTimer = nullptr;
    QString               phoneHostname = "phone";  // CHANGE THIS TO YOUR PHONE'S HOSTNAME
    bool                  phoneWasOnline = true;
    QMap<QString, QJsonObject> lastKnownStates;
};

#endif // MAINWINDOW_H
