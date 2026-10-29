#ifndef INFRAREDPANEL_H
#define INFRAREDPANEL_H

#include <QWidget>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryFile>
#include <QTimer>

class QComboBox;
class QLabel;
class QPushButton;

class InfraredPanel : public QWidget
{
    Q_OBJECT
public:
    explicit InfraredPanel(const QString &settingsFile, QWidget *parent = nullptr,
                           const QString &helperOverride = QString());
    ~InfraredPanel() override;

private:
    void load();
    bool save();
    void refreshHubs(const QString &selected = QString());
    void refreshRemotes(const QString &selected = QString());
    void refreshCommands();
    void refreshButtons();
    void start(QJsonObject request);
    void consumeOutput();
    void finish(int exitCode, QProcess::ExitStatus exitStatus);
    void mergeHubs(const QJsonArray &found);
    void learn();
    void send(const QString &command);
    void addRemote();
    void addAcerPreset();
    void renameHub();
    void removeHub();
    void removeRemote();
    void forgetCommand();
    int hubIndex() const;
    int remoteIndex() const;
    QJsonObject selectedHub() const;
    QJsonObject selectedRemote() const;
    void status(const QString &text);

    QString settingsFile;
    QString helper;
    QTemporaryFile helperFile;
    QJsonArray hubs;
    QJsonArray remotes;
    bool readOnly = false;
    bool busy = false;
    bool gotResult = false;
    bool canceled = false;
    QJsonObject pending;
    QJsonObject result;
    QByteArray output;
    QProcess process;
    QTimer deadline;
    QComboBox *hubBox;
    QComboBox *remoteBox;
    QComboBox *commandBox;
    QLabel *statusLabel;
    QLabel *hubDetails;
    QPushButton *learnButton;
    QPushButton *sendButton;
    QPushButton *menuButton;
    QPushButton *cancelButton;
    QPushButton *forgetButton;
    QPushButton *removeRemoteButton;
    QList<QWidget *> idleControls;
};

#endif
