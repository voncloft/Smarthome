#include "infraredpanel.h"
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHostAddress>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QUuid>
#include <QVBoxLayout>

InfraredPanel::InfraredPanel(const QString &file, QWidget *parent, const QString &helperOverride)
    : QWidget(parent), settingsFile(file), helper(helperOverride)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    auto *intro = new QLabel("Connect your Mini to your home Wi-Fi with the BroadLink phone app, "
                            "then discover it here. Your PC can use Ethernet. "
                            "If access is locked, turn off ‘Lock device’ in the phone app.");
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *hubGroup = new QGroupBox("Room / infrared hub");
    auto *hubLayout = new QVBoxLayout(hubGroup);
    hubBox = new QComboBox;
    hubBox->setObjectName("infraredHub");
    hubLayout->addWidget(hubBox);
    hubDetails = new QLabel;
    hubDetails->setTextFormat(Qt::PlainText);
    hubDetails->setWordWrap(true);
    hubLayout->addWidget(hubDetails);
    auto *hubActions = new QHBoxLayout;
    auto *discover = new QPushButton("Discover hubs");
    discover->setObjectName("discoverHubs");
    auto *addIp = new QPushButton("Add by IP address");
    auto *rename = new QPushButton("Name room");
    auto *remove = new QPushButton("Remove hub");
    auto *check = new QPushButton("Check support");
    for (auto *button : {discover, addIp, rename, remove, check}) hubActions->addWidget(button);
    hubLayout->addLayout(hubActions);
    layout->addWidget(hubGroup);

    auto *remoteGroup = new QGroupBox("TV / projector remote");
    auto *remoteLayout = new QVBoxLayout(remoteGroup);
    auto *remoteActions = new QHBoxLayout;
    remoteBox = new QComboBox;
    remoteBox->setObjectName("infraredRemote");
    auto *add = new QPushButton("Add remote");
    auto *acer = new QPushButton("Add Acer projector");
    acer->setObjectName("addAcerPreset");
    removeRemoteButton = new QPushButton("Remove remote");
    remoteActions->addWidget(remoteBox, 1);
    remoteActions->addWidget(add);
    remoteActions->addWidget(acer);
    remoteActions->addWidget(removeRemoteButton);
    remoteLayout->addLayout(remoteActions);
    auto *form = new QFormLayout;
    commandBox = new QComboBox;
    commandBox->setObjectName("infraredCommand");
    commandBox->setEditable(true);
    commandBox->setInsertPolicy(QComboBox::NoInsert);
    commandBox->lineEdit()->setMaxLength(80);
    form->addRow("Remote button:", commandBox);
    remoteLayout->addLayout(form);
    auto *commandActions = new QHBoxLayout;
    learnButton = new QPushButton("Learn button");
    learnButton->setObjectName("learnInfrared");
    sendButton = new QPushButton("Send button");
    sendButton->setObjectName("sendInfrared");
    menuButton = new QPushButton("Test Menu");
    menuButton->setObjectName("testInfraredMenu");
    forgetButton = new QPushButton("Forget button");
    cancelButton = new QPushButton("Cancel");
    for (auto *button : {learnButton, sendButton, menuButton, forgetButton, cancelButton}) commandActions->addWidget(button);
    remoteLayout->addLayout(commandActions);
    auto *help = new QLabel("Choose a button, click Learn, then point the original remote at the Mini. "
                           "Test Menu sends only the saved Menu button to this remote’s hub. "
                           "Power is a normal remote button, not a confirmed on/off state.");
    help->setWordWrap(true);
    remoteLayout->addWidget(help);
    layout->addWidget(remoteGroup);
    statusLabel = new QLabel("Click Check support, then Discover hubs when your Mini is connected.");
    statusLabel->setObjectName("infraredStatus");
    statusLabel->setWordWrap(true);
    statusLabel->setTextFormat(Qt::PlainText);
    statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(statusLabel);
    layout->addStretch();
    idleControls = {hubBox, remoteBox, commandBox, discover, addIp, rename, remove, check, add, acer};

    if (helper.isEmpty()) {
        QFile resource(":/broadlink/bridge.py");
        if (resource.open(QIODevice::ReadOnly) && helperFile.open()) {
            const QByteArray script = resource.readAll();
            if (helperFile.write(script) == script.size() && helperFile.flush())
                helper = helperFile.fileName();
        }
    }
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, this, [this] {
        canceled = true;
        status("Hub operation timed out. Check its address and network connection; a sent command may already have reached it.");
        process.kill();
    });
    connect(&process, &QProcess::started, this, [this] {
        process.write(QJsonDocument(pending).toJson(QJsonDocument::Compact));
        process.closeWriteChannel();
    });
    connect(&process, &QProcess::readyReadStandardOutput, this, &InfraredPanel::consumeOutput);
    connect(&process, &QProcess::readyReadStandardError, this, [this] { process.readAllStandardError(); });
    connect(&process, &QProcess::finished, this, &InfraredPanel::finish);
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        deadline.stop();
        busy = false;
        status("Could not start Python 3. Install the python and python-broadlink packages.");
        refreshButtons();
    });
    connect(discover, &QPushButton::clicked, this, [this] { start({{"action", "discover"}}); });
    connect(check, &QPushButton::clicked, this, [this] { start({{"action", "check"}}); });
    connect(addIp, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString host = QInputDialog::getText(this, "Add infrared hub", "Mini’s IPv4 address:", QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok) return;
        const QHostAddress address(host);
        if (address.protocol() != QAbstractSocket::IPv4Protocol) { status("Enter the Mini’s IPv4 address, such as 192.168.1.50."); return; }
        start({{"action", "identify"}, {"host", host}});
    });
    connect(rename, &QPushButton::clicked, this, &InfraredPanel::renameHub);
    connect(remove, &QPushButton::clicked, this, &InfraredPanel::removeHub);
    connect(add, &QPushButton::clicked, this, &InfraredPanel::addRemote);
    connect(acer, &QPushButton::clicked, this, &InfraredPanel::addAcerPreset);
    connect(removeRemoteButton, &QPushButton::clicked, this, &InfraredPanel::removeRemote);
    connect(learnButton, &QPushButton::clicked, this, &InfraredPanel::learn);
    connect(sendButton, &QPushButton::clicked, this, [this] { send(commandBox->currentText().trimmed()); });
    connect(menuButton, &QPushButton::clicked, this, [this] { send("Menu"); });
    connect(forgetButton, &QPushButton::clicked, this, &InfraredPanel::forgetCommand);
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        canceled = true;
        status("Canceled. Any previously saved button was kept.");
        process.kill();
    });
    connect(hubBox, &QComboBox::currentIndexChanged, this, [this] { refreshRemotes(); });
    connect(remoteBox, &QComboBox::currentIndexChanged, this, [this] { refreshCommands(); });
    connect(commandBox, &QComboBox::currentTextChanged, this, [this] { refreshButtons(); });
    load();
    refreshHubs();
}

