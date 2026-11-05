"""Compile unmodified mqttd call functions against real cJSON and a controlled transport.

Author: Du lin
Date: 2026-10-29
Note: 最近修改：2026-10-30；修改人：Du lin；验证 5 帧窗口、上行限长/过期及发送失败 token 缺口的信用恢复，复用原重连回归。
Run from MergedQtApp. Requires a C compiler and cJSON.c/.h via --cjson-dir.
No broker or device is accessed. Also reruns the existing threaded reconnect fixture.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess


def function(source, signature):
    """Return one complete production function; raise ValueError if not found."""
    start = source.index(signature + '\n{')
    return source[start:source.index('\n}', start) + 2]


def harness(source):
    """Build a C fixture using real call functions and cJSON, substituting only I/O.

    Args:
        source: Current UTF-8 mqttd source with normalized newlines.
    Returns:
        A standalone native C regression program.
    Raises:
        ValueError: A required production declaration or function is missing.
    """
    start = source.index('typedef enum {\n    IPC_TX_ITEM_LINE')
    definitions = source[start:source.index('} mqttd_cfg_t;', start) + len('} mqttd_cfg_t;')]
    backend = r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include "cJSON.h"
struct mosquitto { int unused; };
struct mosquitto_message { int mid; char *topic; void *payload; int payloadlen; int qos; bool retain; };
enum { MOSQ_ERR_SUCCESS=0, MOSQ_ERR_INVAL=3 };
#define MSG_NOSIGNAL 0
static long long now=1000;
static int publishes,ack_count,last_ok,immediate,send_count,closed,business;
static int send_mode;
static char last_line[4096],sent[4096];
static unsigned char published[650];
static long long monotonic_millis(void) { return now; }
static void check(int ok,const char *text) { if(!ok) { fprintf(stderr,"FAIL: %s\n",text); exit(1); } }
'''
    io = r'''
static void on_call_publish(struct mosquitto *,void *,int);
static void ipc_send_line(mqttd_cfg_t *cfg,const char *line) {
    (void)cfg; ++ack_count; snprintf(last_line,sizeof(last_line),"%s",line);
    cJSON *obj=cJSON_Parse(line); last_ok=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(obj,"ok")); cJSON_Delete(obj);
}
static int ipc_dup_client(mqttd_cfg_t *cfg,unsigned long *gen) { (void)cfg; *gen=1; return 1; }
static void ipc_close_client_if_generation(mqttd_cfg_t *cfg,unsigned long gen) { (void)cfg;(void)gen; ++closed; }
static int fake_close(int fd) { (void)fd; return 0; }
static long long fake_send(int fd,const void *data,size_t len,int flags) {
    (void)fd;(void)flags; ++send_count;
    if(send_mode==1) { errno=EAGAIN; return -1; }
    if(send_mode==2) return (long long)len/2;
    memcpy(sent,data,len);sent[len]=0; return (long long)len;
}
#define send fake_send
#define close fake_close
static int ipc_tx_enqueue_mqtt_message(mqttd_cfg_t *cfg,const struct mosquitto_message *msg) { (void)cfg;(void)msg; ++business; return 0; }
static int mosquitto_publish(struct mosquitto *m,int *mid,const char *topic,int len,const void *payload,int qos,bool retain) {
    (void)m; check(strcmp(topic,"device/ycLinux/D/audio/up")==0,"exact uplink topic");
    check(len==650 && qos==0 && !retain,"wire size, QoS0, non-retained");
    memcpy(published,payload,650); ++publishes; *mid=7;
    if(immediate) on_call_publish(m,(void *)m,7);
    return 0;
}
'''
    signatures = [
        'static void call_reset(mqttd_cfg_t *cfg)',
        'static void call_connected(mqttd_cfg_t *cfg)',
        'static void call_release_rx(mqttd_cfg_t *cfg,const char *epoch,unsigned long token)',
        'static void call_send_rx(mqttd_cfg_t *cfg)',
        'static void call_ack(mqttd_cfg_t *cfg,const char *type,const char *epoch,int ok)',
        'static void on_call_publish(struct mosquitto *mosq,void *userdata,int mid)',
        'static void call_tick(mqttd_cfg_t *cfg)',
        'static int call_hex(unsigned char c)',
        'static void call_command(mqttd_cfg_t *cfg,const char *line)',
        'static void on_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg)',
    ]
    code = '\n\n'.join(function(source, signature) for signature in signatures)
    tests = r'''
static const char *epoch="12345678-1234-1234-1234-123456789abc";
static void enable(mqttd_cfg_t *cfg) {
    char text[256]; snprintf(text,sizeof(text),"{\"cmd\":\"call_media\",\"enabled\":true,\"epoch\":\"%s\",\"topic\":\"device/ycLinux/D/audio\"}",epoch);
    call_command(cfg,text);
}
static void publish(mqttd_cfg_t *cfg,const unsigned char *packet) {
    char hex[1301],text[1800];
    for(int i=0;i<650;++i) sprintf(hex+2*i,"%02x",packet[i]);
    snprintf(text,sizeof(text),"{\"cmd\":\"publish_audio\",\"epoch\":\"%s\",\"topic\":\"device/ycLinux/D/audio/up\",\"hex\":\"%s\"}",epoch,hex);
    call_command(cfg,text);call_tick(cfg);
}
int main(void) {
    mqttd_cfg_t *cfg=calloc(1,sizeof(*cfg)); check(cfg!=NULL,"allocate fixture");
    pthread_mutex_init(&cfg->ipc_tx_lock,NULL); pthread_mutex_init(&cfg->state_lock,NULL); pthread_cond_init(&cfg->ipc_tx_cond,NULL);
    cfg->mosq=(struct mosquitto *)cfg; cfg->mqtt_connected=1;
    unsigned char packet[650]; for(int i=0;i<650;++i) packet[i]=(unsigned char)i;
    packet[0]=0xA1;packet[1]=0xC0;packet[2]=1;packet[3]=0;packet[4]=1;
    enable(cfg); check(cfg->call_enabled && last_ok,"media handshake");
    immediate=1;publish(cfg,packet);check(publishes==1 && memcmp(packet,published,650)==0,"all binary bytes survive IPC hex");
    call_tick(cfg);check(!cfg->call_tx_mid && last_ok,"callback before publish returns restores credit");
    immediate=0; publish(cfg,packet);publish(cfg,packet);check(publishes==2 && cfg->call_tx_mid==7,"only one in-flight packet");
    call_reset(cfg); enable(cfg);check(!last_ok && !cfg->call_enabled,"old in-flight packet blocks new media epoch");
    on_call_publish(cfg->mosq,cfg,7);call_tick(cfg);enable(cfg);check(last_ok,"completion frees old packet");
    publish(cfg,packet);now+=501;call_tick(cfg);check(!cfg->call_enabled && !last_ok,"500ms congestion closes gate");
    publish(cfg,packet);check(publishes==3,"no disconnected or congested backlog");
    on_call_publish(cfg->mosq,cfg,7);call_tick(cfg);enable(cfg);
    publish(cfg,packet);
    for(int i=0;i<10;++i) publish(cfg,packet);
    check(cfg->call_up_count==5,"uplink queue remains bounded behind one MQTT in-flight");
    const int before=publishes;
    now+=101;on_call_publish(cfg->mosq,cfg,7);call_tick(cfg);
    check(!cfg->call_up_count && publishes==before,"queued audio expires without replay");
    call_reset(cfg);enable(cfg);
    struct mosquitto_message msg={0,"device/ycLinux/D/audio",packet,650,0,false};
    for(int i=0;i<60;++i) on_message(cfg->mosq,cfg,&msg);
    check(cfg->call_rx_count==10 && cfg->call_rx_drops==50,"bounded receive ring drops oldest");
    call_send_rx(cfg);check(send_count==1 && cfg->call_rx_pending,"one receive credit");
    cJSON *out=cJSON_Parse(sent);check(out!=NULL,"one complete JSON line");
    cJSON *hex=cJSON_GetObjectItemCaseSensitive(out,"hex");check(cJSON_IsString(hex) && strlen(hex->valuestring)==1300,"zero bytes preserved on downlink");
    cJSON_Delete(out);
    for(int i=0;i<4;++i) call_send_rx(cfg);
    check(send_count==5 && cfg->call_rx_pending==5,"five-frame receive window");
    call_send_rx(cfg);check(send_count==5,"socket audio window stays bounded");
    char ack[256];snprintf(ack,sizeof(ack),"{\"cmd\":\"audio_received\",\"epoch\":\"%s\",\"token\":%lu}",epoch,cfg->call_rx_token+1);
    call_command(cfg,ack);check(cfg->call_rx_pending,"stale token cannot release credit");
    snprintf(ack,sizeof(ack),"{\"cmd\":\"audio_received\",\"epoch\":\"%s\",\"token\":%lu}",epoch,cfg->call_rx_token);
    call_command(cfg,ack);check(!cfg->call_rx_pending,"consumption restores receive credit");
    now+=201;call_send_rx(cfg);check(cfg->call_rx_count==0 && send_count==5,"expired audio never played later");
    msg.retain=true;on_message(cfg->mosq,cfg,&msg);check(!cfg->call_rx_count,"retained audio rejected");msg.retain=false;
    msg.topic="device/ycLinux/Other/audio";on_message(cfg->mosq,cfg,&msg);check(!cfg->call_rx_count,"other devices rejected");msg.topic="device/ycLinux/D/audio";
    msg.payloadlen=4096;on_message(cfg->mosq,cfg,&msg);check(!cfg->call_rx_count,"oversized PCM rejected");msg.payloadlen=650;
    packet[4]=2;on_message(cfg->mosq,cfg,&msg);check(!cfg->call_rx_count,"invalid version rejected");packet[4]=1;
    on_message(cfg->mosq,cfg,&msg);send_mode=1;call_send_rx(cfg);check(!cfg->call_rx_pending && !closed,"EAGAIN drops audio without disconnecting business IPC");
    on_message(cfg->mosq,cfg,&msg);send_mode=0;call_send_rx(cfg);
    snprintf(ack,sizeof(ack),"{\"cmd\":\"audio_received\",\"epoch\":\"%s\",\"token\":%lu}",epoch,cfg->call_rx_token);
    call_command(cfg,ack);check(!cfg->call_rx_pending,"failed-send token gap does not leak receive credit");
    on_message(cfg->mosq,cfg,&msg);send_mode=2;call_send_rx(cfg);check(closed==1,"partial JSON closes IPC instead of corrupting next frame");
    call_reset(cfg);check(!cfg->call_rx_count && !cfg->call_rx_pending,"hangup flushes all audio");
    cfg->call_tx_mid=7;call_connected(cfg);check(!cfg->call_tx_mid,"successful reconnect releases abandoned QoS0 credit");
    msg.topic="business/request";on_message(cfg->mosq,cfg,&msg);check(business==1,"business queue unchanged");
    free(cfg);puts("PASS: real mqttd binary framing, single in-flight credit, early callback, bounded receive, expiry, exact topics, reset, backpressure");return 0;
}
'''
    return backend + definitions + io + code + tests


def main():
    """Build and execute media and existing lifecycle regressions.

    Raises:
        OSError: Compiler, cJSON, or daemon source cannot be read.
        subprocess.CalledProcessError: Compilation or a regression fails.
        subprocess.TimeoutExpired: A build or test exceeds its bound.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path('../mqttd/mqttd.c'))
    parser.add_argument('--cjson-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=Path('.web-build/voice-call-20261029/mqttd'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    compiler = Path('.web-build-tools/sdk/Tools/mingw810_64/bin/gcc.exe').resolve() if os.name == 'nt' else Path(shutil.which('gcc') or 'gcc')
    env = dict(os.environ)
    env['PATH'] = str(compiler.parent) + os.pathsep + env.get('PATH', '')
    source = args.source.read_text(encoding='utf-8')
    spec = importlib.util.spec_from_file_location('reconnect', 'web/tests/mqttd_reconnect_check.py')
    lifecycle = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lifecycle)
    old = lifecycle.harness(source)
    # Keep the existing threaded fixture and production lifecycle bodies intact;
    # add only the new callback registration seam and real reset helper.
    index = old.index('static int mqtt_stop_and_destroy_locked(')
    old = old[:index] + function(source, 'static void call_reset(mqttd_cfg_t *cfg)') + '\n' + old[index:]
    old = '#define mosquitto_publish_callback_set(m,cb) ((void)(m))\n' + old
    old = old.replace('pthread_mutex_init(&cfg->mosq_lock,NULL);', 'pthread_mutex_init(&cfg->ipc_tx_lock,NULL);pthread_mutex_init(&cfg->mosq_lock,NULL);')
    for name, code in [('media', harness(source)), ('lifecycle', old)]:
        cfile = args.output / (name + '.c')
        cfile.write_text(code, encoding='utf-8')
        binary = args.output / (name + ('.exe' if os.name == 'nt' else ''))
        command = [str(compiler), '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-pthread', str(cfile), '-o', str(binary)]
        if name == 'media':
            command += ['-I' + str(args.cjson_dir), str(args.cjson_dir / 'cJSON.c')]
        subprocess.run(command, check=True, timeout=60, env=env)
        subprocess.run([str(binary.resolve())], check=True, timeout=25, env=env)


if __name__ == '__main__':
    main()
