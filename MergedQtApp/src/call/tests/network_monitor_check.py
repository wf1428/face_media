"""Build real NetworkStatusMonitor host tests with isolated hardware and route fixtures.

Author: Du lin
Created: 2026-11-03
Note: 最近修改：2026-11-04；修改人：Du lin；回归独立网络线程的QProcess/定时器归属、ping中止重启及异步释放，保持UI心跳检查。
Uses an installed Qt/MinGW toolchain; creates disposable build inputs under .web-build.
The child route commands are copies of the test executable, never host network changes.
Raises CalledProcessError on compilation/test failure; preserves logs for diagnosis.
"""
import argparse
import os
from pathlib import Path
import subprocess


def run_checked(args, directory, env, log):
    """Run one command and preserve its combined output.

    Args:
        args: Executable and arguments, passed directly without a shell.
        directory: Isolated build directory used as the working directory.
        env: Child environment containing the selected Qt and compiler paths.
        log: Output path overwritten for this command.
    Raises:
        subprocess.CalledProcessError: Compilation or test command failed.
        OSError: Executable or log could not be opened.
    """
    with log.open("w", encoding="utf-8") as output:
        subprocess.run(args, cwd=directory, env=env, stdout=output,
                       stderr=subprocess.STDOUT, check=True)


def main():
    """Generate fixtures, compile the production monitor and execute bounded UI tests.

    Raises:
        subprocess.CalledProcessError: A build or regression command failed.
        OSError: Sources, fixture directories or tool executables are unavailable.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument("--qmake", required=True)
    parser.add_argument("--make", required=True)
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    build = root / ".web-build/voice-remediation-20261104/network"
    build.mkdir(parents=True, exist_ok=True)
    stubs = build / "stubs"
    for folder in ("ic_board", "platform"):
        (stubs / folder).mkdir(parents=True, exist_ok=True)
    (stubs / "ic_board/ic_event_bridge.h").write_text("""#pragma once
class IcEventBridge { public:
static IcEventBridge *instance() { static IcEventBridge bridge; return &bridge; }
void updateNetworkAvailability(bool value) { available=value; ++updates; }
bool available=true; int updates=0;
};
""", encoding="utf-8")
    (stubs / "platform/rk3566_platform.h").write_text("""#pragma once
