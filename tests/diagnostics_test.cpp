#include "mainwindow.h"
#include "infraredpanel.h"
#include <QtTest>
#include <QNetworkProxy>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextEdit>

class DiagnosticsTest : public QObject {
    Q_OBJECT
private slots:
    void originalListCombinesBothSourcesAndSurvivesRefresh() {
        MainWindow window;
        for (auto *timer : window.findChildren<QTimer *>()) timer->stop();
        auto *tab = window.createDiagnosticsTab();
        auto *list = tab->findChild<QListWidget *>("routineVerifyList");
        QVERIFY(list);
        QCOMPARE(tab->findChildren<QListWidget *>().size(), 1);
        QVERIFY(tab->findChildren<QPlainTextEdit *>().isEmpty());
        QVERIFY(tab->findChildren<QTextEdit *>().isEmpty());
        auto text = [list] {
            QStringList entries;
            for (int i = 0; i < list->count(); ++i) entries << list->item(i)->text();
            return entries.join('\n');
        };
        // The temporary invalid IR file is read before the shared view exists.
        QVERIFY(text().contains("Could not read saved infrared settings"));
        window.addRoutineVerifyRecent("[MANUAL]", "Desk light -> power=on -> verified");
        emit window.infraredPanel->diagnosticAdded("12:30:00 [IR] Projector / Menu — Sent Menu");
        QVERIFY(text().contains("Desk light -> power=on -> verified"));
        QVERIFY(text().contains("[IR] Projector / Menu"));
        QVERIFY(window.routineVerifyRecentEntries.first().contains("[IR]"));
        window.refreshRoutineVerifyDiagnostics();
        window.buildUI();
        QCOMPARE(window.createDiagnosticsTab(), tab);
        QVERIFY(text().contains("[IR] Projector / Menu"));
        const int before = window.routineVerifyRecentEntries.size();
        emit window.infraredPanel->diagnosticAdded("[IR] Hub unreachable");
        QCOMPARE(window.routineVerifyRecentEntries.size(), before + 1);
        window.tabWidget->setCurrentWidget(tab);
        window.resize(1000, 550); window.show();
        if (!qEnvironmentVariableIsEmpty("LIGHTS_DIAGNOSTICS_SCREENSHOT")) {
            QTest::qWait(100);
            QVERIFY(window.grab().save(qEnvironmentVariable("LIGHTS_DIAGNOSTICS_SCREENSHOT")));
        }
        for (int i = 0; i < 520; ++i) {
            if (i % 2) window.addRoutineVerifyRecent("[MANUAL]", QString("Light event %1").arg(i));
            else emit window.infraredPanel->diagnosticAdded(QString("[IR] Event %1").arg(i));
        }
        QCOMPARE(window.routineVerifyRecentEntries.size(), 500);
        QVERIFY(!text().contains("Desk light -> power=on -> verified"));
        QVERIFY(text().contains("Light event 519"));
        QVERIFY(text().contains("[IR] Event 518"));
        QTRY_VERIFY(list->verticalScrollBar()->maximum() > 10);
        list->verticalScrollBar()->setValue(10);
        window.refreshRoutineVerifyDiagnostics();
        QCOMPARE(list->verticalScrollBar()->value(), 10);
    }
};

int main(int argc, char **argv) {
    QTemporaryDir home;
    if (!home.isValid()) return 1;
    qputenv("HOME", home.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", home.filePath(".config").toUtf8());
    QDir().mkpath(home.filePath(".config/govee"));
    QFile settings(home.filePath(".config/govee/Lights.json"));
    if (!settings.open(QIODevice::WriteOnly)) return 1;
    settings.write(R"({"apiKey":"test-only","hideMessageBoxes":true,"presenceAutoOnAllGroups":false})");
    settings.close();
    QFile infrared(home.filePath(".config/govee/infrared.json"));
    if (!infrared.open(QIODevice::WriteOnly)) return 1;
    infrared.write("{broken"); infrared.close();
    QApplication app(argc, argv);
    // All network requests fail locally; no real settings or devices are used.
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", 1));
    DiagnosticsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "diagnostics_test.moc"