InfraredPanel::~InfraredPanel()
{
    disconnect(&process, nullptr, this, nullptr);
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(1000);
    }
}

void InfraredPanel::status(const QString &text) { statusLabel->setText(text); }

void InfraredPanel::load()
{
    QFile file(settingsFile);
    if (!file.exists()) return;
    QJsonParseError error;
    const bool opened = file.open(QIODevice::ReadOnly);
    const QJsonDocument doc = opened && file.size() <= 1024 * 1024
        ? QJsonDocument::fromJson(file.readAll(), &error) : QJsonDocument();
    const QJsonObject root = doc.object();
    bool valid = !doc.isNull() && error.error == QJsonParseError::NoError
        && root.value("version").toInt() == 1 && root.value("hubs").isArray() && root.value("remotes").isArray();
    QSet<QString> ids;
    const QRegularExpression mac("^[0-9a-f]{12}$");
    for (const auto &value : root.value("hubs").toArray()) {
        const QJsonObject hub = value.toObject();
        const QString id = hub.value("id").toString();
        valid = valid && mac.match(id).hasMatch() && !ids.contains(id)
            && QHostAddress(hub.value("host").toString()).protocol() == QAbstractSocket::IPv4Protocol
            && !hub.value("name").toString().trimmed().isEmpty();
        ids.insert(id);
    }
    QSet<QString> remoteIds;
    for (const auto &value : root.value("remotes").toArray()) {
        const QJsonObject remote = value.toObject();
        const QString id = remote.value("id").toString();
        valid = valid && !id.isEmpty() && !remoteIds.contains(id) && ids.contains(remote.value("hub").toString())
            && !remote.value("name").toString().trimmed().isEmpty() && remote.value("commands").isObject();
        remoteIds.insert(id);
        const auto codes = remote.value("commands").toObject();
        for (auto it = codes.begin(); it != codes.end(); ++it)
            valid = valid && !it.key().trimmed().isEmpty() && it.value().isString();
    }
    if (!valid) {
        readOnly = true;
        status("Could not read saved infrared settings. The existing file has been left unchanged: " + settingsFile);
        return;
    }
    hubs = root.value("hubs").toArray();
    remotes = root.value("remotes").toArray();
}

