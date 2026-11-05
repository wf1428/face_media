"""Test the production call-volume wrappers against a controlled Qt focus manager.

Author: Du lin
Date: 2026-10-30
Note: 最近修改：2026-10-30；修改人：Du lin；补充通话专用 acquireCall 边界，保留取消与多持有者票据测试；硬件隔离由真实控制器回归另行验证。
Checks deferred readiness, cancellation, duplicate readiness, and overlapping owners.
Only the existing hardware focus boundary is substituted; no real gain is changed.
"""
import os
from pathlib import Path
import subprocess


def function(source, signature):
    """Extract a complete production function.

    Args:
        source: UTF-8 volume-controller source with normalized newlines.
        signature: Exact signature immediately preceding the opening brace.
    Returns:
        The unmodified definition, including its closing brace.
    Raises:
        ValueError: The signature or definition end cannot be found.
    """
    start = source.index(signature + '\n{')
    return source[start:source.index('\n}', start) + 2]


def main():
    """Build and execute the real asynchronous lease wrappers.

    Raises:
        OSError: Qt tools or source files are unavailable.
        subprocess.CalledProcessError: Compilation or a regression assertion fails.
        subprocess.TimeoutExpired: A tool exceeds its bounded runtime.
    """
    root = Path.cwd()
    output = root / '.web-build/voice-call-20261030/volume'
    output.mkdir(parents=True, exist_ok=True)
    source = (root / 'components/multimedia/vol_ctrl.cpp').read_text(encoding='utf-8')
    code = r'''
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QSet>
#include <QVariant>
#include <cstdio>
#include <cstdlib>
#include "src/call/AudioOutputFocus.h"
static void check(bool ok,const char *message) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1); } }
class AudioPromptFocusManager : public QObject {
    Q_OBJECT
public:
    QSet<QObject *> owners;
    static AudioPromptFocusManager &instance() { static AudioPromptFocusManager value;return value; }
    void acquire(QObject *owner) {
        owners.insert(owner);
        QTimer::singleShot(1,this,[this,owner]() { if(owners.contains(owner)) emit focusReady(owner); });
    }
    void acquireCall(QObject *owner) { acquire(owner); }
    void release(QObject *owner) { owners.remove(owner); }
signals:
    void focusReady(QObject *owner);
};
static void pump() { QEventLoop loop;QTimer::singleShot(10,&loop,&QEventLoop::quit);loop.exec(); }
'''
    for signature in [
        'void VoiceCall::initializeOutputVolume()',
        'void VoiceCall::acquireOutputVolume(QObject *owner,std::function<void()> ready)',
        'void VoiceCall::releaseOutputVolume(QObject *owner)',
    ]:
        code += '\n' + function(source, signature) + '\n'
    code += r'''
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);QObject call,prompt;
    VoiceCall::initializeOutputVolume();
    auto &focus=AudioPromptFocusManager::instance();int ready=0;
    focus.acquire(&prompt);
    VoiceCall::acquireOutputVolume(&call,[&]() { ++ready; });
    check(ready==0,"call setup never waits for gain synchronously");pump();
    check(ready==1 && focus.owners.size()==2,"call readiness and existing prompt ownership coexist");
    focus.focusReady(&call);check(ready==1,"duplicate ready event does not restart audio");
    VoiceCall::releaseOutputVolume(&call);
    check(focus.owners.size()==1 && focus.owners.contains(&prompt),"call hangup preserves overlapping prompt focus");
    VoiceCall::acquireOutputVolume(&call,[&]() { ++ready; });VoiceCall::releaseOutputVolume(&call);pump();
    check(ready==1,"cancelled hardware setup cannot run stale call callback");
    check(!call.findChild<QObject *>("voiceCallVolumeLease"),"cancelled ticket is released");
    VoiceCall::releaseOutputVolume(&call);focus.release(&prompt);check(focus.owners.isEmpty(),"all owners restored without duplicate release");
    std::puts("PASS: production call-volume wrappers are asynchronous, cancellable, idempotent on ready, and preserve other focus owners");
}
#include "volume.moc"
'''
    (output / 'volume.cpp').write_text(code, encoding='utf-8')
    (output / 'volume.pro').write_text(
        'QT += core\nQT -= gui\nCONFIG += console c++17\nSOURCES += volume.cpp\nTARGET = volume\n'
        + 'INCLUDEPATH += "' + root.as_posix() + '"\n', encoding='utf-8')
    env = dict(os.environ)
    if os.name == 'nt':
        qt = root / '.web-build-tools/sdk/5.15.2/mingw81_64/bin'
        gcc = root / '.web-build-tools/sdk/Tools/mingw810_64/bin'
        env['PATH'] = str(qt) + os.pathsep + str(gcc) + os.pathsep + env.get('PATH', '')
        qmake, make, binary = str(qt / 'qmake.exe'), str(gcc / 'mingw32-make.exe'), output / 'release/volume.exe'
    else:
        qmake, make, binary = 'qmake', 'make', output / 'volume'
    subprocess.run([qmake, 'volume.pro'], cwd=output, env=env, check=True, timeout=30)
    subprocess.run([make, '-j4'], cwd=output, env=env, check=True, timeout=60)
    subprocess.run([str(binary)], cwd=output, env=env, check=True, timeout=15)


if __name__ == '__main__':
    main()
