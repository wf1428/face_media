"""Compile the real shared-mixer handoff/removal methods with a controlled GST boundary.

Author: Du lin
Date: 2026-10-29
Note: 最近修改：2026-11-11；修改人：Du lin；验证普通播报在共享输出重开失败时仍报告旧 PCM 已确认清空，同时保留未确认清空和通话独占用例。
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
#include <QDebug>
#include <QMutex>
#include <QAtomicInt>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QHash>
#include <QVector>
#include <functional>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
enum GstState { GST_STATE_NULL,GST_STATE_PAUSED,GST_STATE_PLAYING };
enum { GST_STATE_CHANGE_FAILURE=0,GST_STATE_CHANGE_SUCCESS=1,GST_STATE_CHANGE_ASYNC=2 };
using GstClockTime=unsigned long long;
constexpr unsigned long long GST_CLOCK_TIME_NONE=~0ULL,GST_SECOND=1000000000ULL;
struct GstElement { GstState state=GST_STATE_PLAYING; int refs=1; };
enum GstFlowReturn { GST_FLOW_OK, GST_FLOW_FLUSHING, GST_FLOW_ERROR };
constexpr unsigned long long GST_MSECOND=1000000ULL;
struct GstSample { int refs=1; };
struct GstPlayerWidget {
    QMutex gstMutex_;
    GstSample *promptPrerollSample_=nullptr;
    std::atomic<quint64> pipelineRevision_{1};
    QAtomicInt promptAudioPrimed_{0}, promptLeadInQueued_{0};
    GstFlowReturn primeManagedPrompt(quint64 revision);
};
static GstSample *gst_sample_ref(GstSample *sample) { ++sample->refs;return sample; }
static void gst_sample_unref(GstSample *sample) { --sample->refs; }
static void g_usleep(unsigned) {}
using gboolean = int;
using gpointer = void *;
struct GstPad { bool muted=false; int changes=0; };
struct GstPadProbeInfo {};
using GstBus = GstElement;
struct GstMessage { int type; GstState state; GstElement *source; };
enum { GST_MESSAGE_STATE_CHANGED, GST_MESSAGE_ERROR, GST_BUS_DROP };
using GstBusSyncReply = int;
#define GST_MESSAGE_TYPE(message) ((message)->type)
#define GST_MESSAGE_SRC(message) ((message)->source)
#define GST_IS_PIPELINE(source) ((source) != nullptr)
using gchar = char;
struct GError { char *message; };
static void gst_message_parse_error(GstMessage *,GError **,gchar **) {}
static void gst_message_parse_state_changed(GstMessage *message,void *,GstState *state,void *) {
    *state=message->state;
}
static void g_error_free(GError *) {}
static void g_free(gchar *) {}
enum GstPadProbeReturn { GST_PAD_PROBE_OK, GST_PAD_PROBE_DROP };
#define G_OBJECT(p) (p)
static void g_object_set(GstPad *pad,const char *,gboolean mute,decltype(nullptr)) {
    pad->muted=mute;
    ++pad->changes;
}
static std::function<void()> stateHook;
static QMutex *checkedMutex;
static int stateCalls=0,waitCalls=0,destroyed=0;
static GstClockTime lastWait=0;
static bool nullTimeout=false, playingTimeout=false, failPlaying=false;
static void check(bool ok,const char *message) { if (!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); } }
static GstElement *gst_object_ref(GstElement *p) { ++p->refs; return p; }
static void gst_object_unref(GstElement *p) { --p->refs; }
#define GST_ELEMENT(p) (p)
static void gst_bus_set_sync_handler(GstElement *,decltype(nullptr),decltype(nullptr),decltype(nullptr)) {}
static int gst_element_set_state(GstElement *p,GstState target) {
    check(checkedMutex->tryLock(),"GST state transition must not hold callback table mutex"); checkedMutex->unlock();
    ++stateCalls;p->state=target;
    if(stateHook) { auto hook=stateHook; stateHook=nullptr; hook(); }
    if(target==GST_STATE_PLAYING && failPlaying) return GST_STATE_CHANGE_FAILURE;
    return target==GST_STATE_PLAYING ? GST_STATE_CHANGE_ASYNC : GST_STATE_CHANGE_SUCCESS;
}
static int gst_element_get_state(GstElement *p,GstState *state,void *,GstClockTime timeout) {
    lastWait=timeout;
    ++waitCalls;
    const bool timeoutNow = p->state==GST_STATE_PLAYING ? playingTimeout : nullTimeout;
    *state=p->state==GST_STATE_PLAYING && timeoutNow ? GST_STATE_PAUSED : p->state;
    return timeoutNow ? GST_STATE_CHANGE_ASYNC : GST_STATE_CHANGE_SUCCESS;
}
class SharedAudioMixer {
public:
    static SharedAudioMixer *current;
    static SharedAudioMixer &instance() { return *current; }
    struct SourceBranch { unsigned long long nextPts=10; };
    QMutex mutex_;QWaitCondition removalFinished_;
    QHash<GstPlayerWidget *,SourceBranch *> sources_;
    QVector<SourceBranch *> deferredSources_;
    int activeRemovals_=0; bool callExclusive_=false;
    GstElement *pipeline_, *mixer_=nullptr, *outputSink_=nullptr, *bus_=nullptr;
    std::atomic_bool outputFault_{false}, broadcastRecoveryPending_{false};
    std::mutex outputMutex_;
    std::condition_variable outputChanged_;
    bool callOutputRequested_=false;
    std::atomic_bool *broadcastStop_=nullptr;
    std::atomic_bool broadcastOutputActive_{false};
    std::atomic<uint64_t> ringtoneToken_{0};
    std::atomic<const void *> priorityOwner_{nullptr};
    std::atomic_bool videoAudioHeld_{false};
    int prerollAttempts=0;
    int flushingAttempts=0;
    bool dropPreroll=false;
    GstFlowReturn pushSilentPreroll(GstPlayerWidget *, GstSample *, unsigned long long,
                                   uint64_t, bool *submitted) {
        ++prerollAttempts;
        *submitted=false;
        if (flushingAttempts>0) { --flushingAttempts;return GST_FLOW_FLUSHING; }
        *submitted=!dropPreroll;
        return GST_FLOW_OK;
    }
    void destroySourceLocked(SourceBranch *branch) { ++destroyed; delete branch; }
'''
    code += method(source, 'bool setCallExclusive(bool exclusive, GstClockTime timeout = GST_SECOND)')
    code += '\n' + method(source, 'void removeSource(GstPlayerWidget *owner)')
    for signature in ('bool acquireBroadcastOutput(std::atomic_bool &stop)',
                      'bool releaseBroadcastOutput()', 'bool sharedOutputReady()',
                      'bool resetSharedOutput()', 'bool setCallOutput(bool exclusive)',
                      'bool discardQueuedAudioForPrompt(bool *nullConfirmed = nullptr)',
                      'static GstBusSyncReply busSyncHandler(GstBus *, GstMessage *message, gpointer data)'):
        code += '\n' + method(source, signature)
    for signature in ('enum class AudioDisposition {', 'struct AudioGate {'):
        start = source.index('    ' + signature)
        code += source[start:source.index('\n    };', start) + len('\n    };')] + '\n'
    code += method(source, 'AudioDisposition audioDisposition(const void *owner, uint64_t token, bool managed) const')
    code += '\n' + method(source, 'static GstPadProbeReturn applyQueuedAudioGate(GstPad *pad, GstPadProbeInfo *, gpointer data)')
    code += '\n};\n'
    start = source.index('GstFlowReturn GstPlayerWidget::primeManagedPrompt(quint64 revision)')
    code += source[start:source.index('\n}', start) + 2]
    code += r'''
SharedAudioMixer *SharedAudioMixer::current=nullptr;
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);GstElement pipeline;
    SharedAudioMixer mixer;mixer.pipeline_=&pipeline;checkedMutex=&mixer.mutex_;
    SharedAudioMixer::current=&mixer;
    GstPlayerWidget first,second;
    using Decision=SharedAudioMixer::AudioDisposition;
    auto video=SharedAudioMixer::AudioGate{&first,0,false};
    auto live=SharedAudioMixer::AudioGate{&second,0,true};
    GstPad videoPad,livePad;
    check(mixer.applyQueuedAudioGate(&videoPad,nullptr,&video)==GST_PAD_PROBE_OK && !videoPad.muted,
        "video is initially audible");
    check(mixer.applyQueuedAudioGate(&livePad,nullptr,&live)==GST_PAD_PROBE_DROP,
        "managed input is gated before focus");
    mixer.priorityOwner_=&second;
    check(mixer.audioDisposition(&first,0,false)==Decision::Mute &&
        mixer.applyQueuedAudioGate(&videoPad,nullptr,&video)==GST_PAD_PROBE_OK && videoPad.muted,
        "live mutes the video pad while its timestamped buffers continue");
    check(mixer.applyQueuedAudioGate(&videoPad,nullptr,&video)==GST_PAD_PROBE_OK && videoPad.changes==1,
        "consecutive video buffers keep their timeline without repeated property writes");
    check(mixer.applyQueuedAudioGate(&livePad,nullptr,&live)==GST_PAD_PROBE_OK && !livePad.muted,
        "live pad remains audible beside the muted video input");
    mixer.priorityOwner_=&mixer;
    check(mixer.applyQueuedAudioGate(&videoPad,nullptr,&video)==GST_PAD_PROBE_DROP &&
        mixer.applyQueuedAudioGate(&livePad,nullptr,&live)==GST_PAD_PROBE_DROP,
        "call focus continues to block shared media");
    mixer.ringtoneToken_=8;
    check(mixer.audioDisposition(&second,0,true)==Decision::Drop &&
        mixer.audioDisposition(&second,7,true)==Decision::Drop &&
        mixer.audioDisposition(&first,8,false)==Decision::Play,
        "call ringtone excludes live, stale ringtone and prompts");
    mixer.ringtoneToken_=0;
    mixer.priorityOwner_=&second;
    check(mixer.applyQueuedAudioGate(&livePad,nullptr,&live)==GST_PAD_PROBE_OK,
        "live owner resumes after call");
    mixer.priorityOwner_=nullptr;
    check(mixer.applyQueuedAudioGate(&videoPad,nullptr,&video)==GST_PAD_PROBE_OK &&
        !videoPad.muted && videoPad.changes==2,
        "video recovers audio on the same input after live stops");
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
    mixer.sources_.insert(&first,new SharedAudioMixer::SourceBranch);
    std::atomic_bool stopLive{false}, callReady{false};
    check(mixer.acquireBroadcastOutput(stopLive) && pipeline.state==GST_STATE_NULL,
        "live opens only after shared sink is NULL");
    std::thread call([&] { callReady=mixer.setCallOutput(true); });
    const auto limit=std::chrono::steady_clock::now()+std::chrono::milliseconds(750);
    while (!stopLive && std::chrono::steady_clock::now()<limit) std::this_thread::yield();
    check(stopLive && !callReady,"call cancels live and waits for PCM close");
    mixer.releaseBroadcastOutput();
    call.join();
    check(callReady && pipeline.state==GST_STATE_NULL,"no sink restart between live and call");
    std::atomic_bool nextLive{false};
    check(!mixer.acquireBroadcastOutput(nextLive),"live cannot preempt an active call");
    check(mixer.setCallOutput(false) && pipeline.state==GST_STATE_PLAYING,"call returns speaker");
    check(mixer.acquireBroadcastOutput(nextLive),"live can reacquire after call");
    check(mixer.releaseBroadcastOutput() && pipeline.state==GST_STATE_PLAYING &&
          lastWait==200*GST_MSECOND,"pausing live confirms the shared sink");
    mixer.removeSource(&first);
    mixer.sources_.insert(&first,new SharedAudioMixer::SourceBranch);
    mixer.videoAudioHeld_=true;
    check(mixer.audioDisposition(&first,0,false)==Decision::Mute,
        "video remains muted while gain is restoring without a focus owner");
    bool nullConfirmed=false;
    check(mixer.discardQueuedAudioForPrompt(&nullConfirmed) && nullConfirmed && lastWait==200*GST_MSECOND &&
          pipeline.state==GST_STATE_PLAYING && !mixer.callExclusive_,
        "prompt confirms NULL then restores the shared sink");
    nullConfirmed=false;
    failPlaying=true;
    check(!mixer.discardQueuedAudioForPrompt(&nullConfirmed) && nullConfirmed &&
          pipeline.state==GST_STATE_PLAYING && !mixer.callExclusive_,
        "failed shared resume retains confirmed PCM clear for prompt gain");
    failPlaying=false;
    nullTimeout=true;
    nullConfirmed=true;
    check(!mixer.discardQueuedAudioForPrompt(&nullConfirmed) && !nullConfirmed &&
          pipeline.state==GST_STATE_PLAYING &&
          !mixer.callExclusive_,"unconfirmed clear restores sink but denies gain boost");
    nullTimeout=false;
    nullConfirmed=true;
    check(mixer.setCallOutput(true) && !mixer.discardQueuedAudioForPrompt(&nullConfirmed) &&
          !nullConfirmed &&
          pipeline.state==GST_STATE_NULL,"prompt cannot touch call-owned output");
    check(mixer.setCallOutput(false),"call returns output");
    mixer.videoAudioHeld_=false;
    check(mixer.audioDisposition(&first,0,false)==Decision::Play,"video gate opens after restore");
    std::atomic_bool retryLive{false};
    check(mixer.acquireBroadcastOutput(retryLive),"live starts before injected restore timeout");
    playingTimeout=true;
    check(mixer.releaseBroadcastOutput() && !mixer.pipeline_ && mixer.sources_.isEmpty() &&
          pipeline.state==GST_STATE_NULL && pipeline.refs==0 && !mixer.callExclusive_,
          "restore timeout retires only shared audio; next video PCM can rebuild it");
    check(mixer.audioDisposition(&first,0,false)==Decision::Mute,
          "video remains muted while replacement output is not yet playing");
    GstElement rebuilt;
    GstMessage stateMessage{GST_MESSAGE_STATE_CHANGED,GST_STATE_PAUSED,&rebuilt};
    mixer.busSyncHandler(nullptr,&stateMessage,&mixer);
    check(mixer.audioDisposition(&first,0,false)==Decision::Mute,"preroll does not unmute video");
    stateMessage.state=GST_STATE_PLAYING;
    mixer.busSyncHandler(nullptr,&stateMessage,&mixer);
    check(mixer.audioDisposition(&first,0,false)==Decision::Play,"rebuilt output unblocks video at PLAYING");
    GstPlayerWidget prompt;
    check(prompt.primeManagedPrompt(1)==GST_FLOW_FLUSHING && mixer.prerollAttempts==0,
        "play waits for the first decoded sample");
    GstSample initial;
    prompt.promptPrerollSample_=&initial;
    mixer.dropPreroll=true;
    check(prompt.primeManagedPrompt(1)==GST_FLOW_FLUSHING &&
          !prompt.promptLeadInQueued_.loadAcquire() && initial.refs==1,
        "focus drop cannot masquerade as successful warmup");
    mixer.dropPreroll=false;
    mixer.flushingAttempts=2;
    check(prompt.primeManagedPrompt(1)==GST_FLOW_OK && mixer.prerollAttempts==4 &&
          prompt.promptLeadInQueued_.loadAcquire() && initial.refs==1,
        "warmup retries and retains initial sample ownership before PLAYING");
    prompt.promptLeadInQueued_.storeRelease(0);
    mixer.flushingAttempts=25;
    check(prompt.primeManagedPrompt(1)==GST_FLOW_ERROR && mixer.prerollAttempts==24 &&
          !prompt.promptLeadInQueued_.loadAcquire() && initial.refs==1,
        "persistent output failure stops retries without losing the retained sample");
    std::puts("PASS: real mixer call handoff, deferred UI teardown, callback mutex exclusion, asynchronous preroll, EOS during restore, priority, video mute and resume, ringtone gate, file prompt preroll");
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
