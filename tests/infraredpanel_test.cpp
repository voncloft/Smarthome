#include "infraredpanel.h"
#include <QtTest>
#include <QComboBox>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>

class InfraredPanelTest : public QObject
{
    Q_OBJECT
    QTemporaryDir dir;
    QString file;
    QString helper;
    QJsonObject settings;
    const QString menuCode = "JgAEAAECAwQ=";
    const QString powerCode = "JgAEAAQDAgE=";
    void write(const QString &path, const QByteArray &data) {
        QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(data), data.size());
    }
    QJsonObject read(const QString &path) {
        QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return QJsonDocument::fromJson(f.readAll()).object();
    }
    void reply(QJsonObject response) {
        response["event"] = "result"; response["ok"] = true;
        write(helper + ".reply", QJsonDocument(response).toJson());
    }
    QPushButton *button(InfraredPanel &panel, const char *name) { return panel.findChild<QPushButton *>(name); }
    QComboBox *combo(InfraredPanel &panel, const char *name) { return panel.findChild<QComboBox *>(name); }
private slots:
    void init() {
        QVERIFY(dir.isValid());
        file = dir.filePath("infrared.json"); helper = dir.filePath("fake.py");
        QFile::remove(helper + ".request");
        write(helper, R"PY(import json, pathlib, sys
request = json.load(sys.stdin)
pathlib.Path(__file__ + '.request').write_text(json.dumps(request))
print(json.dumps(json.loads(pathlib.Path(__file__ + '.reply').read_text())), flush=True)
)PY");
        QJsonArray hubs{
            QJsonObject{{"id", "001122334455"}, {"host", "192.168.1.20"}, {"name", "Bedroom"}, {"model", "RM4 mini"}},
            QJsonObject{{"id", "aabbccddeeff"}, {"host", "192.168.1.21"}, {"name", "Projector room"}, {"model", "RM4 mini"}}};
        QJsonArray remotes{
            QJsonObject{{"id", "tv"}, {"hub", "001122334455"}, {"name", "TV"}, {"commands", QJsonObject{{"Menu", menuCode}, {"Power", powerCode}}}},
            QJsonObject{{"id", "projector"}, {"hub", "aabbccddeeff"}, {"name", "Projector"}, {"commands", QJsonObject{{"Menu", menuCode}, {"Power", powerCode}}}}};
        settings = {{"version", 1}, {"hubs", hubs}, {"remotes", remotes}};
        write(file, QJsonDocument(settings).toJson());
        reply({{"message", "Sent"}});
    }
    void menuAlwaysUsesMenuAndSelectedHub() {
        InfraredPanel panel(file, nullptr, helper);
        combo(panel, "infraredHub")->setCurrentIndex(1);
        combo(panel, "infraredCommand")->setCurrentText("Power");
        QVERIFY(button(panel, "testInfraredMenu")->isEnabled());
        button(panel, "testInfraredMenu")->click();
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(helper + ".request"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(button(panel, "testInfraredMenu")->isEnabled(), 5000);
        const auto request = read(helper + ".request");
        QCOMPARE(request.value("command").toString(), QString("Menu"));
        QCOMPARE(request.value("code").toString(), menuCode);
        QCOMPARE(request.value("id").toString(), QString("aabbccddeeff"));
        QCOMPARE(request.value("host").toString(), QString("192.168.1.21"));
        QVERIFY(panel.findChild<QLabel *>("infraredStatus")->text().contains("Projector room"));
    }
    void menuNeverFallsBackToPower() {
        auto remotes = settings["remotes"].toArray();
        auto remote = remotes[0].toObject(); remote["commands"] = QJsonObject{{"Power", powerCode}}; remotes[0] = remote;
        settings["remotes"] = remotes; write(file, QJsonDocument(settings).toJson());
        InfraredPanel panel(file, nullptr, helper);
        QVERIFY(!button(panel, "testInfraredMenu")->isEnabled());
        QVERIFY(!button(panel, "sendInfrared")->isEnabled());
        QVERIFY(!QFile::exists(helper + ".request"));
    }
    void learnsAndRestoresCodeForOnlySelectedRemote() {
        reply({{"code", "JgAEAAUGBwg="}});
        {
            InfraredPanel panel(file, nullptr, helper);
            combo(panel, "infraredHub")->setCurrentIndex(1);
            button(panel, "learnInfrared")->click();
            QVERIFY(!combo(panel, "infraredHub")->isEnabled());
            QTRY_VERIFY_WITH_TIMEOUT(combo(panel, "infraredHub")->isEnabled(), 5000);
        }
        const auto saved = read(file)["remotes"].toArray();
        QCOMPARE(saved[0].toObject()["commands"].toObject()["Menu"].toString(), menuCode);
        QCOMPARE(saved[1].toObject()["commands"].toObject()["Menu"].toString(), QString("JgAEAAUGBwg="));
        reply({{"message", "Sent"}});
        InfraredPanel reopened(file, nullptr, helper);
        combo(reopened, "infraredHub")->setCurrentIndex(1);
        button(reopened, "testInfraredMenu")->click();
        QTRY_VERIFY_WITH_TIMEOUT(button(reopened, "testInfraredMenu")->isEnabled(), 5000);
        QCOMPARE(read(helper + ".request")["code"].toString(), QString("JgAEAAUGBwg="));
    }
    void rediscoveryPreservesNamesRemotesAndUpdatesAddresses() {
        reply({{"hubs", QJsonArray{QJsonObject{{"id", "001122334455"}, {"host", "192.168.1.99"}, {"name", "Factory name"}, {"model", "RM4 mini"}}}}});
        InfraredPanel panel(file, nullptr, helper);
        button(panel, "discoverHubs")->click();
        QTRY_VERIFY_WITH_TIMEOUT(button(panel, "discoverHubs")->isEnabled(), 5000);
        const auto root = read(file);
        QCOMPARE(root["hubs"].toArray().size(), 2);
        QCOMPARE(root["hubs"].toArray()[0].toObject()["host"].toString(), QString("192.168.1.99"));
        QCOMPARE(root["hubs"].toArray()[0].toObject()["name"].toString(), QString("Bedroom"));
        QCOMPARE(root["remotes"], settings["remotes"]);
    }
    void malformedSettingsAreNotOverwritten() {
        write(file, "{broken");
        InfraredPanel panel(file, nullptr, helper);
        QVERIFY(!button(panel, "discoverHubs")->isEnabled());
        QVERIFY(panel.findChild<QLabel *>("infraredStatus")->text().contains("left unchanged"));
        QFile f(file); QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(), QByteArray("{broken"));
    }
    void acerPresetNeedsNoLearningAndNeverSendsOnAdd() {
        InfraredPanel panel(file, nullptr, helper);
        combo(panel, "infraredHub")->setCurrentIndex(1);
        button(panel, "addAcerPreset")->click();
        QVERIFY(!QFile::exists(helper + ".request"));
        const auto saved = read(file)["remotes"].toArray();
        QCOMPARE(saved.size(), 3);
        const auto remote = saved.last().toObject();
        QCOMPARE(remote["hub"].toString(), QString("aabbccddeeff"));
        QVERIFY(!remote["commands"].toObject()["Menu"].toString().isEmpty());
        QVERIFY(remote["commands"].toObject()["Menu"] != remote["commands"].toObject()["Power"]);
        button(panel, "addAcerPreset")->click();
        QCOMPARE(read(file)["remotes"].toArray().size(), 3);
        button(panel, "testInfraredMenu")->click();
        QTRY_VERIFY_WITH_TIMEOUT(button(panel, "testInfraredMenu")->isEnabled(), 5000);
        const auto request = read(helper + ".request");
        QCOMPARE(request["command"].toString(), QString("Menu"));
        QCOMPARE(request["code"], remote["commands"].toObject()["Menu"]);
    }
    void customButtonNamesKeepSpaces() {
        InfraredPanel panel(file, nullptr, helper);
        auto *edit = combo(panel, "infraredCommand")->lineEdit();
        edit->clear(); QTest::keyClicks(edit, "Custom button");
        QCOMPARE(edit->text(), QString("Custom button"));
        QVERIFY(button(panel, "learnInfrared")->isEnabled());
        QVERIFY(!button(panel, "sendInfrared")->isEnabled());
    }
    void failedLearningKeepsSavedCode() {
        write(helper + ".reply", QJsonDocument(QJsonObject{{"event", "result"}, {"ok", false}, {"message", "Timed out"}}).toJson());
        InfraredPanel panel(file, nullptr, helper);
        button(panel, "learnInfrared")->click();
        QTRY_VERIFY_WITH_TIMEOUT(button(panel, "learnInfrared")->isEnabled(), 5000);
        QCOMPARE(read(file), settings);
        QCOMPARE(panel.findChild<QLabel *>("infraredStatus")->text(), QString("Timed out"));
    }
    void screenshot() {
        if (qEnvironmentVariableIsEmpty("LIGHTS_TEST_SCREENSHOT")) QSKIP("No preview requested");
        InfraredPanel panel(file, nullptr, helper);
        panel.resize(1100, 550); panel.show(); QTest::qWait(100);
        QVERIFY(panel.grab().save(qEnvironmentVariable("LIGHTS_TEST_SCREENSHOT")));
    }
};
QTEST_MAIN(InfraredPanelTest)
#include "infraredpanel_test.moc"