bool InfraredPanel::save()
{
    if (readOnly) return false;
    QDir().mkpath(QFileInfo(settingsFile).absolutePath());
    QSaveFile file(settingsFile);
    const QByteArray data = QJsonDocument(QJsonObject{{"version", 1}, {"hubs", hubs}, {"remotes", remotes}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        || file.write(data) != data.size() || !file.commit()) {
        readOnly = true;
        status("Could not save infrared settings. Existing settings were kept. Check permissions and restart the app: " + settingsFile);
        refreshButtons();
        return false;
    }
    return true;
}

int InfraredPanel::hubIndex() const
{
    const QString id = hubBox->currentData().toString();
    for (int i = 0; i < hubs.size(); ++i) if (hubs[i].toObject().value("id").toString() == id) return i;
    return -1;
}

int InfraredPanel::remoteIndex() const
{
    const QString id = remoteBox->currentData().toString();
    for (int i = 0; i < remotes.size(); ++i) if (remotes[i].toObject().value("id").toString() == id) return i;
    return -1;
}

QJsonObject InfraredPanel::selectedHub() const { const int i = hubIndex(); return i < 0 ? QJsonObject() : hubs[i].toObject(); }
QJsonObject InfraredPanel::selectedRemote() const { const int i = remoteIndex(); return i < 0 ? QJsonObject() : remotes[i].toObject(); }

void InfraredPanel::refreshHubs(const QString &selected)
{
    const QString id = selected.isEmpty() ? hubBox->currentData().toString() : selected;
    {
        QSignalBlocker blocker(hubBox);
        hubBox->clear();
        for (const auto &value : hubs) { const auto hub = value.toObject(); hubBox->addItem(hub.value("name").toString(), hub.value("id").toString()); }
        const int index = hubBox->findData(id);
        if (index >= 0) hubBox->setCurrentIndex(index);
    }
    refreshRemotes();
}

void InfraredPanel::refreshRemotes(const QString &selected)
{
    const QString id = selected.isEmpty() ? remoteBox->currentData().toString() : selected;
    const QString hubId = hubBox->currentData().toString();
    {
        QSignalBlocker blocker(remoteBox);
        remoteBox->clear();
        for (const auto &value : remotes) {
            const auto remote = value.toObject();
            if (remote.value("hub").toString() == hubId)
                remoteBox->addItem(remote.value("name").toString(), remote.value("id").toString());
        }
        const int index = remoteBox->findData(id);
        if (index >= 0) remoteBox->setCurrentIndex(index);
    }
    refreshCommands();
}

void InfraredPanel::refreshCommands()
{
    const QJsonObject commands = selectedRemote().value("commands").toObject();
    const QString current = commandBox->currentText().trimmed();
    {
        QSignalBlocker blocker(commandBox);
        commandBox->clear();
        QStringList names = {"Menu", "Back", "Up", "Down", "Left", "Right", "OK", "Input", "Power", "Volume up", "Volume down", "Mute"};
        for (const auto &name : commands.keys()) if (!names.contains(name)) names << name;
        commandBox->addItems(names);
        if (!current.isEmpty()) commandBox->setCurrentText(current);
    }
    refreshButtons();
}

void InfraredPanel::refreshButtons()
{
    const bool available = !busy && !readOnly;
    for (auto *control : idleControls) control->setEnabled(available);
    const QJsonObject hub = selectedHub();
    const QJsonObject remote = selectedRemote();
    hubDetails->setText(hub.isEmpty() ? "No hub saved yet." : hub.value("model").toString() + " · " + hub.value("host").toString());
    const QJsonObject commands = remote.value("commands").toObject();
    const QString command = commandBox->currentText().trimmed();
    const bool saved = !commands.value(command).toString().isEmpty();
    learnButton->setEnabled(available && !remote.isEmpty() && !command.isEmpty());
    sendButton->setEnabled(available && !remote.isEmpty() && saved);
    menuButton->setEnabled(available && !remote.isEmpty() && !commands.value("Menu").toString().isEmpty());
    forgetButton->setEnabled(available && saved);
    removeRemoteButton->setEnabled(available && !remote.isEmpty());
    cancelButton->setEnabled(busy && pending.value("action") != "send");
}

void InfraredPanel::start(QJsonObject request)
{
    if (busy || readOnly) return;
    if (helper.isEmpty()) { status("The infrared helper is missing. Rebuild or reinstall SmartHome."); return; }
    pending = request;
    output.clear();
    result = {};
    gotResult = canceled = false;
    busy = true;
    refreshButtons();
    status(request.value("action") == "learn" ? "Preparing the Mini to learn…" : "Contacting BroadLink support…");
    deadline.start(request.value("action") == "learn" ? 50000 : 18000);
    process.start("python3", {"-I", helper});
}

void InfraredPanel::consumeOutput()
{
    output += process.readAllStandardOutput();
    if (output.size() > 1024 * 1024) { canceled = true; status("Unexpectedly large response from infrared helper."); process.kill(); return; }
    int end;
    while ((end = output.indexOf('\n')) >= 0) {
        const QByteArray line = output.left(end);
        output.remove(0, end + 1);
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) { canceled = true; status("Invalid response from infrared helper."); process.kill(); return; }
        const auto event = doc.object();
        if (event.value("event") == "progress" && !canceled) status(event.value("message").toString());
        if (event.value("event") == "result") { result = event; gotResult = true; }
    }
}

