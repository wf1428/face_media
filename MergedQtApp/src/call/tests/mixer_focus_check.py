"""Compile the real shared-mixer handoff/removal methods with a controlled GST boundary.

Author: Du lin
Date: 2026-10-29
Requires local QtCore development files. Tests deferred UI removal, lock-free GST state
changes, asynchronous preroll restoration, and an EOS during restoration. No media runs.
"""
import os
from pathlib import Path
import subprocess


def method(source, signature):
    """Extract one inline production method; raise ValueError if it is absent."""
    start = source.index('    ' + signature + '\n')
    return source[start:source.index('\n    }', start) + len('\n    }')]


def main():
    """Generate, compile and run the handoff fixture; build or assertion failures raise."""
    root = Path.cwd()
    output = root / '.web-build/voice-call-20261029/mixer'
    output.mkdir(parents=True, exist_ok=True)
    source = (root / 'components/multimedia/gst_player_widget.cpp').read_text(encoding='utf-8')
    code = r'''
#include <QCoreApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QHash>
#include <QVector>
#include <functional>
#include <cstdio>
#include <cstdlib>
enum GstState { GST_STATE_NULL,GST_STATE_PAUSED,GST_STATE_PLAYING };
enum { GST_STATE_CHANGE_FAILURE=0,GST_STATE_CHANGE_SUCCESS=1,GST_STATE_CHANGE_ASYNC=2 };
constexpr unsigned long long GST_CLOCK_TIME_NONE=~0ULL,GST_SECOND=1000000000ULL;
struct GstElement { GstState state=GST_STATE_PLAYING; int refs=1; };
struct GstPlayerWidget {};
static std::function<void()> stateHook;
static QMutex *checkedMutex;
static int stateCalls=0,waitCalls=0,destroyed=0;
static void check(bool ok,const char *message) { if (!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); } }
static GstElement *gst_object_ref(GstElement *p) { ++p->refs; return p; }
static void gst_object_unref(GstElement *p) { --p->refs; }
#define GST_ELEMENT(p) (p)
static int gst_element_set_state(GstElement *p,GstState target) {
    check(checkedMutex->tryLock(),"GST state transition must not hold callback table mutex"); checkedMutex->unlock();
    ++stateCalls;p->state=target;
    if(stateHook) { auto hook=stateHook; stateHook=nullptr; hook(); }
    return target==GST_STATE_PLAYING ? GST_STATE_CHANGE_ASYNC : GST_STATE_CHANGE_SUCCESS;
}
static int gst_element_get_state(GstElement *p,GstState *state,void *,unsigned long long) {
    ++waitCalls;*state=p->state==GST_STATE_PLAYING ? GST_STATE_PAUSED : p->state;
    return p->state==GST_STATE_PLAYING ? GST_STATE_CHANGE_ASYNC : GST_STATE_CHANGE_SUCCESS;
}
class SharedAudioMixer {
public:
    struct SourceBranch { unsigned long long nextPts=10; };
    QMutex mutex_;QWaitCondition removalFinished_;
    QHash<GstPlayerWidget *,SourceBranch *> sources_;
    QVector<SourceBranch *> deferredSources_;
    int activeRemovals_=0; bool callExclusive_=false;
    GstElement *pipeline_;
    void destroySourceLocked(SourceBranch *branch) { ++destroyed; delete branch; }
'''
    code += method(source, 'bool setCallExclusive(bool exclusive)')
    code += '\n' + method(source, 'void removeSource(GstPlayerWidget *owner)')
    code += r'''
};
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);GstElement pipeline;
    SharedAudioMixer mixer;mixer.pipeline_=&pipeline;checkedMutex=&mixer.mutex_;
    GstPlayerWidget first,second;
    mixer.sources_.insert(&first,new SharedAudioMixer::SourceBranch);
    mixer.sources_.insert(&second,new SharedAudioMixer::SourceBranch);
    check(mixer.setCallExclusive(true) && pipeline.state==GST_STATE_NULL,"call releases physical output");
    const int before=stateCalls;
    mixer.removeSource(&first);
    check(stateCalls==before && destroyed==0 && mixer.deferredSources_.size()==1,"UI removes only ownership during call");
    check(mixer.setCallExclusive(false),"asynchronous restore accepted before preroll");
    check(waitCalls==1 && !mixer.callExclusive_ && destroyed==1 && pipeline.state==GST_STATE_PLAYING,"restore gate opens without waiting on its own muted input");
    check(mixer.setCallExclusive(true),"second call releases output");
    stateHook=[&]() { mixer.removeSource(&second); };
    check(mixer.setCallExclusive(false),"EOS arriving during restore handled");
    check(mixer.sources_.isEmpty() && mixer.deferredSources_.isEmpty() && destroyed==2,"all retired branches released by background path");
    check(pipeline.state==GST_STATE_NULL && pipeline.refs==1 && !mixer.callExclusive_,"empty restored mixer releases output without leaked refs");
    std::puts("PASS: real mixer call handoff, deferred UI teardown, callback mutex exclusion, asynchronous preroll, EOS during restore");
}
'''
    project = output / 'mixer.pro'
    project.write_text('QT += core\nQT -= gui\nCONFIG += console c++17\nSOURCES += mixer.cpp\nTARGET = mixer\n', encoding='utf-8')
    (output / 'mixer.cpp').write_text(code, encoding='utf-8')
    env = dict(os.environ)
    if os.name == 'nt':
        qt = root / '.web-build-tools/sdk/5.15.2/mingw81_64/bin'
        gcc = root / '.web-build-tools/sdk/Tools/mingw810_64/bin'
        env['PATH'] = str(qt) + os.pathsep + str(gcc) + os.pathsep + env.get('PATH', '')
        qmake, make = str(qt / 'qmake.exe'), str(gcc / 'mingw32-make.exe')
        binary = output / 'release/mixer.exe'
    else:
        qmake, make, binary = 'qmake', 'make', output / 'mixer'
    subprocess.run([qmake, str(project)], cwd=output, env=env, check=True, timeout=30)
    subprocess.run([make, '-j4'], cwd=output, env=env, check=True, timeout=60)
    subprocess.run([str(binary)], cwd=output, env=env, check=True, timeout=15)


if __name__ == '__main__':
    main()
