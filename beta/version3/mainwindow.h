#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QJsonArray>
#include <QMap>
#include <QTabWidget>
#include <QAction>
#include <QHash>

QT_FORWARD_DECLARE_CLASS(QNetworkAccessManager)
QT_FORWARD_DECLARE_CLASS(QTimer)

struct Light
{
    QString id;           // Govee: mac, Hue: light ID as string
    QString name;
    QString model;
    QString room;         // derived from name or Hue room
    QString type;         // "govee" or "hue"
    bool    online = true;
    bool    on = false;
    int     brightness = 100;
    int     temperature = 6500;  // Kelvin
    int     rgb = 0xFFFFFF;
};

struct Scene
{
    QString id;
    QString name;
    ;
    QString owner;      // "govee" or "hue"
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void refreshAll();
    void authenticateHueBridge();

private:
    void createMenusAndToolbar();
    void discoverHueBridge();
    void loadGoveeDevices();
    void loadHueLights();
    void refreshAllStates();
    void buildUI();

    QWidget* createGroupControl(const QString& roomName);
    QWidget* createLightWidget(const Light& light);
    QWidget* createScenesWidget(const QString& roomName);

    void sendGoveeCommand(const QString& mac, const QString& sku,
                          const QString& instance, const QVariant& value);
    void sendHueCommand(const QString& lightId, const QJsonObject& body);
    void activateGoveeScene(const QString& sceneId);
    void activateHueScene(const QString& sceneId);

    // API keys & auth
    QString loadGoveeKey();
    bool    saveGoveeKey(const QString& key);
    void    promptGoveeKey(bool force = false);

    QNetworkAccessManager *nam = nullptr;
    QTabWidget            *tabWidget = nullptr;
    QAction               *refreshAction = nullptr;

    // Data
    QVector<Light>   allLights;
    QSet<QString>    goveeSkus;           // to avoid duplicates
    QString          goveeApiKey;
    QString          hueBridgeIp;
    QString          hueUsername;         // created after push-link
    QTimer           *hueDiscoveryTimer = nullptr;

    // Scenes cache
    QVector<Scene>   goveeScenes;
    QVector<Scene>   hueScenes;
};

#endif // MAINWINDOW_H