void InfraredPanel::finish(int exitCode, QProcess::ExitStatus exitStatus)
{
    consumeOutput();
    deadline.stop();
    busy = false;
    if (canceled) { refreshButtons(); return; }
    if (!gotResult || !result.value("ok").toBool() || exitCode != 0 || exitStatus != QProcess::NormalExit) {
        status(result.value("message").toString("Infrared helper stopped without a result. Check Python and python-broadlink."));
    } else {
        const QString action = pending.value("action").toString();
        if (action == "discover" || action == "identify") mergeHubs(result.value("hubs").toArray());
        else if (action == "learn") {
            const QString id = pending.value("remoteId").toString();
            const QString command = pending.value("command").toString();
            const QString code = result.value("code").toString();
            if (code.isEmpty()) status("No infrared code was returned. The saved button was kept.");
            else for (int i = 0; i < remotes.size(); ++i) {
                auto remote = remotes[i].toObject();
                if (remote.value("id").toString() != id || remote.value("hub") != pending.value("id")) continue;
                auto commands = remote.value("commands").toObject();
                commands[command] = code;
                remote["commands"] = commands;
                remotes[i] = remote;
                if (save()) status("Saved " + command + " for " + remote.value("name").toString() + ".");
                break;
            }
        } else if (action == "send") {
            status("Sent " + pending.value("command").toString() + " to " + pending.value("remoteName").toString()
                   + " through " + pending.value("hubName").toString() + ". Check the device for its response.");
        } else status(result.value("message").toString());
    }
    refreshCommands();
}

void InfraredPanel::mergeHubs(const QJsonArray &found)
{
    if (found.isEmpty()) { status("No infrared hubs found. Check Wi-Fi and ‘Lock device’, or add the hub by IP address."); return; }
    for (const auto &value : found) {
        auto incoming = value.toObject();
        bool exists = false;
        for (int i = 0; i < hubs.size(); ++i) {
            const auto old = hubs[i].toObject();
            if (old.value("id") != incoming.value("id")) continue;
            incoming["name"] = old.value("name");
            hubs[i] = incoming;
            exists = true;
            break;
        }
        if (!exists) hubs.append(incoming);
    }
    if (save()) status(QString("Found %1 infrared hub(s). Select one and use Name room, then Add remote.").arg(found.size()));
    refreshHubs();
}