#include <QString>
namespace Rk3566Platform { inline QString netConfigPath() { return qEnvironmentVariable("NETWORK_TEST_INI"); } }
""", encoding="utf-8")
    monitor = root / "components/multimedia/network_status_monitor.cpp"
    # Exercise the actual route-reading body with a zero-size /proc fixture.
    body = monitor.read_text(encoding="utf-8")
    begin = body.index("bool NetworkStatusMonitor::hasDefaultRoute()")
    end = body.index("\n/**", begin)
    route = body[begin:end].replace("bool NetworkStatusMonitor::hasDefaultRoute()", "bool checkedRoute()")
    route = route.replace("QFile f(", "ZeroProc f(")
    source = r'''
/** @file network_tests.cpp
 * @brief Generated host regression around the actual production network monitor.
 * @author Du lin
 * @date 2026-11-03
 */
#include <QtWidgets>
#include <QtNetwork>
#include <QtTest>
#include <QThreadPool>
#include <QProcess>
#include <atomic>
#define private public
#include "components/multimedia/network_status_monitor.h"
#undef private
#include "ic_board/ic_event_bridge.h"
struct ZeroProc {
    static int reads;
    explicit ZeroProc(const QString &) {}
    bool open(QIODevice::OpenMode) { return true; }
    bool atEnd() const { return true; }
    QByteArray readAll() { ++reads;return "Iface Destination Gateway Flags\neth0 00000000 010200C0 0003\n"; }
};
int ZeroProc::reads=0;
ROUTE_BODY
class NetworkTests:public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    QString ini;
private slots:
    void initTestCase() {
        QVERIFY(directory.isValid());ini=directory.filePath("net_cfg.ini");
        QSettings settings(ini,QSettings::IniFormat);settings.setValue("feature/online_v1",true);settings.sync();
        for(const auto &name:{"ip.exe","route.exe","ping.exe"})
            QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(),directory.filePath(name)));
        qputenv("PATH",(directory.path()+QDir::listSeparator()+qEnvironmentVariable("PATH")).toLocal8Bit());
    }
    void zeroSizeProc() { ZeroProc::reads=0;QVERIFY(checkedRoute());QCOMPARE(ZeroProc::reads,1); }
    void slowRouteKeepsUiResponsive() {
        NetworkStatusMonitor monitor(nullptr,ini);QTimer heartbeat;int beats=0;
        connect(&heartbeat,&QTimer::timeout,this,[&]() { ++beats; });heartbeat.start(5);
        QElapsedTimer elapsed;elapsed.start();monitor.start();QVERIFY(elapsed.elapsed()<50);
        QCOMPARE(monitor.checkState_,NetworkStatusMonitor::CheckState::CheckingRoute);
        QTest::qWait(150);QVERIFY(beats>=5);
        QTRY_VERIFY_WITH_TIMEOUT(monitor.checkState_!=NetworkStatusMonitor::CheckState::CheckingRoute,3000);
        QVERIFY(monitor.lastDefaultRouteOk_);monitor.stop();QTest::qWait(50);
    }
    void destroyWhileQueryRuns() {
        auto *monitor=new NetworkStatusMonitor(nullptr,ini);monitor->start();QTest::qWait(80);
        const int previous=IcEventBridge::instance()->updates;
        QElapsedTimer elapsed;elapsed.start();delete monitor;QVERIFY(elapsed.elapsed()<50);
        QTest::qWait(700);QCOMPARE(IcEventBridge::instance()->updates,previous);
    }
    void pingObjectsStayOffUiAndCancel() {
        qputenv("NETWORK_TEST_PING_MS","600");NetworkStatusMonitor monitor(nullptr,ini);monitor.start();
        QTRY_COMPARE_WITH_TIMEOUT(monitor.checkState_,NetworkStatusMonitor::CheckState::Pinging,3000);
        QVERIFY(monitor.networkThread_!=qApp->thread());QVERIFY(monitor.networkThread_->isRunning());
        auto *probe=reinterpret_cast<QObject*>(monitor.networkWorker_);std::atomic_bool checked{false},correct{false};
        QMetaObject::invokeMethod(probe,[&] {
            const auto processes=probe->findChildren<QProcess*>();const auto timers=probe->findChildren<QTimer*>();
            correct=processes.size()==1 && timers.size()==1 && processes[0]->thread()==QThread::currentThread() && timers[0]->thread()==QThread::currentThread();checked=true;
        },Qt::QueuedConnection);
        QTRY_VERIFY(checked.load());QVERIFY(correct.load());
        const int oldUpdates=IcEventBridge::instance()->updates;QElapsedTimer elapsed;elapsed.start();monitor.stop();QVERIFY(elapsed.elapsed()<30);
        QTest::qWait(80);QCOMPARE(IcEventBridge::instance()->updates,oldUpdates);
        monitor.start();QTRY_COMPARE_WITH_TIMEOUT(monitor.checkState_,NetworkStatusMonitor::CheckState::Pinging,3000);monitor.stop();
        qunsetenv("NETWORK_TEST_PING_MS");QTest::qWait(100);
    }
    void asynchronousThreadRelease() {
        qputenv("NETWORK_TEST_PING_MS","600");auto *monitor=new NetworkStatusMonitor(nullptr,ini);monitor->start();
        QTRY_COMPARE_WITH_TIMEOUT(monitor->checkState_,NetworkStatusMonitor::CheckState::Pinging,3000);
        QPointer<QThread> worker=monitor->networkThread_;QElapsedTimer elapsed;elapsed.start();delete monitor;QVERIFY(elapsed.elapsed()<30);
        QTRY_VERIFY_WITH_TIMEOUT(worker.isNull(),3000);qunsetenv("NETWORK_TEST_PING_MS");
    }
    void cancelAndRestart() {
        NetworkStatusMonitor monitor(nullptr,ini);monitor.start();QTest::qWait(60);monitor.stop();
        QCOMPARE(monitor.checkState_,NetworkStatusMonitor::CheckState::Idle);monitor.start();
        QTRY_VERIFY_WITH_TIMEOUT(monitor.checkState_!=NetworkStatusMonitor::CheckState::CheckingRoute,3000);
        QVERIFY(monitor.lastDefaultRouteOk_);monitor.stop();QTest::qWait(50);
    }
    void cleanupTestCase() { QThreadPool::globalInstance()->waitForDone(); }
};
int main(int argc,char **argv) {
    const auto name=QFileInfo(QString::fromLocal8Bit(argv[0])).baseName();
    if(name=="ip" || name=="route") { QThread::msleep(300);fputs("default via 192.0.2.1 dev test\n",stdout);return 0; }
    if(name=="ping") { const int pause=qEnvironmentVariableIntValue("NETWORK_TEST_PING_MS");QThread::msleep(pause>0 ? pause:5);return 0; }
    QApplication app(argc,argv);NetworkTests test;return QTest::qExec(&test,argc,argv);
}
#include "network_tests.moc"
'''.replace("ROUTE_BODY", route)
    (build / "network_tests.cpp").write_text(source, encoding="utf-8")
    (build / "network_tests.pro").write_text(f'''QT += core widgets network testlib
CONFIG += console testcase c++14
CONFIG -= app_bundle
TEMPLATE = app
TARGET = network_tests
INCLUDEPATH += "{stubs.as_posix()}" "{root.as_posix()}"
SOURCES += network_tests.cpp "{monitor.as_posix()}" "{(root / 'common/debug/ui_performance.cpp').as_posix()}"
HEADERS += "{(root / 'components/multimedia/network_status_monitor.h').as_posix()}"
''', encoding="utf-8")
    env = os.environ.copy()
    env["PATH"] = str(Path(options.qmake).resolve().parent) + os.pathsep + str(Path(options.make).resolve().parent) + os.pathsep + env.get("PATH", "")
    env["QT_QPA_PLATFORM"] = "offscreen"
    run_checked([str(Path(options.qmake).resolve()), "network_tests.pro"], build, env, build / "qmake.log")
    run_checked([str(Path(options.make).resolve()), "-j4"], build, env, build / "build.log")
    result = build / "results.txt"
    run_checked([str(build / "release/network_tests.exe"), "-o", str(result) + ",txt"], build, env, build / "console.log")
    print(result.read_text(encoding="utf-8"))


if __name__ == "__main__":
    main()