void InfraredPanel::learn()
{
    auto hub = selectedHub();
    const auto remote = selectedRemote();
    const QString command = commandBox->currentText().trimmed();
    if (hub.isEmpty() || remote.isEmpty() || command.isEmpty()) return;
    hub["action"] = "learn";
    hub["remoteId"] = remote.value("id");
    hub["command"] = command;
    start(hub);
}

void InfraredPanel::send(const QString &command)
{
    auto hub = selectedHub();
    const auto remote = selectedRemote();
    const QString code = remote.value("commands").toObject().value(command).toString();
    if (hub.isEmpty() || remote.isEmpty() || code.isEmpty() || remote.value("hub") != hub.value("id")) {
        status("Learn " + command + " for this remote first."); return;
    }
    hub["action"] = "send";
    hub["command"] = command;
    hub["code"] = code;
    hub["hubName"] = hub.value("name");
    hub["remoteName"] = remote.value("name");
    start(hub);
}

void InfraredPanel::addRemote()
{
    const auto hub = selectedHub();
    if (hub.isEmpty()) { status("Discover or add a hub first."); return; }
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Add remote", "Device name (for example, Projector or Bedroom TV):", QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    remotes.append(QJsonObject{{"id", id}, {"hub", hub.value("id")}, {"name", name}, {"commands", QJsonObject()}});
    if (save()) status("Added " + name + ". Learn Menu first for a test without changing power.");
    refreshRemotes(id);
}

void InfraredPanel::addAcerPreset()
{
    const auto hub = selectedHub();
    if (hub.isEmpty()) { status("Discover or add a hub first."); return; }
    for (const auto &value : remotes) {
        const auto remote = value.toObject();
        if (remote.value("hub") == hub.value("id") && remote.value("preset") == "acer-h6510bd") {
            refreshRemotes(remote.value("id").toString());
            status("Selected the saved Acer remote. Its buttons have been kept.");
            return;
        }
    }
    QFile file(":/broadlink/acer-h6510bd.json");
    if (!file.open(QIODevice::ReadOnly)) { status("Acer preset is missing from this build."); return; }
    const auto preset = QJsonDocument::fromJson(file.readAll()).object();
    const auto codes = preset.value("commands").toObject();
    if (codes.value("Menu").toString().isEmpty()) { status("Acer preset has no Menu command."); return; }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    remotes.append(QJsonObject{{"id", id}, {"hub", hub.value("id")}, {"name", "Acer H6510BD"},
                              {"preset", "acer-h6510bd"}, {"commands", codes}});
    if (save()) status("Added published Acer H6510BD codes. With the projector already on, click Test Menu to check reception. No original remote is needed.");
    refreshRemotes(id);
}

void InfraredPanel::renameHub()
{
    const int index = hubIndex();
    if (index < 0) return;
    auto hub = hubs[index].toObject();
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Name room", "Room / hub name:", QLineEdit::Normal, hub.value("name").toString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    hub["name"] = name;
    hubs[index] = hub;
    if (save()) status("Hub name saved.");
    refreshHubs(hub.value("id").toString());
}

void InfraredPanel::removeHub()
{
    const int index = hubIndex();
    if (index < 0) return;
    const QString id = hubs[index].toObject().value("id").toString();
    if (QMessageBox::question(this, "Remove hub", "Remove this hub and its saved remotes?", QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    hubs.removeAt(index);
    for (int i = remotes.size() - 1; i >= 0; --i) if (remotes[i].toObject().value("hub").toString() == id) remotes.removeAt(i);
    if (save()) status("Hub removed.");
    refreshHubs();
}

void InfraredPanel::removeRemote()
{
    const int index = remoteIndex();
    if (index < 0) return;
    if (QMessageBox::question(this, "Remove remote", "Remove this remote and its learned buttons?", QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    remotes.removeAt(index);
    if (save()) status("Remote removed.");
    refreshRemotes();
}

void InfraredPanel::forgetCommand()
{
    const int index = remoteIndex();
    if (index < 0) return;
    auto remote = remotes[index].toObject();
    auto codes = remote.value("commands").toObject();
    codes.remove(commandBox->currentText().trimmed());
    remote["commands"] = codes;
    remotes[index] = remote;
    if (save()) status("Saved button removed.");
    refreshButtons();
}
