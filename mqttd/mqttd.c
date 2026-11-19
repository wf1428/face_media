/**
 * @file mqttd.c
 * @brief libmosquitto 网络连接、配置重建与 Qt 本地 IPC 的守护进程。
 * @author Du lin
 * @note 最近修改：2026-11-18；修改人：Du lin；OTA 字符串载荷固定 QoS2、保留 long taskId 原值，并抑制安装过程发布日志与普通 ACK。
 */
/*
 * mqttd.c - minimal MQTT daemon using libmosquitto
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o mqttd mqttd.c $(pkg-config --cflags --libs libmosquitto libcjson) -lpthread
 *
 * Example (no TLS):
 *   ./mqttd -h 192.168.3.11 -p 1884 -i t113-001 -t test -v
 *
 * Example (user/pass):
 *   ./mqttd -h 192.168.3.11 -p 1884 -i t113-001 -u user -P pass -t test -v
 *
 * Example (publish once):
 *   ./mqttd -h 192.168.3.11 -p 1884 -i t113-001 -t test --pub-topic test --pub-msg '{"msg":"hi"}' -v
 *
 * Example (periodic heartbeat publish every 5s):
 *   ./mqttd -h 192.168.3.11 -p 1884 -i t113-001 -t test --pub-topic hb --pub-interval 5 --pub-msg '{"hb":1}' -v
 *
 * Example (TLS, CA verify):
 *   ./mqttd -h 192.168.3.11 -p 8883 -i t113-001 -t test --tls --cafile /etc/ssl/certs/ca.crt -v
 *
 * Example (TLS, skip verify - for debug only):
 *   ./mqttd -h 192.168.3.11 -p 8883 -i t113-001 -t test --tls --insecure -v
 */

#include <mosquitto.h>
#include <cJSON.h>

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* IPC 相关头文件 */
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>   // chmod
#include <sys/un.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

#define IPC_RX_INITIAL_CAP 8192U
#define IPC_RX_MAX_CAP (2U * 1024U * 1024U)
#define IPC_TX_TIMEOUT_MS 10000
#define IPC_TX_SOCKET_BUFFER (2 * 1024 * 1024)
#define IPC_TX_MAX_FRAME_BYTES (16U * 1024U * 1024U)
#define IPC_TX_MAX_QUEUE_BYTES (32U * 1024U * 1024U)
#define IPC_TX_MAX_QUEUE_ITEMS 512U
#define IPC_TX_MAX_RETRIES 3

/*
 * g_running：程序是否继续运行的“开关”
 * - 定义为 volatile sig_atomic_t：适合在信号处理函数中安全修改
 * - 当收到 SIGINT/SIGTERM 时，on_signal 会把它置为 0，让主循环退出
 */
static volatile sig_atomic_t g_running = 1;

typedef enum {
    IPC_TX_ITEM_LINE = 0,
    IPC_TX_ITEM_MQTT_MESSAGE = 1
} ipc_tx_item_kind_t;

/** @brief 直播突发帧暂存容量；按120ms上限丢弃陈旧帧。 */
#define BROADCAST_RX_CAPACITY 4

/** @brief 音频接收环元素；由 ipc_tx_lock 保护，回调只拷贝固定长度数据。 */
typedef struct {
    unsigned char pcm[650]; /**< 已校验的完整协议帧，含 10B 头。 */
    char topic[512]; /**< 精确下行音频主题，超长主题拒绝。 */
    long long arrival_ms; /**< CLOCK_MONOTONIC 接收时刻，毫秒。 */
} call_rx_frame_t;

/** @brief 尚未提交 MQTT 的上行 PCM；由 IPC 主线程生产/消费，短锁保护清理。 */
typedef struct {
    unsigned char pcm[650]; /**< 完整二进制音频帧。 */
    long long deadline_ms; /**< 本地单调时钟过期时间，最长接收后 100ms。 */
} call_up_frame_t;

typedef struct ipc_tx_item {
    ipc_tx_item_kind_t kind;
    char *line;
    size_t line_len;
    char *topic;
    char *payload;
    size_t payload_len;
    int qos;
    int retain;
    size_t accounted_bytes;
    unsigned long sequence;
    int attempts;
    struct ipc_tx_item *next;
} ipc_tx_item_t;


/**
 * @brief mqttd 运行配置、线程和 IPC 生命周期状态。
 * @details 主线程在 mosq_lock 内启动/停止网络线程；回调仅使用独立状态/IPC 锁，不获取 mosq_lock。
 * mqttd_cfg_t：运行配置结构体（把命令行参数解析的结果都存这里）
 * - 回调函数（on_connect/on_message 等）只给 userdata 指针
 *   把 cfg 地址作为 userdata 传进去，回调里就能访问所有配置
 */
typedef struct {
    /* ======== Broker 连接参数 ======== */

    char *host;          // Broker 的 IP 或域名
    int port;            // Broker 端口：普通 TCP 通常 1883；TLS 通常 8883
    char *client_id;     // MQTT ClientID：同一个 broker 内要尽量唯一；默认 mqttd-<pid>
    char *username;      // 账号
    char *password;      // 密码
    int keepalive;       // keepalive 秒数：MQTT 心跳间隔（保持连接活跃），默认 30
    bool clean_session;  // clean session：
                          // true  = “清洁会话”：每次连接都当新会话，断线后 broker 不保存订阅/未送达消息
                          // false = “持久会话”：broker 可保存订阅和 QoS1/2 未送达消息（依 broker 配置）
    bool verbose;        // 是否输出更详细日志：-v 开启

    /* ======== 订阅相关参数 ======== */

    char **topics;       // 订阅 topic 列表（字符串数组）
    int topic_count;     // topics 数组里 topic 的数量
    int qos;             // QoS 等级（0/1/2）：
                          // - QoS0：最多一次（可能丢）
                          // - QoS1：至少一次（可能重复）
                          // - QoS2：恰好一次（最可靠也最重）
                          // 这里用于订阅（subscribe）和发布（publish）的 QoS

    /* ======== 发布（Publish）相关参数 ======== */

    char *pub_topic;     // 要发布的 topic（可选）
    char *pub_msg;       // 要发布的消息 payload（可选，文本/JSON）
    int pub_interval_sec;// 周期性发布的间隔（秒）：
                          // 0 = 不周期发布
                          // >0 = 每 N 秒发布一次 pub_msg 到 pub_topic

    /* ======== TLS 相关参数（可选启用） ======== */

    bool tls_enable;     // 是否启用 TLS：--tls 开启后才走 mosquitto_tls_set
    char *cafile;        // CA 证书文件路径：用于校验 broker 的服务端证书（单向 TLS 必备）
    char *certfile;      // 客户端证书路径：用于双向认证 mTLS（可选）
    char *keyfile;       // 客户端私钥路径：用于双向认证 mTLS（可选）
    bool tls_insecure;   // 是否跳过服务端证书校验（仅调试用，量产不要）

    /* ======== IPC相关 ======== */
    char *ipc_path;      // Unix Socket 路径，/tmp/mqttd.sock
    int ipc_listen_fd;   // 监听 fd
    int ipc_client_fd;   // 当前已连接的 Qt 客户端 fd（简单起见：只保留 1 个）
    pthread_mutex_t ipc_lock; // 保护 ipc_client_fd 和连接代次
    unsigned long ipc_client_generation;

    pthread_t ipc_tx_thread;
    pthread_mutex_t ipc_tx_lock;
    pthread_cond_t ipc_tx_cond;
    ipc_tx_item_t *ipc_tx_head;
    ipc_tx_item_t *ipc_tx_tail;
    size_t ipc_tx_queue_bytes;
    size_t ipc_tx_queue_count;
    unsigned long ipc_tx_next_sequence;
    int ipc_tx_thread_started;
    int ipc_tx_stop;

    /* 以下通话状态统一由 ipc_tx_lock 保护；不得在持锁时调用 mosquitto/IPC 写入。 */
    unsigned char broadcast_packets[BROADCAST_RX_CAPACITY][1290]; /**< 直播下行FIFO，仅ipc_tx_lock保护。 */
    long long broadcast_arrival_ms[BROADCAST_RX_CAPACITY]; /**< 每槽的本地单调接收毫秒。 */
    unsigned int broadcast_head, broadcast_count; /**< 队首及有效帧数，最多四帧。 */
    size_t broadcast_expected; /**< 直播门接受的帧长度，0关闭。 */
    char broadcast_topic[160]; /**< 精确主题，受ipc_tx_lock保护。 */
    char broadcast_epoch[37]; /**< Qt本地UUID，切换会话时重置。 */
    unsigned long broadcast_seen, broadcast_accepted, broadcast_ipc_picked; /**< 每代次主题收帧、验帧和出队统计，受ipc_tx_lock保护。 */
    unsigned long broadcast_queue_drops; /**< 当前代次的FIFO溢出和过期丢弃数。 */
    int call_enabled; /**< true 允许当前本地媒体代次的 PCM。 */
    char call_audio_topic[512]; /**< 当前媒体精确下行主题，拒绝其他设备或通配符。 */
    char call_epoch[48]; /**< Qt 为每次媒体门切换生成 UUID，仅在 IPC 使用。 */
    call_rx_frame_t call_rx[10]; /**< 下行最多 200ms，溢出覆盖最旧帧。 */
    unsigned int call_rx_head, call_rx_count; /**< 接收环读游标与有效帧数。 */
    int call_rx_pending; /**< 已交给 IPC 尚未消费的帧数，最多 5，脱离逐帧停等。 */
    unsigned long call_rx_outstanding[5]; /**< 已成功/正在写出的 token 集，写失败可准确撤销，不以序号差代替窗口数量。 */
    unsigned long call_rx_acked_token; /**< 当前代次最后累计确认 token，重复确认不增加信用。 */
    call_up_frame_t call_up[5]; /**< 上行待提交最多 5 帧，过期/溢出丢最旧。 */
    unsigned int call_up_head,call_up_count; /**< 上行环游标/深度，由 ipc_tx_lock 保护。 */
    char call_tx_epoch[48]; /**< 唯一 MQTT 在途帧所属代次，隔离旧完成回调。 */
    unsigned long call_rx_token; /**< 下行信用序号；代次和序号都匹配才释放。 */
    int call_tx_mid; /**< 0 无在途；非零为唯一在途 QoS0 音频消息。 */
    unsigned long long call_callback_serial; /**< 发布回调递增计数，区分 mid 回绕。 */
    unsigned long long call_completed[65536]; /**< 每个 mid 最近一次完成序号，解决回调先于 publish 返回的竞争。 */
    unsigned long long call_tx_before; /**< 当前 publish 前回调计数。 */
    long long call_tx_since; /**< 唯一在途帧提交的本地毫秒。 */
    unsigned long call_rx_drops, call_tx_drops; /**< 音频溢出/过期及提交失败次数。 */

    /* ======== mosquitto 句柄（用于 apply 重建） ======== */
    struct mosquitto *mosq;
    int mosq_loop_started; /**< 1 表示 loop_start 成功且尚未 join；仅主线程在 mosq_lock 内读写。 */
    pthread_mutex_t mosq_lock;  // apply 时保护 mosq 重建

    /* ======== MQTT 连接状态（给 Qt 显示/测试连接用） ======== */
    pthread_mutex_t state_lock;
    int mqtt_connected;      // 1=已连接；0=未连接
    int last_conn_rc;        // 最近一次 CONNACK rc 或错误码

    char *ipc_rx_buf;
    size_t ipc_rx_len;
    size_t ipc_rx_cap;

    /* ======== apply 失败后的周期重试 ======== */
    int retry_apply_enabled;       // 1=需要重试，0=不需要
    int retry_apply_interval_sec;  // 重试间隔秒
    time_t retry_apply_next_ts;    // 下次重试时间点

} mqttd_cfg_t;

static int mqtt_apply_reconnect(mqttd_cfg_t *cfg);
static void on_log(struct mosquitto *mosq, void *userdata, int level, const char *str);
static void cancel_apply_retry(mqttd_cfg_t *cfg);
static void call_reset(mqttd_cfg_t *cfg);
static void broadcast_reset(mqttd_cfg_t *cfg);
static void call_connected(mqttd_cfg_t *cfg);
static void call_send_rx(mqttd_cfg_t *cfg);
static void call_tick(mqttd_cfg_t *cfg);
static void call_command(mqttd_cfg_t *cfg, const char *line);
static void on_call_publish(struct mosquitto *mosq, void *userdata, int mid);
static void schedule_apply_retry(mqttd_cfg_t *cfg, int delay_sec);

/*
 * 信号处理函数：
 * - 当用户 Ctrl+C（SIGINT）或系统发 SIGTERM 时触发
 * - 这里把 g_running = 0，主循环就会退出
 */
static void on_signal(int sig)
{
    (void)sig; // 避免未使用参数的编译警告
    g_running = 0;
}

/*
 * 打印帮助信息
 * prog：程序名 argv[0]
 */
static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "Options:\n"
        "  -h, --host <ip|name>        Broker host (default: 127.0.0.1)\n"
        "  -p, --port <port>           Broker port (default: 1883)\n"
        "  -i, --client-id <id>        Client ID (default: mqttd-<pid>)\n"
        "  -u, --username <user>       Username (optional)\n"
        "  -P, --password <pass>       Password (optional)\n"
        "  -k, --keepalive <sec>       Keepalive seconds (default: 30)\n"
        "      --clean                Clean session (default)\n"
        "      --no-clean             Persistent session\n"
        "  -t, --topic <topic>         Subscribe topic (repeatable)\n"
        "  -q, --qos <0|1|2>            QoS for subscribe/publish (default: 0)\n"
        "  -v, --verbose               Verbose logs\n"
        "\n"
        "Publish:\n"
        "      --pub-topic <topic>     Publish topic (optional)\n"
        "      --pub-msg <msg>         Publish payload (optional)\n"
        "      --pub-interval <sec>    Publish periodically every N seconds (optional)\n"
        "\n"
        "TLS:\n"
        "      --tls                   Enable TLS\n"
        "      --cafile <path>         CA file (optional)\n"
        "      --cert <path>           Client certificate (optional, for mTLS)\n"
        "      --key <path>            Client key (optional, for mTLS)\n"
        "      --insecure              Skip server cert verify (DEBUG ONLY)\n"
        "\n"
        "IPC:\n"
        "      --ipc <path>            Unix socket path (default: /tmp/mqttd.sock)\n"
        "\n",
        prog
    );
}


/*
 * set_defaults：设置 cfg 默认值
 * - 使用 memset 清零结构体
 * - 用 strdup 分配字符串（后续 free_cfg 会释放）
 */
static void set_defaults(mqttd_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->host = strdup("127.0.0.1");
    cfg->port = 1883;
    cfg->keepalive = 60;
    cfg->clean_session = true;
    cfg->qos = 0;
    cfg->verbose = false;

    cfg->tls_enable = false;
    cfg->tls_insecure = false;

    cfg->ipc_rx_cap = IPC_RX_INITIAL_CAP;
    cfg->ipc_rx_buf = (char *)malloc(cfg->ipc_rx_cap);
    if (!cfg->ipc_rx_buf) {
        fprintf(stderr, "[ipc] RX buffer allocation failed bytes=%zu\n", cfg->ipc_rx_cap);
        exit(2);
    }
    cfg->ipc_rx_len = 0;
    cfg->ipc_rx_buf[0] = '\0';

    cfg->ipc_path = strdup("/tmp/mqttd.sock");
    cfg->ipc_listen_fd = -1;
    cfg->ipc_client_fd = -1;
    cfg->ipc_client_generation = 0;

    cfg->ipc_tx_head = NULL;
    cfg->ipc_tx_tail = NULL;
    cfg->ipc_tx_queue_bytes = 0;
    cfg->ipc_tx_queue_count = 0;
    cfg->ipc_tx_next_sequence = 0;
    cfg->ipc_tx_thread_started = 0;
    cfg->ipc_tx_stop = 0;

    cfg->retry_apply_enabled = 0;
    cfg->retry_apply_interval_sec = 10;   //  10 秒重试apply
    cfg->retry_apply_next_ts = 0;

    cfg->mosq = NULL;
    pthread_mutex_init(&cfg->mosq_lock, NULL);
    
    pthread_mutex_init(&cfg->ipc_lock, NULL);
    pthread_mutex_init(&cfg->ipc_tx_lock, NULL);
    pthread_cond_init(&cfg->ipc_tx_cond, NULL);
    pthread_mutex_init(&cfg->state_lock, NULL);

    cfg->mqtt_connected = 0;
    cfg->last_conn_rc = -1;
}

/*
 * add_topic：把一个订阅 topic 加入 cfg->topics 列表
 * - 支持多次 -t 参数：每次就调用一次 add_topic
 * - realloc 扩容 topics 数组；strdup 拷贝字符串
 */
static void add_topic(mqttd_cfg_t *cfg, const char *topic)
{
    // 扩容：把 topics 数组从 topic_count 个扩大到 topic_count+1 个
    cfg->topics = (char **)realloc(cfg->topics, sizeof(char *) * (size_t)(cfg->topic_count + 1));
    if (!cfg->topics) {
        fprintf(stderr, "Out of memory while adding topic\n");
        exit(2);
    }
    // 拷贝 topic 字符串，保存到数组末尾
    cfg->topics[cfg->topic_count] = strdup(topic);
    if (!cfg->topics[cfg->topic_count]) {
        fprintf(stderr, "Out of memory while adding topic\n");
        exit(2);
    }
    cfg->topic_count++;
}

/*
 * free_cfg：释放配置结构体里动态申请的内存
 * 注意：cfg 本身是在栈上定义的，不需要 free(cfg)，只释放里面的指针
 */
static void free_cfg(mqttd_cfg_t *cfg)
{
    if (!cfg) return;
    ipc_tx_item_t *item = cfg->ipc_tx_head;
    while (item) {
        ipc_tx_item_t *next = item->next;
        free(item->line);
        free(item->topic);
        free(item->payload);
        free(item);
        item = next;
    }
    cfg->ipc_tx_head = NULL;
    cfg->ipc_tx_tail = NULL;
    // 释放 topics 数组里的每个字符串
    for (int i = 0; i < cfg->topic_count; i++) {
        free(cfg->topics[i]);
    }
    // 释放 topics 指针数组本体
    free(cfg->topics);

    // 释放各类 strdup 得到的字符串
    free(cfg->host);
    free(cfg->client_id);
    free(cfg->username);
    free(cfg->password);

    free(cfg->pub_topic);
    free(cfg->pub_msg);

    free(cfg->cafile);
    free(cfg->certfile);
    free(cfg->keyfile);

    free(cfg->ipc_path);
    free(cfg->ipc_rx_buf);

    pthread_mutex_destroy(&cfg->mosq_lock);
    pthread_mutex_destroy(&cfg->ipc_lock);
    pthread_mutex_destroy(&cfg->ipc_tx_lock);
    pthread_cond_destroy(&cfg->ipc_tx_cond);
    pthread_mutex_destroy(&cfg->state_lock);
}


/* ========== IPC：fd 非阻塞设置 ========== */
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ========== IPC：初始化监听 socket ========== */
static int ipc_server_init(mqttd_cfg_t *cfg)
{
    if (!cfg->ipc_path) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[ipc] socket");
        return -1;
    }

    // 允许快速重启：先 unlink 旧文件
    unlink(cfg->ipc_path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    // 注意：sun_path 有长度限制（一般 108 字节）
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", cfg->ipc_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[ipc] bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        perror("[ipc] listen");
        close(fd);
        return -1;
    }    
    // 让普通用户也能连（Qt 进程可能不是 root）
    chmod(cfg->ipc_path, 0666);

    if (set_nonblocking(fd) < 0) {
        perror("[ipc] nonblock listen");
        // 不致命，但建议成功
    }

    cfg->ipc_listen_fd = fd;

    if (cfg->verbose) {
        fprintf(stderr, "[ipc] listening on %s\n", cfg->ipc_path);
    }
    return 0;
}


/* ========== IPC：发送一行 JSON 给 Qt（线程安全） ========== */
static long long monotonic_millis(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

static void ipc_tx_item_free(ipc_tx_item_t *item)
{
    if (!item) return;
    free(item->line);
    free(item->topic);
    free(item->payload);
    free(item);
}

static const char *ipc_tx_kind_name(ipc_tx_item_kind_t kind)
{
    return kind == IPC_TX_ITEM_MQTT_MESSAGE ? "mqtt" : "line";
}

static int ipc_tx_is_stopping(mqttd_cfg_t *cfg)
{
    int stopping;
    pthread_mutex_lock(&cfg->ipc_tx_lock);
    stopping = cfg->ipc_tx_stop;
    pthread_mutex_unlock(&cfg->ipc_tx_lock);
    return stopping;
}

static void ipc_tx_wake(mqttd_cfg_t *cfg)
{
    pthread_mutex_lock(&cfg->ipc_tx_lock);
    pthread_cond_broadcast(&cfg->ipc_tx_cond);
    pthread_mutex_unlock(&cfg->ipc_tx_lock);
}

static int ipc_tx_enqueue_item(mqttd_cfg_t *cfg, ipc_tx_item_t *item)
{
    if (!cfg || !item) return -1;

    pthread_mutex_lock(&cfg->ipc_tx_lock);
    if (cfg->ipc_tx_stop ||
        cfg->ipc_tx_queue_count >= IPC_TX_MAX_QUEUE_ITEMS ||
        item->accounted_bytes > IPC_TX_MAX_QUEUE_BYTES ||
        cfg->ipc_tx_queue_bytes > IPC_TX_MAX_QUEUE_BYTES - item->accounted_bytes) {
        size_t count = cfg->ipc_tx_queue_count;
        size_t bytes = cfg->ipc_tx_queue_bytes;
        int stopping = cfg->ipc_tx_stop;
        pthread_mutex_unlock(&cfg->ipc_tx_lock);
        fprintf(stderr,
                "[ipc-q] enqueue rejected kind=%s bytes=%zu count=%zu queuedBytes=%zu stopping=%d\n",
                ipc_tx_kind_name(item->kind), item->accounted_bytes,
                count, bytes, stopping);
        ipc_tx_item_free(item);
        return -1;
    }

    item->sequence = ++cfg->ipc_tx_next_sequence;
    item->next = NULL;
    if (cfg->ipc_tx_tail) {
        cfg->ipc_tx_tail->next = item;
    } else {
        cfg->ipc_tx_head = item;
    }
    cfg->ipc_tx_tail = item;
    cfg->ipc_tx_queue_count++;
    cfg->ipc_tx_queue_bytes += item->accounted_bytes;

    pthread_cond_signal(&cfg->ipc_tx_cond);
    pthread_mutex_unlock(&cfg->ipc_tx_lock);
    return 0;
}

static int ipc_tx_enqueue_line(mqttd_cfg_t *cfg, const char *line)
{
    if (!cfg || !line) return -1;

    size_t line_len = strlen(line);
    if (line_len + 1U > IPC_TX_MAX_FRAME_BYTES) {
        fprintf(stderr, "[ipc-q] line too large bytes=%zu max=%u\n",
                line_len + 1U, IPC_TX_MAX_FRAME_BYTES);
        return -1;
    }

    ipc_tx_item_t *item = (ipc_tx_item_t *)calloc(1, sizeof(*item));
    if (!item) return -1;

    item->line = (char *)malloc(line_len + 1U);
    if (!item->line) {
        ipc_tx_item_free(item);
        return -1;
    }
    memcpy(item->line, line, line_len + 1U);
    item->kind = IPC_TX_ITEM_LINE;
    item->line_len = line_len;
    item->accounted_bytes = line_len + 1U;
    return ipc_tx_enqueue_item(cfg, item);
}

static int ipc_tx_enqueue_mqtt_message(mqttd_cfg_t *cfg,
                                       const struct mosquitto_message *msg)
{
    if (!cfg || !msg || !msg->topic || msg->payloadlen < 0) return -1;

    size_t topic_len = strlen(msg->topic);
    size_t payload_len = (size_t)msg->payloadlen;
    if (payload_len > 0 && !msg->payload) return -1;
    if (payload_len > IPC_TX_MAX_FRAME_BYTES ||
        topic_len > IPC_TX_MAX_FRAME_BYTES - 512U ||
        payload_len > IPC_TX_MAX_FRAME_BYTES - topic_len - 512U) {
        fprintf(stderr,
                "[ipc-q] MQTT message too large topic=%s payloadBytes=%zu maxFrame=%u\n",
                msg->topic, payload_len, IPC_TX_MAX_FRAME_BYTES);
        return -1;
    }

    ipc_tx_item_t *item = (ipc_tx_item_t *)calloc(1, sizeof(*item));
    if (!item) return -1;

    item->topic = strdup(msg->topic);
    item->payload = (char *)malloc(payload_len + 1U);
    if (!item->topic || !item->payload) {
        ipc_tx_item_free(item);
        return -1;
    }

    if (payload_len > 0 && msg->payload) {
        memcpy(item->payload, msg->payload, payload_len);
    }
    item->payload[payload_len] = '\0';
    item->kind = IPC_TX_ITEM_MQTT_MESSAGE;
    item->payload_len = payload_len;
    item->qos = msg->qos;
    item->retain = msg->retain;
    item->accounted_bytes = payload_len + topic_len + 512U;
    return ipc_tx_enqueue_item(cfg, item);
}

/*
 * Send the complete frame through the non-blocking Unix socket. Large face
 * image messages may be written only partially, so retry the unsent tail.
 */
static int ipc_write_all(mqttd_cfg_t *cfg, int fd,
                         const char *data, size_t len, size_t *sent_out,
                         unsigned long sequence)
{
    size_t sent = 0;
    const long long started_ms = monotonic_millis();
    long long last_block_log_ms = 0;

    while (sent < len) {
        if (ipc_tx_is_stopping(cfg)) {
            errno = ECANCELED;
            break;
        }
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
#else
        ssize_t n = write(fd, data + sent, len - sent);
#endif
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n == 0) {
            errno = EPIPE;
            break;
        }
        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            long long elapsed_ms = monotonic_millis() - started_ms;
            if (last_block_log_ms == 0 || elapsed_ms - last_block_log_ms >= 1000) {
                fprintf(stderr,
                        "[ipc-q] backpressure seq=%lu sent=%zu remaining=%zu elapsedMs=%lld\n",
                        sequence, sent, len - sent, elapsed_ms);
                last_block_log_ms = elapsed_ms;
            }
            int wait_ms = IPC_TX_TIMEOUT_MS - (int)elapsed_ms;
            if (wait_ms <= 0) {
                errno = ETIMEDOUT;
                break;
            }

            struct pollfd pfd;
            memset(&pfd, 0, sizeof(pfd));
            pfd.fd = fd;
            pfd.events = POLLOUT;

            int pr = poll(&pfd, 1, wait_ms);
            if (pr > 0) {
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    errno = EPIPE;
                    break;
                }
                continue;
            }
            if (pr == 0) {
                errno = ETIMEDOUT;
                break;
            }
            if (errno == EINTR) {
                continue;
            }
        }
        break;
    }

    if (sent_out) {
        *sent_out = sent;
    }
    return sent == len ? 0 : -1;
}

static int ipc_tx_build_frame(ipc_tx_item_t *item, char **frame_out, size_t *frame_len_out)
{
    if (!item || !frame_out || !frame_len_out) return -1;
    *frame_out = NULL;
    *frame_len_out = 0;

    char *text = NULL;
    size_t text_len = 0;
    if (item->kind == IPC_TX_ITEM_LINE) {
        text = item->line;
        text_len = item->line_len;
    } else {
        cJSON *root = cJSON_CreateObject();
        if (!root) return -1;

        cJSON_AddStringToObject(root, "type", "msg");
        cJSON_AddStringToObject(root, "topic", item->topic ? item->topic : "");
        cJSON_AddNumberToObject(root, "qos", item->qos);
        cJSON_AddNumberToObject(root, "retain", item->retain);
        cJSON_AddStringToObject(root, "payload", item->payload ? item->payload : "");
        text = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!text) return -1;
        text_len = strlen(text);
    }

    if (text_len + 1U > IPC_TX_MAX_FRAME_BYTES) {
        fprintf(stderr,
                "[ipc-q] built frame too large seq=%lu bytes=%zu max=%u\n",
                item->sequence, text_len + 1U, IPC_TX_MAX_FRAME_BYTES);
        if (item->kind == IPC_TX_ITEM_MQTT_MESSAGE) free(text);
        return -1;
    }

    char *frame = (char *)malloc(text_len + 1U);
    if (!frame) {
        if (item->kind == IPC_TX_ITEM_MQTT_MESSAGE) free(text);
        return -1;
    }
    memcpy(frame, text, text_len);
    frame[text_len] = '\n';
    if (item->kind == IPC_TX_ITEM_MQTT_MESSAGE) free(text);

    *frame_out = frame;
    *frame_len_out = text_len + 1U;
    return 0;
}

static int ipc_dup_client(mqttd_cfg_t *cfg, unsigned long *generation_out)
{
    int duplicate_fd = -1;
    pthread_mutex_lock(&cfg->ipc_lock);
    if (cfg->ipc_client_fd >= 0) {
        duplicate_fd = dup(cfg->ipc_client_fd);
        if (duplicate_fd >= 0 && generation_out) {
            *generation_out = cfg->ipc_client_generation;
        }
    }
    pthread_mutex_unlock(&cfg->ipc_lock);
    return duplicate_fd;
}

/** @brief 仅关闭匹配代次的 IPC，并在实际关闭时禁用通话媒体。
 * @param cfg 非空运行配置，连接代次由 ipc_lock 保护。
 * @param generation 调用方复制 socket 时取得的代次；过期代次不影响新连接。
 */
static void ipc_close_client_if_generation(mqttd_cfg_t *cfg,
                                           unsigned long generation)
{
    int closed=0;
    pthread_mutex_lock(&cfg->ipc_lock);
    if (cfg->ipc_client_fd >= 0 && cfg->ipc_client_generation == generation) {
        closed=1;
        shutdown(cfg->ipc_client_fd, SHUT_RDWR);
        close(cfg->ipc_client_fd);
        cfg->ipc_client_fd = -1;
        cfg->ipc_client_generation++;
    }
    pthread_mutex_unlock(&cfg->ipc_lock);
    if (closed) {
        broadcast_reset(cfg);
        call_reset(cfg);
    }
}

static void ipc_tx_wait_for_client(mqttd_cfg_t *cfg)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;

    pthread_mutex_lock(&cfg->ipc_tx_lock);
    if (!cfg->ipc_tx_stop) {
        pthread_cond_timedwait(&cfg->ipc_tx_cond, &cfg->ipc_tx_lock, &deadline);
    }
    pthread_mutex_unlock(&cfg->ipc_tx_lock);
}

/** @brief 禁止当前媒体并清空环；不取消已提交 MQTT 的唯一一帧。
 * @param cfg 非空运行配置；本函数自行获取 ipc_tx_lock。
 * @note 日志在锁外输出；在途 mid 保留到回调完成、新 CONNACK 或旧客户端销毁。
 */
static void call_reset(mqttd_cfg_t *cfg)
{
    char epoch[48]; unsigned long rx_drops,tx_drops; int was_enabled;
    pthread_mutex_lock(&cfg->ipc_tx_lock);
    was_enabled=cfg->call_enabled;
    memcpy(epoch,cfg->call_epoch,sizeof(epoch)); rx_drops=cfg->call_rx_drops; tx_drops=cfg->call_tx_drops;
    cfg->call_enabled=0;
    cfg->call_rx_head=cfg->call_rx_count=0;
    cfg->call_rx_pending=0;
    cfg->call_up_head=cfg->call_up_count=0;
    pthread_mutex_unlock(&cfg->ipc_tx_lock);
    if (was_enabled) fprintf(stderr,"[call-mqtt] epoch=%s rxDrop=%lu txDrop=%lu\n",epoch,rx_drops,tx_drops);
}

/** @brief 连接断开或重建时关闭直播收帧门并丢弃旧帧。
 * @param cfg 非空运行配置；本函数自行获取 ipc_tx_lock。
 */
static void broadcast_reset(mqttd_cfg_t *cfg)
{
    pthread_mutex_lock(&cfg->ipc_tx_lock);
    cfg->broadcast_head=cfg->broadcast_count=0;
    cfg->broadcast_expected=0;
    pthread_mutex_unlock(&cfg->ipc_tx_lock);
}

/** @brief 新 CONNACK 成功后清除已被 libmosquitto 重连清理的 QoS0 在途信用。
 * @param cfg 非空运行配置；由网络连接回调调用，内部获取 ipc_tx_lock。
 * @note libmosquitto reconnect 在发送新 CONNECT 前清理旧 packet 队列，QoS0 不在重发消息表中；
 *       此处只恢复本地信用，不改变原业务 QoS1/2 重发、连接或订阅管理。
 */
static void call_connected(mqttd_cfg_t *cfg)
{
    pthread_mutex_lock(&cfg->ipc_tx_lock);
    cfg->call_tx_mid=0;
    pthread_mutex_unlock(&cfg->ipc_tx_lock);
}

/** @brief 撤销未完整写入 IPC 的下行 token，不影响已在途的其他帧信用。
 * @param cfg 非空配置；内部只获取短队列锁。
 * @param epoch 写入失败帧的媒体代次，旧代次不影响新信用。
 * @param token 需要撤销的已预留下行序号。
 */
static void call_release_rx(mqttd_cfg_t *cfg,const char *epoch,unsigned long token)
{
    pthread_mutex_lock(&cfg->ipc_tx_lock);
    if (strcmp(epoch,cfg->call_epoch)==0) {
        for (int i=0;i<cfg->call_rx_pending;++i) if (cfg->call_rx_outstanding[i]==token) {
            memmove(&cfg->call_rx_outstanding[i],&cfg->call_rx_outstanding[i+1],
                (size_t)(cfg->call_rx_pending-i-1)*sizeof(unsigned long));
            --cfg->call_rx_pending;break;
        }
        ++cfg->call_rx_drops;
    }
    pthread_mutex_unlock(&cfg->ipc_tx_lock);
}

/** @brief 同一 IPC 写线程发送一帧音频；窗口满暂停，过期或背压时丢弃，不重放。
 * @param cfg 非空运行配置，接收环和信用由 ipc_tx_lock 保护。
 * @note 不持锁写 socket；部分 JSON 写入后关闭当前连接，避免破坏后续业务帧。
 */
static void call_send_rx(mqttd_cfg_t *cfg)
{
    call_rx_frame_t packet;
    char epoch[48];
    unsigned long token=0;
    int have=0;
    pthread_mutex_lock(&cfg->ipc_tx_lock);
    while (cfg->call_enabled && cfg->call_rx_pending<5 && cfg->call_rx_count) {
        packet=cfg->call_rx[cfg->call_rx_head];
        cfg->call_rx_head=(cfg->call_rx_head+1)%10; --cfg->call_rx_count;
        if (monotonic_millis()-packet.arrival_ms>200) { ++cfg->call_rx_drops; continue; }
        memcpy(epoch,cfg->call_epoch,sizeof(epoch));
        token=++cfg->call_rx_token; cfg->call_rx_outstanding[cfg->call_rx_pending++]=token; have=1; break;
    }
    pthread_mutex_unlock(&cfg->ipc_tx_lock);
    if (!have) return;
    char hex[1301];
    const char *digits="0123456789abcdef";
    for (int i=0;i<650;++i) { hex[i*2]=digits[packet.pcm[i]>>4]; hex[i*2+1]=digits[packet.pcm[i]&15]; }
    hex[1300]=0;
    cJSON *root=cJSON_CreateObject();
    if (!root) { call_release_rx(cfg,epoch,token);return; }
    cJSON_AddStringToObject(root,"type","audio");
    cJSON_AddStringToObject(root,"epoch",epoch);
    cJSON_AddStringToObject(root,"topic",packet.topic);
    cJSON_AddStringToObject(root,"hex",hex);
    cJSON_AddNumberToObject(root,"retain",0);
    cJSON_AddNumberToObject(root,"token",(double)token);
    cJSON_AddNumberToObject(root,"arrivalMs",(double)packet.arrival_ms);
    char *text=cJSON_PrintUnformatted(root); cJSON_Delete(root);
    if (!text) { call_release_rx(cfg,epoch,token);return; }
    size_t len=strlen(text); char *line=malloc(len+2);
    if (!line) { free(text);call_release_rx(cfg,epoch,token);return; }
    memcpy(line,text,len); line[len++]='\n'; free(text);
    unsigned long generation=0; int fd=ipc_dup_client(cfg,&generation);
    if (fd>=0) {
        // 小帧只尝试一次；部分写入必须关闭连接，不能拼接破损 JSON 后继续。
        ssize_t n=send(fd,line,len,MSG_NOSIGNAL);
        const int send_error=errno;close(fd);
        if (n!=(ssize_t)len) {
            if (n>=0 || (send_error!=EAGAIN && send_error!=EWOULDBLOCK)) ipc_close_client_if_generation(cfg,generation);
            call_release_rx(cfg,epoch,token);
        }
    } else call_reset(cfg);
    free(line);
}

/** @brief 在普通消息和通话媒体均未占用时提交一条最新直播帧。
 * @param cfg 非空运行配置；锁内取值、锁外一次非阻塞写入。
 * @note 过期或背压时丢弃。发生部分写入时退役连接以维持JSON行边界。
 */
static void broadcast_send_rx(mqttd_cfg_t *cfg)
{
    unsigned char packet[1290];
    char topic[160], epoch[37];
    size_t length=0;
    long long arrival=0;
    pthread_mutex_lock(&cfg->ipc_tx_lock);
    const long long now=monotonic_millis();
    while (!cfg->call_enabled && cfg->broadcast_count) {
        const unsigned int head=cfg->broadcast_head;
        cfg->broadcast_head=(head+1)%BROADCAST_RX_CAPACITY;
        --cfg->broadcast_count;
        arrival=cfg->broadcast_arrival_ms[head];
        if (now-arrival>120) {
            ++cfg->broadcast_queue_drops;
            continue;
        }
        length=cfg->broadcast_expected;
        ++cfg->broadcast_ipc_picked;
        memcpy(packet,cfg->broadcast_packets[head],length);
        memcpy(topic,cfg->broadcast_topic,sizeof(topic));
        memcpy(epoch,cfg->broadcast_epoch,sizeof(epoch));
        break;
    }
    pthread_mutex_unlock(&cfg->ipc_tx_lock);
    if (!length) return;
    char hex[2581];
    const char digits[]="0123456789abcdef";
    for (size_t i=0;i<length;++i) {
        hex[2*i]=digits[packet[i]>>4];
        hex[2*i+1]=digits[packet[i]&15];
    }
    hex[2*length]='\0';
    char line[3000];
    const int len=snprintf(line,sizeof(line),
        "{\"type\":\"broadcast_audio\",\"topic\":\"%s\",\"epoch\":\"%s\",\"hex\":\"%s\",\"arrivalMs\":%lld}\n",
        topic,epoch,hex,arrival);
    if (len<=0 || (size_t)len>=sizeof(line)) return;
    unsigned long generation=0;
    const int fd=ipc_dup_client(cfg,&generation);
    if (fd<0) return;
    const ssize_t sent=send(fd,line,(size_t)len,MSG_NOSIGNAL);
    const int error=errno;
    close(fd);
    if (sent!=len && (sent>=0 || (error!=EAGAIN && error!=EWOULDBLOCK)))
        ipc_close_client_if_generation(cfg,generation);
}

/** @brief 唯一 IPC 写线程，保留原业务发送队列，并在空隙发送有信用的新鲜音频。
 * @param arg 非空 mqttd_cfg_t；停止标志和队列由 ipc_tx_lock 保护。
 * @return 停止后返回 NULL；主线程 join 后才可释放配置。
 */
static void *ipc_tx_worker(void *arg)
{
    mqttd_cfg_t *cfg = (mqttd_cfg_t *)arg;

    for (;;) {
        pthread_mutex_lock(&cfg->ipc_tx_lock);
        while (!cfg->ipc_tx_stop && cfg->ipc_tx_head == NULL &&
               !(cfg->call_enabled && cfg->call_rx_count && cfg->call_rx_pending<5) &&
               !(!cfg->call_enabled && cfg->broadcast_count)) {
            pthread_cond_wait(&cfg->ipc_tx_cond, &cfg->ipc_tx_lock);
        }
        if (cfg->ipc_tx_stop) {
            pthread_mutex_unlock(&cfg->ipc_tx_lock);
            break;
        }

        if (!cfg->ipc_tx_head) {
            const int call=cfg->call_enabled;
            pthread_mutex_unlock(&cfg->ipc_tx_lock);
            if (call) call_send_rx(cfg);
            else broadcast_send_rx(cfg);
            continue;
        }
        ipc_tx_item_t *item = cfg->ipc_tx_head;
        cfg->ipc_tx_head = item->next;
        if (!cfg->ipc_tx_head) cfg->ipc_tx_tail = NULL;
        cfg->ipc_tx_queue_count--;
        cfg->ipc_tx_queue_bytes -= item->accounted_bytes;
        item->next = NULL;
        pthread_mutex_unlock(&cfg->ipc_tx_lock);

        char *frame = NULL;
        size_t frame_len = 0;
        if (ipc_tx_build_frame(item, &frame, &frame_len) != 0) {
            fprintf(stderr, "[ipc-q] build failed seq=%lu kind=%s\n",
                    item->sequence, ipc_tx_kind_name(item->kind));
            ipc_tx_item_free(item);
            continue;
        }

        int delivered = 0;
        while (!ipc_tx_is_stopping(cfg) && item->attempts < IPC_TX_MAX_RETRIES) {
            unsigned long generation = 0;
            int cfd = ipc_dup_client(cfg, &generation);
            if (cfd < 0) {
                ipc_tx_wait_for_client(cfg);
                continue;
            }

            item->attempts++;
            size_t sent = 0;
            int result = ipc_write_all(cfg, cfd, frame, frame_len, &sent,
                                       item->sequence);
            int saved_errno = errno;
            close(cfd);

            if (result == 0) {
                delivered = 1;
                break;
            }

            fprintf(stderr,
                    "[ipc-q] send failed seq=%lu attempt=%d expected=%zu sent=%zu error=%d (%s)\n",
                    item->sequence, item->attempts, frame_len, sent,
                    saved_errno, strerror(saved_errno));
            ipc_close_client_if_generation(cfg, generation);
        }

        if (!delivered && !ipc_tx_is_stopping(cfg)) {
            fprintf(stderr,
                    "[ipc-q] drop seq=%lu kind=%s after %d attempts bytes=%zu\n",
                    item->sequence, ipc_tx_kind_name(item->kind),
                    item->attempts, frame_len);
        }
        free(frame);
        ipc_tx_item_free(item);
    }
    return NULL;
}

static int ipc_tx_start(mqttd_cfg_t *cfg)
{
    cfg->ipc_tx_stop = 0;
    int rc = pthread_create(&cfg->ipc_tx_thread, NULL, ipc_tx_worker, cfg);
    if (rc != 0) {
        fprintf(stderr, "[ipc-q] worker start failed error=%d (%s)\n", rc, strerror(rc));
        return -1;
    }
    cfg->ipc_tx_thread_started = 1;
    return 0;
}

static void ipc_tx_stop(mqttd_cfg_t *cfg)
{
    pthread_mutex_lock(&cfg->ipc_tx_lock);
    cfg->ipc_tx_stop = 1;
    pthread_cond_broadcast(&cfg->ipc_tx_cond);
    pthread_mutex_unlock(&cfg->ipc_tx_lock);

    pthread_mutex_lock(&cfg->ipc_lock);
    if (cfg->ipc_client_fd >= 0) {
        shutdown(cfg->ipc_client_fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&cfg->ipc_lock);

    if (cfg->ipc_tx_thread_started) {
        pthread_join(cfg->ipc_tx_thread, NULL);
        cfg->ipc_tx_thread_started = 0;
    }

    pthread_mutex_lock(&cfg->ipc_tx_lock);
    ipc_tx_item_t *item = cfg->ipc_tx_head;
    cfg->ipc_tx_head = NULL;
    cfg->ipc_tx_tail = NULL;
    cfg->ipc_tx_queue_bytes = 0;
    cfg->ipc_tx_queue_count = 0;
    pthread_mutex_unlock(&cfg->ipc_tx_lock);

    while (item) {
        ipc_tx_item_t *next = item->next;
        ipc_tx_item_free(item);
        item = next;
    }
}

static void ipc_send_line(mqttd_cfg_t *cfg, const char *line)
{
    if (ipc_tx_enqueue_line(cfg, line) != 0) {
        fprintf(stderr, "[ipc-q] failed to enqueue line\n");
    }
}

/* ========== IPC：发送状态快照（Qt连接时用） ========== */
static void ipc_send_status(mqttd_cfg_t *cfg, const char *type)
{
    // type 可传 "hello" / "status" / "test_result"
    if (!type) type = "status";

    pthread_mutex_lock(&cfg->state_lock);
    int connected = cfg->mqtt_connected;
    int last_rc = cfg->last_conn_rc;
    pthread_mutex_unlock(&cfg->state_lock);

    // topics 拼接字符串
    char topic_buf[512];
    topic_buf[0] = '\0';
    for (int i = 0; i < cfg->topic_count; i++) {
        if (i > 0) strncat(topic_buf, ",", sizeof(topic_buf) - strlen(topic_buf) - 1);
        strncat(topic_buf, cfg->topics[i], sizeof(topic_buf) - strlen(topic_buf) - 1);
    }

    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"mqtt\":{\"host\":\"%s\",\"port\":%d,\"client_id\":\"%s\",\"qos\":%d,"
             "\"tls\":%s},\"sub_topics\":\"%s\",\"connected\":%s,\"last_rc\":%d}",
             type,
             cfg->host ? cfg->host : "",
             cfg->port,
             cfg->client_id ? cfg->client_id : "",
             cfg->qos,
             cfg->tls_enable ? "true" : "false",
             topic_buf,
             connected ? "true" : "false",
             last_rc);

    ipc_send_line(cfg, buf);
}

/* ========== IPC：接入新的 Qt 连接（只保留 1 个） ========== */
static void ipc_accept_if_needed(mqttd_cfg_t *cfg)
{
    if (cfg->ipc_listen_fd < 0) return;

    struct sockaddr_un caddr;
    socklen_t clen = sizeof(caddr);
    int cfd = accept(cfg->ipc_listen_fd, (struct sockaddr *)&caddr, &clen);
    if (cfd < 0) {
        // 非阻塞 accept：没连接时会返回 EAGAIN/EWOULDBLOCK
        return;
    }

    set_nonblocking(cfd);

    int requested_send_buffer = IPC_TX_SOCKET_BUFFER;
    if (setsockopt(cfd, SOL_SOCKET, SO_SNDBUF,
                   &requested_send_buffer, sizeof(requested_send_buffer)) != 0) {
        fprintf(stderr, "[ipc] set SO_SNDBUF failed error=%d (%s)\n",
                errno, strerror(errno));
    }
    int actual_send_buffer = 0;
    socklen_t actual_send_buffer_len = sizeof(actual_send_buffer);
    if (getsockopt(cfd, SOL_SOCKET, SO_SNDBUF,
                   &actual_send_buffer, &actual_send_buffer_len) != 0) {
        actual_send_buffer = -1;
    }

    pthread_mutex_lock(&cfg->ipc_lock);
    if (cfg->ipc_client_fd >= 0) {
        // 已经有一个 Qt 客户端，替换掉旧的
        shutdown(cfg->ipc_client_fd, SHUT_RDWR);
        close(cfg->ipc_client_fd);
    }
    cfg->ipc_client_fd = cfd;
    cfg->ipc_client_generation++;
    pthread_mutex_unlock(&cfg->ipc_lock);

    cfg->ipc_rx_len = 0;
    if (cfg->ipc_rx_buf && cfg->ipc_rx_cap > 0) cfg->ipc_rx_buf[0] = '\0';

    if (cfg->verbose) {
        fprintf(stderr, "[ipc] Qt client connected SO_SNDBUF=%d\n", actual_send_buffer);
    }

    // Qt 连接上来先推送一份 hello（包含 host/port/topic/connected 等）
    ipc_send_status(cfg, "hello");
    ipc_tx_wake(cfg);
}

/* ========== JSON 命令解析（cmd":"test/status） ========== */
/* 注意：这里只做非常简单的字符串匹配，要求 Qt 发来的格式规范：{"cmd":"test"} */
static const char *json_find_cmd(const char *line)
{
    // 找 "cmd":"xxx"
    const char *p = strstr(line, "\"cmd\"");
    if (!p) return NULL;
    p = strchr(p, ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '\"') return NULL;
    p++;
    return p; // 指向 cmd 内容开始
}

static int json_get_string(const char *line, const char *key, char *out, size_t outlen)
{
    // 找 "key":"xxx"
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(line, pat);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    const char *q = strchr(p, '"');
    if (!q) return -1;
    size_t n = (size_t)(q - p);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

static int json_get_int(const char *line, const char *key, int *out)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(line, pat);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    *out = atoi(p);
    return 0;
}

static int json_get_bool(const char *line, const char *key, int *out)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(line, pat);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0) { *out = 1; return 0; }
    if (strncmp(p, "false", 5) == 0) { *out = 0; return 0; }
    return -1;
}


static void topics_clear(mqttd_cfg_t *cfg)
{
    for (int i = 0; i < cfg->topic_count; i++) free(cfg->topics[i]);
    free(cfg->topics);
    cfg->topics = NULL;
    cfg->topic_count = 0;
}

static void parse_subs_array(mqttd_cfg_t *cfg, const char *line)
{
    // 找到 "subs":[ ... ]
    const char *p = strstr(line, "\"subs\"");
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    p++;

    topics_clear(cfg);

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
        if (*p == ']') break;
        if (*p != '"') { p++; continue; }
        p++;
        const char *q = strchr(p, '"');
        if (!q) break;

        char t[256];
        size_t n = (size_t)(q - p);
        if (n >= sizeof(t)) n = sizeof(t) - 1;
        memcpy(t, p, n);
        t[n] = '\0';
        add_topic(cfg, t);

        p = q + 1;
    }
}

static void ipc_send_ack(mqttd_cfg_t *cfg, const char *cmd, int ok, const char *msg)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"ack\",\"cmd\":\"%s\",\"ok\":%s,\"msg\":\"%s\"}",
             cmd ? cmd : "",
             ok ? "true" : "false",
             msg ? msg : "");
    ipc_send_line(cfg, buf);
}

static void handle_set_config(mqttd_cfg_t *cfg, const char *line)
{
    // 注意：Qt 发的是 {"cmd":"set_config","mqtt":{...},"subs":[...]}
    // 这里用简单方式直接在整行里找键值

    char host[128], client_id[128], user[128], pass[128];
    int port, keepalive, qos, tls;

    if (json_get_string(line, "host", host, sizeof(host)) == 0) {
        free(cfg->host);
        cfg->host = strdup(host);
    }
    if (json_get_int(line, "port", &port) == 0 && port > 0 && port <= 65535) {
        cfg->port = port;
    }
    if (json_get_string(line, "client_id", client_id, sizeof(client_id)) == 0) {
        free(cfg->client_id);
        cfg->client_id = strdup(client_id);
    }
    if (json_get_int(line, "keepalive", &keepalive) == 0 && keepalive > 0) {
        cfg->keepalive = keepalive;
    }
    if (json_get_int(line, "qos", &qos) == 0) {
        if (qos < 0) qos = 0;
        if (qos > 2) qos = 2;
        cfg->qos = qos;
    }
    if (json_get_bool(line, "tls", &tls) == 0) {
        cfg->tls_enable = (tls ? true : false);
    }

    // username/password：允许空（清空认证）
    if (json_get_string(line, "username", user, sizeof(user)) == 0) {
        free(cfg->username);
        cfg->username = strdup(user);
    }
    if (json_get_string(line, "password", pass, sizeof(pass)) == 0) {
        free(cfg->password);
        cfg->password = strdup(pass);
    }

    // subs 数组
    parse_subs_array(cfg, line);

    ipc_send_ack(cfg, "set_config", 1, "saved");
    ipc_send_status(cfg, "status");
}

static void handle_apply(mqttd_cfg_t *cfg)
{
    int r = mqtt_apply_reconnect(cfg);
    if (r == MOSQ_ERR_SUCCESS) {
        // on_connect 成功后再取消
        //cancel_apply_retry(cfg);
        ipc_send_ack(cfg, "apply", 1, "reconnecting");
    } else {
        char m[128];
        snprintf(m, sizeof(m), "apply failed: %s", mosquitto_strerror(r));
        ipc_send_ack(cfg, "apply", 0, m);

        // 失败后启动周期重试
        schedule_apply_retry(cfg, 10);
    }
}


static void dump_hex(const char *tag, const unsigned char *buf, int len)
{
    fprintf(stderr, "[hex] %s len=%d: ", tag, len);
    for (int i = 0; i < len; ++i) {
        fprintf(stderr, "%02x", buf[i]);
    }
    fprintf(stderr, "\n");
}

/** @brief 发回当前代次的控制信用；不代表 QoS0 已到达平台。
 * @param cfg 非空配置，复用原可靠 IPC 队列。
 * @param type call_ready 或 call_sent。
 * @param epoch 需要回显的本地媒体代次 UUID。
 * @param ok 非零为就绪/完成，0 要求 Qt 停止通话。
 */
static void call_ack(mqttd_cfg_t *cfg,const char *type,const char *epoch,int ok)
{
    cJSON *obj=cJSON_CreateObject();
    if (!obj) return;
    cJSON_AddStringToObject(obj,"type",type); cJSON_AddStringToObject(obj,"epoch",epoch);
    cJSON_AddBoolToObject(obj,"ok",ok);
    if (strcmp(type,"call_ready")==0) cJSON_AddNumberToObject(obj,"window",5);
    char *text=cJSON_PrintUnformatted(obj); cJSON_Delete(obj);
    if (text) { ipc_send_line(cfg,text); free(text); }
}

/** @brief 网络线程仅记录发布完成序号，不调用 IPC 写入或音频设备。
 * @param mosq 原客户端，本回调不访问其状态。
 * @param userdata 非空 mqttd_cfg_t，由主线程在网络线程 join 后释放。
 * @param mid libmosquitto 分配的 1..65535 消息号，QoS0 也分配。
 */
static void on_call_publish(struct mosquitto *mosq,void *userdata,int mid)
{
    (void)mosq; mqttd_cfg_t *cfg=userdata;
    if (mid<=0 || mid>65535) return;
    pthread_mutex_lock(&cfg->ipc_tx_lock);
    cfg->call_completed[mid]=++cfg->call_callback_serial;
    pthread_mutex_unlock(&cfg->ipc_tx_lock);
}

/** @brief 主线程确认唯一 QoS0 帧已离开 libmosquitto，或超时后禁止继续提交。
 * @param cfg 非空运行配置；只由 IPC 主线程调用。
 * @note 500ms 是停发兜底；TCP/Broker 内已经发送的消息无法由 V1 协议撤回。
 */
static void call_tick(mqttd_cfg_t *cfg)
{
    char completed_epoch[48]={0},epoch[48]={0},topic[516]={0};
    int notify=0,ok=0,expired=0,have=0;
    call_up_frame_t frame;
    unsigned long long before=0;
    pthread_mutex_lock(&cfg->ipc_tx_lock);
    if (cfg->call_tx_mid>0) {
        ok=cfg->call_completed[cfg->call_tx_mid]>cfg->call_tx_before;
        if (ok || monotonic_millis()-cfg->call_tx_since>500) {
            memcpy(completed_epoch,cfg->call_tx_epoch,sizeof(completed_epoch)); notify=cfg->call_enabled;
            if (ok) cfg->call_tx_mid=0;
            else {
                if (cfg->call_enabled) ++cfg->call_tx_drops;
                cfg->call_enabled=0;cfg->call_rx_count=0;cfg->call_up_count=0;
            }
        }
    }
    memcpy(epoch,cfg->call_epoch,sizeof(epoch));
    if (cfg->call_enabled && !cfg->call_tx_mid) {
        while (cfg->call_up_count) {
            frame=cfg->call_up[cfg->call_up_head];
            cfg->call_up_head=(cfg->call_up_head+1)%5; --cfg->call_up_count;
            if (monotonic_millis()>frame.deadline_ms) { ++expired; ++cfg->call_tx_drops; continue; }
            snprintf(topic,sizeof(topic),"%s/up",cfg->call_audio_topic);
            before=cfg->call_callback_serial;have=1;break;
        }
    }
    pthread_mutex_unlock(&cfg->ipc_tx_lock);
    if (notify) call_ack(cfg,"call_sent",completed_epoch,ok);
    for (int i=0;i<expired;++i) call_ack(cfg,"call_sent",epoch,1);
    if (!have) return;
    pthread_mutex_lock(&cfg->state_lock);int connected=cfg->mqtt_connected;pthread_mutex_unlock(&cfg->state_lock);
    int mid=0;
    int rc=connected && cfg->mosq ? mosquitto_publish(cfg->mosq,&mid,topic,650,frame.pcm,0,false) : MOSQ_ERR_INVAL;
    if (rc==MOSQ_ERR_SUCCESS && mid>0 && mid<=65535) {
        pthread_mutex_lock(&cfg->ipc_tx_lock);
        cfg->call_tx_mid=mid;cfg->call_tx_before=before;cfg->call_tx_since=monotonic_millis();
        memcpy(cfg->call_tx_epoch,epoch,sizeof(cfg->call_tx_epoch));
        pthread_mutex_unlock(&cfg->ipc_tx_lock);
    } else { call_reset(cfg); call_ack(cfg,"call_sent",epoch,0); }
}

/** @brief 十六进制单字符解码。
 * @param c ASCII 字符，大小写字母均可。
 * @return 0..15 表示成功；-1 表示不是十六进制字符。
 */
static int call_hex(unsigned char c)
{
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}

/** @brief 主线程处理媒体代次门、下行信用及 650B 二进制上行。
 * @param cfg 非空运行配置；仅主线程执行命令。
 * @param line 完整的单行 JSON，解析失败或代次错误时拒绝/忽略。
 * @details 本地 IPC 使用 hex 保留零字节，MQTT 发布显式长度 650、QoS0、retain=false。
 *          IPC 最多 5 帧窗口与 5 帧待发环之外不排队；libmosquitto 仍只保留一个音频在途帧；所有库调用均在短队列锁外。
 */
static void call_command(mqttd_cfg_t *cfg,const char *line)
{
    cJSON *obj=cJSON_Parse(line);
    if (!obj) return;
    cJSON *cmd=cJSON_GetObjectItemCaseSensitive(obj,"cmd");
    cJSON *epoch=cJSON_GetObjectItemCaseSensitive(obj,"epoch");
    if (!cJSON_IsString(cmd) || !cJSON_IsString(epoch) || strlen(epoch->valuestring)!=36) { cJSON_Delete(obj); return; }
    if (strcmp(cmd->valuestring,"call_media")==0) {
        int enabled=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(obj,"enabled"));
        if (!enabled) call_reset(cfg);
        cJSON *topic=cJSON_GetObjectItemCaseSensitive(obj,"topic");
        int valid_topic=cJSON_IsString(topic);
        if (valid_topic) {
            size_t len=strlen(topic->valuestring);
            valid_topic=len>21 && len<sizeof(cfg->call_audio_topic)-3 &&
                strncmp(topic->valuestring,"device/ycLinux/",15)==0 &&
                strcmp(topic->valuestring+len-6,"/audio")==0 &&
                !strchr(topic->valuestring,'+') && !strchr(topic->valuestring,'#');
        }
        pthread_mutex_lock(&cfg->ipc_tx_lock);
        int ready=!cfg->call_tx_mid && (!enabled || valid_topic);
        cfg->call_enabled=enabled && ready;
        if (cfg->call_enabled) cfg->broadcast_head=cfg->broadcast_count=0;
        if (enabled && ready) {
            strcpy(cfg->call_audio_topic,topic->valuestring);
            cfg->call_rx_drops=cfg->call_tx_drops=0;
        }
        memcpy(cfg->call_epoch,epoch->valuestring,37);
        cfg->call_rx_count=cfg->call_rx_head=0; cfg->call_rx_pending=0;
        cfg->call_rx_acked_token=cfg->call_rx_token;
        cfg->call_up_head=cfg->call_up_count=0;
        pthread_mutex_unlock(&cfg->ipc_tx_lock);
        call_ack(cfg,"call_ready",epoch->valuestring,ready);
    } else if (strcmp(cmd->valuestring,"audio_received")==0) {
        cJSON *token=cJSON_GetObjectItemCaseSensitive(obj,"token");
        pthread_mutex_lock(&cfg->ipc_tx_lock);
        if (cJSON_IsNumber(token) && strcmp(cfg->call_epoch,epoch->valuestring)==0 &&
            token->valuedouble>(double)cfg->call_rx_acked_token && token->valuedouble<=(double)cfg->call_rx_token) {
            unsigned long acked=(unsigned long)token->valuedouble;
            int consumed=0;
            while (consumed<cfg->call_rx_pending && cfg->call_rx_outstanding[consumed]<=acked) ++consumed;
            memmove(cfg->call_rx_outstanding,&cfg->call_rx_outstanding[consumed],
                (size_t)(cfg->call_rx_pending-consumed)*sizeof(unsigned long));
            cfg->call_rx_pending-=consumed;cfg->call_rx_acked_token=acked;
        }
        pthread_cond_signal(&cfg->ipc_tx_cond);
        pthread_mutex_unlock(&cfg->ipc_tx_lock);
    } else if (strcmp(cmd->valuestring,"publish_audio")==0) {
        cJSON *topic=cJSON_GetObjectItemCaseSensitive(obj,"topic");
        cJSON *hex=cJSON_GetObjectItemCaseSensitive(obj,"hex");
        unsigned char packet[650]; int valid=cJSON_IsString(topic) && cJSON_IsString(hex);
        if (valid) {
            const size_t tl=strlen(topic->valuestring);
            valid=tl>9 && tl<512 && strncmp(topic->valuestring,"device/ycLinux/",15)==0 &&
                strcmp(topic->valuestring+tl-9,"/audio/up")==0 && strlen(hex->valuestring)==1300;
        }
        for (int i=0;valid && i<650;++i) {
            int a=call_hex((unsigned char)hex->valuestring[2*i]),b=call_hex((unsigned char)hex->valuestring[2*i+1]);
            if (a<0 || b<0) valid=0; else packet[i]=(unsigned char)((a<<4)|b);
        }
        if (valid) valid=packet[0]==0xA1 && packet[1]==0xC0 && packet[2]==1 && packet[3]==0 && packet[4]==1;
        pthread_mutex_lock(&cfg->ipc_tx_lock);
        char expected[516]; snprintf(expected,sizeof(expected),"%s/up",cfg->call_audio_topic);
        valid=valid && cfg->call_enabled && strcmp(cfg->call_epoch,epoch->valuestring)==0 &&
            strcmp(topic->valuestring,expected)==0;
        int dropped=0;
        cJSON *age=cJSON_GetObjectItemCaseSensitive(obj,"ageMs");
        double age_ms=cJSON_IsNumber(age) ? age->valuedouble : 0;
        if (age_ms<0 || age_ms>100) valid=0;
        if (valid) {
            if (cfg->call_up_count==5) {
                cfg->call_up_head=(cfg->call_up_head+1)%5;--cfg->call_up_count;++cfg->call_tx_drops;dropped=1;
            }
            call_up_frame_t *slot=&cfg->call_up[(cfg->call_up_head+cfg->call_up_count)%5];
            memcpy(slot->pcm,packet,650);slot->deadline_ms=monotonic_millis()+100-(long long)age_ms;
            ++cfg->call_up_count;
        }
        pthread_mutex_unlock(&cfg->ipc_tx_lock);
        // 过期音频只丢帧并归还窗口，不把可恢复背压升级为整个通话失败。
        if (!valid || dropped) call_ack(cfg,"call_sent",epoch->valuestring,1);
    }
    cJSON_Delete(obj);
}

/** @brief 沿原路径发布 JSON/文本；call.* 和 OTA 固定 QoS2，OTA 文本不重编码。
 * @param cfg 非空配置，只由 IPC 主线程访问客户端生命周期。
 * @param line 完整 publish JSON；普通业务沿用 ACK，OTA 文本以持久结果重放补偿连接失败。
 */
static void handle_publish(mqttd_cfg_t *cfg, const char *line)
{
    cJSON *root = cJSON_Parse(line);
    if (!root) {
        ipc_send_ack(cfg, "publish", 0, "invalid json");
        return;
    }

    cJSON *topicItem = cJSON_GetObjectItemCaseSensitive(root, "topic");
    cJSON *payloadItem = cJSON_GetObjectItemCaseSensitive(root, "payload");
    /* OTA has its own durable result path; generic ACK/logs stay enabled for other business. */
    const bool ota = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ota"));

    if (!cJSON_IsString(topicItem) || topicItem->valuestring == NULL || topicItem->valuestring[0] == '\0') {
        cJSON_Delete(root);
        if (!ota) ipc_send_ack(cfg, "publish", 0, "missing topic");
        return;
    }

    if (!payloadItem) {
        cJSON_Delete(root);
        if (!ota) ipc_send_ack(cfg, "publish", 0, "missing payload");
        return;
    }

    pthread_mutex_lock(&cfg->mosq_lock);
    struct mosquitto *m = cfg->mosq;
    pthread_mutex_unlock(&cfg->mosq_lock);

    if (!m) {
        cJSON_Delete(root);
        if (!ota) ipc_send_ack(cfg, "publish", 0, "mosq not ready");
        return;
    }

    pthread_mutex_lock(&cfg->state_lock);
    int connected = cfg->mqtt_connected;
    pthread_mutex_unlock(&cfg->state_lock);

    if (!connected) {
        cJSON_Delete(root);
        if (!ota) ipc_send_ack(cfg, "publish", 0, "mqtt not connected");
        return;
    }

    char *payloadText = NULL;

    if (cJSON_IsString(payloadItem) && payloadItem->valuestring) {
        // 如果 payload 本身就是字符串，直接用
        payloadText = strdup(payloadItem->valuestring);
    } else {
        // 如果 payload 是 object/array，用 cJSON 重新序列化成紧凑 JSON
        payloadText = cJSON_PrintUnformatted(payloadItem);
    }

    if (!payloadText) {
        cJSON_Delete(root);
        if (!ota) ipc_send_ack(cfg, "publish", 0, "build payload failed");
        return;
    }

    //dump_hex("publish-payload", (const unsigned char *)payloadText, (int)strlen(payloadText));

    int publish_qos=cfg->qos;
    if (ota && cJSON_IsString(payloadItem)) {
        cJSON *ota_payload=cJSON_Parse(payloadText);
        cJSON *method=ota_payload ? cJSON_GetObjectItemCaseSensitive(ota_payload,"method") : NULL;
        if (cJSON_IsString(method) && (strcmp(method->valuestring,"ota.progress")==0 ||
                strcmp(method->valuestring,"ota.result")==0 || strcmp(method->valuestring,"ota.version")==0)) publish_qos=2;
        cJSON_Delete(ota_payload); /* Parse for method only; never print/re-serialize taskId. */
    }
    const char *live_method=NULL;
    if (cJSON_IsObject(payloadItem)) {
        cJSON *method=cJSON_GetObjectItemCaseSensitive(payloadItem,"method");
        if (cJSON_IsString(method)) {
            const char *name=method->valuestring;
            if (strcmp(name,"broadcastLiveAck")==0 || strcmp(name,"broadcastLiveState")==0) live_method=name;
            if (strncmp(name,"call.",5)==0 || strcmp(name,"broadcastAck")==0 ||
                    strcmp(name,"broadcastState")==0 || strcmp(name,"broadcastTagPlay")==0 ||
                    strcmp(name,"broadcastLiveAck")==0 || strcmp(name,"broadcastLiveState")==0)
                publish_qos=2;
        }
    }
    int r = mosquitto_publish(
        m,
        NULL,
        topicItem->valuestring,
        (int)strlen(payloadText),
        payloadText,
        publish_qos,
        false
    );
    if (live_method)
        fprintf(stderr,"[mqttd-live] publish method=%s qos=%d rc=%d\n",live_method,publish_qos,r);

    if (r == MOSQ_ERR_SUCCESS) {
        if (cfg->verbose && !ota) {
            fprintf(stderr, "[mqttd] IPC publish ok: topic=%s bytes=%zu\n",
                    topicItem->valuestring, strlen(payloadText));
        }
        if (!ota) ipc_send_ack(cfg, "publish", 1, "published");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "publish failed: %s", mosquitto_strerror(r));
        if (!ota) ipc_send_ack(cfg, "publish", 0, msg);
    }

    free(payloadText);
    cJSON_Delete(root);
}


/** @brief 设置独立直播媒体门并汇总上一代次的收帧和出队统计。
 * @param cfg 非空配置，入口在IPC主线程，状态由ipc_tx_lock保护。
 * @param line 非空、以NUL结束的本地JSON行。
 */
static void broadcast_command(mqttd_cfg_t *cfg, const char *line)
{
    cJSON *root=cJSON_Parse(line);
    if (!root) return;
    const cJSON *topic=cJSON_GetObjectItemCaseSensitive(root,"topic");
    const cJSON *epoch=cJSON_GetObjectItemCaseSensitive(root,"epoch");
    const cJSON *frame=cJSON_GetObjectItemCaseSensitive(root,"frameMs");
    const int enabled=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"enabled"));
    const char *name=cJSON_IsString(topic) ? topic->valuestring : "";
    const char *token=cJSON_IsString(epoch) ? epoch->valuestring : "";
    const char *suffix="/broadcast/audio";
    const size_t name_len=strlen(name);
    const char *end=name_len>31 ? name+name_len-16 : NULL;
    const char *allowed="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
    const size_t device_len=end && end>name+15 ? (size_t)(end-name-15) : 0;
    const int valid=device_len && strlen(name)<sizeof(cfg->broadcast_topic) &&
        strncmp(name,"device/ycLinux/",15)==0 && strcmp(end,suffix)==0 &&
        strspn(name+15,allowed)==device_len &&
        strlen(token)==36 && strspn(token,"abcdefABCDEF0123456789-")==36 &&
        cJSON_IsNumber(frame) && (frame->valuedouble==20 || frame->valuedouble==40);
    char previous_epoch[37];
    pthread_mutex_lock(&cfg->ipc_tx_lock);
    const int was_enabled=cfg->broadcast_expected!=0;
    const unsigned long seen=cfg->broadcast_seen, accepted=cfg->broadcast_accepted;
    const unsigned long picked=cfg->broadcast_ipc_picked, dropped=cfg->broadcast_queue_drops;
    memcpy(previous_epoch,cfg->broadcast_epoch,sizeof(previous_epoch));
    cfg->broadcast_head=cfg->broadcast_count=0;
    cfg->broadcast_expected=0;
    cfg->broadcast_seen=cfg->broadcast_accepted=cfg->broadcast_ipc_picked=cfg->broadcast_queue_drops=0;
    if (enabled && valid) {
        strcpy(cfg->broadcast_topic,name);
        strcpy(cfg->broadcast_epoch,token);
        cfg->broadcast_expected=(size_t)(10+frame->valueint*32);
    }
    pthread_mutex_unlock(&cfg->ipc_tx_lock);
    if (was_enabled)
        fprintf(stderr,"[mqttd-live] media_summary epoch=%s mqttFrames=%lu acceptedFrames=%lu ipcPicked=%lu queueDrops=%lu\n",
                previous_epoch,seen,accepted,picked,dropped);
    fprintf(stderr,"[mqttd-live] media_gate enabled=%d accepted=%d epoch=%s frameMs=%d\n",
            enabled,!enabled || valid,enabled && valid ? token : "-",enabled && valid ? frame->valueint : 0);
    cJSON_Delete(root);
}

/* ========== IPC：处理 Qt 发来的命令，并返回结果（本回合先做 test/status） ========== */
/** @brief 分派本地命令，通话二进制命令使用独立信用，不改变原 JSON 命令路径。
 * @param cfg 非空配置，仅由 IPC 主线程调用。
 * @param line 以零结束的完整 JSON 行，未知命令保留原错误响应。
 */
static void ipc_handle_command(mqttd_cfg_t *cfg, const char *line)
{
    const char *cmd = json_find_cmd(line);
    if (!cmd) return;
    if (strncmp(cmd,"call_media\"",11)==0 || strncmp(cmd,"publish_audio\"",14)==0 ||
        strncmp(cmd,"audio_received\"",15)==0) { call_command(cfg,line); return; }

    if (strncmp(cmd, "broadcast_media\"", 16) == 0) {
        broadcast_command(cfg,line);
        return;
    }
    if (strncmp(cmd, "test\"", 5) == 0) {
        ipc_send_status(cfg, "test_result");
        return;
    }
    if (strncmp(cmd, "status\"", 7) == 0) {
        ipc_send_status(cfg, "status");
        return;
    }

    if (strncmp(cmd, "set_config\"", 11) == 0) {
        handle_set_config(cfg, line);
        return;
    }
    if (strncmp(cmd, "apply\"", 6) == 0) {
        handle_apply(cfg);
        return;
    }
    if (strncmp(cmd, "publish\"", 8) == 0) {
        handle_publish(cfg, line);
        return;
    }

    ipc_send_line(cfg, "{\"type\":\"error\",\"msg\":\"unsupported cmd\"}");
}

/* ========== IPC：读 Qt 发来的数据（按行） ========== */
// static void ipc_read_loop(mqttd_cfg_t *cfg)
// {
//     pthread_mutex_lock(&cfg->ipc_lock);
//     int cfd = cfg->ipc_client_fd;
//     pthread_mutex_unlock(&cfg->ipc_lock);

//     if (cfd < 0) return;

//     char buf[1024];
//     ssize_t n = read(cfd, buf, sizeof(buf) - 1);
//     if (n == 0) {
//         // Qt 断开
//         if (cfg->verbose) fprintf(stderr, "[ipc] Qt client disconnected\n");
//         pthread_mutex_lock(&cfg->ipc_lock);
//         close(cfg->ipc_client_fd);
//         cfg->ipc_client_fd = -1;
//         pthread_mutex_unlock(&cfg->ipc_lock);
//         return;
//     }
//     if (n < 0) {
//         // 非阻塞读：没数据会 EAGAIN
//         if (errno == EAGAIN || errno == EWOULDBLOCK) return;

//         // 其它错误：关闭连接
//         if (cfg->verbose) perror("[ipc] read");
//         pthread_mutex_lock(&cfg->ipc_lock);
//         close(cfg->ipc_client_fd);
//         cfg->ipc_client_fd = -1;
//         pthread_mutex_unlock(&cfg->ipc_lock);
//         return;
//     }

//     buf[n] = '\0';

//     // 简化处理：假设一条命令一行（Qt 发送时保证带 '\n'）
//     // 多行做按 '\n' 切分
//     char *saveptr = NULL;
//     char *line = strtok_r(buf, "\n", &saveptr);
//     while (line) {
//         ipc_handle_command(cfg, line);
//         line = strtok_r(NULL, "\n", &saveptr);
//     }
// }

static int ipc_rx_ensure_capacity(mqttd_cfg_t *cfg, size_t required)
{
    if (required <= cfg->ipc_rx_cap) return 0;
    if (required > IPC_RX_MAX_CAP) return -1;

    size_t new_cap = cfg->ipc_rx_cap ? cfg->ipc_rx_cap : IPC_RX_INITIAL_CAP;
    while (new_cap < required) {
        if (new_cap >= IPC_RX_MAX_CAP / 2U) {
            new_cap = IPC_RX_MAX_CAP;
            break;
        }
        new_cap *= 2U;
    }

    char *new_buf = (char *)realloc(cfg->ipc_rx_buf, new_cap);
    if (!new_buf) return -1;
    cfg->ipc_rx_buf = new_buf;
    cfg->ipc_rx_cap = new_cap;
    if (cfg->verbose) fprintf(stderr, "[ipc] RX buffer grown bytes=%zu\n", new_cap);
    return 0;
}

static void ipc_read_loop(mqttd_cfg_t *cfg)
{
    unsigned long generation = 0;
    int cfd = ipc_dup_client(cfg, &generation);
    if (cfd < 0) return;

    char tmp[4096];
    ssize_t n = read(cfd, tmp, sizeof(tmp));
    int saved_errno = errno;
    close(cfd);

    if (n == 0) {
        if (cfg->verbose) fprintf(stderr, "[ipc] Qt client disconnected\n");
        ipc_close_client_if_generation(cfg, generation);
        cfg->ipc_rx_len = 0;
        if (cfg->ipc_rx_buf) cfg->ipc_rx_buf[0] = '\0';
        return;
    }

    if (n < 0) {
        if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) return;

        errno = saved_errno;
        if (cfg->verbose) perror("[ipc] read");
        ipc_close_client_if_generation(cfg, generation);
        cfg->ipc_rx_len = 0;
        if (cfg->ipc_rx_buf) cfg->ipc_rx_buf[0] = '\0';
        return;
    }

    size_t required = cfg->ipc_rx_len + (size_t)n + 1U;
    if (ipc_rx_ensure_capacity(cfg, required) != 0) {
        fprintf(stderr,
                "[ipc] command too long or allocation failed, drop buffer len=%zu add=%zd max=%u\n",
                cfg->ipc_rx_len, n, IPC_RX_MAX_CAP);
        cfg->ipc_rx_len = 0;
        if (cfg->ipc_rx_buf) cfg->ipc_rx_buf[0] = '\0';
        ipc_send_ack(cfg, "publish", 0, "ipc line too long");
        return;
    }

    memcpy(cfg->ipc_rx_buf + cfg->ipc_rx_len, tmp, (size_t)n);
    cfg->ipc_rx_len += (size_t)n;
    cfg->ipc_rx_buf[cfg->ipc_rx_len] = '\0';

    char *line_start = cfg->ipc_rx_buf;
    char *newline = NULL;

    while ((newline = strchr(line_start, '\n')) != NULL) {
        *newline = '\0';

        if (*line_start != '\0') {
            ipc_handle_command(cfg, line_start);
        }

        line_start = newline + 1;
    }

    size_t remain = cfg->ipc_rx_buf + cfg->ipc_rx_len - line_start;
    if (line_start != cfg->ipc_rx_buf && remain > 0) {
        memmove(cfg->ipc_rx_buf, line_start, remain);
    }
    cfg->ipc_rx_len = remain;
    cfg->ipc_rx_buf[cfg->ipc_rx_len] = '\0';
}

/*
 * on_connect：连接回调
 * libmosquitto 在以下时机会调用：
 * - 初次连接成功时
 * - 断线重连成功时
 *
 * 参数说明：
 * mosq：libmosquitto 客户端对象
 * userdata：我们在 mosquitto_new() 里传进去的 &cfg
 * rc：连接结果（0 表示成功，其它表示失败）
 */
/** @brief 在既有连接中恢复订阅，ycLinux request 使用 QoS2、audio 使用 QoS0。
 * @param mosq 触发回调的既有客户端。
 * @param userdata 非空运行配置，生命周期覆盖网络线程。
 * @param rc CONNACK 结果；0 成功，其他值按原连接失败流程上报。
 */
static void on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    mqttd_cfg_t *cfg = (mqttd_cfg_t *)userdata;

    pthread_mutex_lock(&cfg->state_lock);
    cfg->last_conn_rc = rc;     // 更新错误码
    cfg->mqtt_connected = (rc == 0) ? 1 : 0;
    pthread_mutex_unlock(&cfg->state_lock);

    if (rc == 0) {
        call_connected(cfg);
        cancel_apply_retry(cfg);

        // rc==0：CONNACK 接受，连接成功
        if (cfg->verbose) {
            fprintf(stderr, "[mqttd] connected\n");
        }

        // 连接成功后订阅所有 topic
        for (int i = 0; i < cfg->topic_count; i++) {
            int sub_qos=cfg->qos;
            const char *tp=cfg->topics[i]; size_t tl=strlen(tp);
            if (strncmp(tp,"device/ycLinux/",15)==0) {
                if (tl>6 && strcmp(tp+tl-6,"/audio")==0) sub_qos=0;
                else if (tl>8 && strcmp(tp+tl-8,"/request")==0) sub_qos=2;
            }
            int s = mosquitto_subscribe(mosq, NULL, cfg->topics[i], sub_qos);
            if (s != MOSQ_ERR_SUCCESS) {
                // 订阅失败时输出错误原因
                fprintf(stderr, "[mqttd] subscribe failed (%s): %s\n",
                        cfg->topics[i], mosquitto_strerror(s));
            } else if (cfg->verbose) {
                fprintf(stderr, "[mqttd] subscribed: %s (qos=%d)\n", cfg->topics[i], cfg->qos);
            }
        }

        // 推送给 Qt：连接成功事件 + 当前状态
        ipc_send_line(cfg, "{\"type\":\"conn\",\"state\":\"connected\"}");
        ipc_send_status(cfg, "status");

        // 连接后发布一次消息（用于快速验证 publish 是否正常）
        if (cfg->pub_topic && cfg->pub_msg && cfg->pub_interval_sec == 0) {
            int p = mosquitto_publish(mosq, NULL, cfg->pub_topic,
                                      (int)strlen(cfg->pub_msg), cfg->pub_msg,
                                      cfg->qos, false);
            if (p != MOSQ_ERR_SUCCESS) {
                fprintf(stderr, "[mqttd] publish-once failed: %s\n", mosquitto_strerror(p));
            } else if (cfg->verbose) {
                fprintf(stderr, "[mqttd] published once: %s\n", cfg->pub_topic);
            }
        }
    } else {
        // 推送给 Qt：连接失败
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"conn\",\"state\":\"failed\",\"rc\":%d,\"desc\":\"%s\"}",
                 rc, mosquitto_connack_string(rc));
        ipc_send_line(cfg, msg);
        fprintf(stderr, "[mqttd] connect failed rc=%d (%s)\n", rc, mosquitto_connack_string(rc));
    }
}


// subscribe回调
static void on_subscribe(struct mosquitto *mosq, void *userdata,
                         int mid, int qos_count, const int *granted_qos)
{
    mqttd_cfg_t *cfg = userdata;
    (void)mosq;

    fprintf(stderr, "[mqttd] SUBACK mid=%d qos_count=%d\n", mid, qos_count);
    for (int i = 0; i < qos_count; i++) {
        fprintf(stderr, "  granted_qos[%d]=%d\n", i, granted_qos[i]); // 0/1/2 或失败(3.1.1常见0x80)
    }
}



/*
 * on_disconnect：断开连接回调
 * rc：断开原因（0 通常表示主动断开；非 0 可能是网络断开等）
 */
static void on_disconnect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq;
    mqttd_cfg_t *cfg = (mqttd_cfg_t *)userdata;
    broadcast_reset(cfg);
    call_reset(cfg);

    pthread_mutex_lock(&cfg->state_lock);
    cfg->mqtt_connected = 0;
    cfg->last_conn_rc = rc;                     //断开时同步更新最近错误码
    pthread_mutex_unlock(&cfg->state_lock);

    /* 断线原因属于运行错误信息，不能受 verbose 调试开关影响。 */
    fprintf(stderr, "[mqttd] disconnected rc=%d (%s)\n",
            rc, mosquitto_strerror(rc));
    
    // 推送给 Qt：断开事件 + 状态
    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"conn\",\"state\":\"disconnected\",\"rc\":%d,\"desc\":\"%s\"}",
             rc, mosquitto_strerror(rc));
    ipc_send_line(cfg, msg);
    ipc_send_status(cfg, "status");
}


/*
 * on_message：收到订阅消息回调
 * msg：包含 topic、payload、payloadlen、qos、retain 等信息
 */
// static void on_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg)
// {
//     (void)mosq;
//     mqttd_cfg_t *cfg = (mqttd_cfg_t *)userdata;
//     if (!msg || !msg->topic) return;
    
//     // Print: topic payload
//     // payload may contain binary, but for your current JSON test this is fine.
//     if (cfg->verbose) {
//         fprintf(stderr, "[mqttd] RX topic=%s qos=%d retain=%d len=%d\n",
//                 msg->topic, msg->qos, msg->retain, msg->payloadlen);
//     }
    
//     // 仍然打印到 stdout（你调试用）
//     printf("%s %.*s\n", msg->topic, msg->payloadlen, (const char *)msg->payload);
//     fflush(stdout);

//     // 推送给 Qt：收到消息事件
//     char out[1400];
//     snprintf(out, sizeof(out),
//              "{\"type\":\"msg\",\"topic\":\"%s\",\"qos\":%d,\"retain\":%d,\"payload\":\"%.*s\"}",
//              msg->topic, msg->qos, msg->retain, msg->payloadlen, (const char *)msg->payload);
//     ipc_send_line(cfg, out);
// }


/** @brief 网络线程分流音频到固定 10 帧接收环，其他消息仍复制到原业务队列。
 * @param mosq 原客户端，回调不创建其他客户端或网络循环。
 * @param userdata 非空运行配置，媒体门和接收环由 ipc_tx_lock 保护。
 * @param msg 借用的 MQTT 消息，回调返回后不保留其指针。
 * @note 仅接收当前精确主题、非 retain、V1头和协商长度；直播只累计计数，回调不写日志或 socket。
 */
static void on_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg)
{
    (void)mosq;
    mqttd_cfg_t *cfg = (mqttd_cfg_t *)userdata;
    if (msg && msg->topic) {
        const size_t len=strlen(msg->topic);
        if (len>31 && strncmp(msg->topic,"device/ycLinux/",15)==0 &&
                strcmp(msg->topic+len-16,"/broadcast/audio")==0) {
            const unsigned char *data=msg->payload;
            const int valid=!msg->retain && data && (msg->payloadlen==650 || msg->payloadlen==1290) &&
                memcmp(data,"\xA1\xC0\x01\x00\x01",5)==0;
            pthread_mutex_lock(&cfg->ipc_tx_lock);
            if (cfg->broadcast_expected && strcmp(msg->topic,cfg->broadcast_topic)==0) {
                ++cfg->broadcast_seen;
                if (valid && !cfg->call_enabled && cfg->broadcast_expected==(size_t)msg->payloadlen) {
                    ++cfg->broadcast_accepted;
                    if (cfg->broadcast_count==BROADCAST_RX_CAPACITY) {
                        cfg->broadcast_head=(cfg->broadcast_head+1)%BROADCAST_RX_CAPACITY;
                        --cfg->broadcast_count;
                        ++cfg->broadcast_queue_drops;
                    }
                    const unsigned int slot=(cfg->broadcast_head+cfg->broadcast_count)%BROADCAST_RX_CAPACITY;
                    memcpy(cfg->broadcast_packets[slot],data,(size_t)msg->payloadlen);
                    cfg->broadcast_arrival_ms[slot]=monotonic_millis();
                    ++cfg->broadcast_count;
                    pthread_cond_signal(&cfg->ipc_tx_cond);
                }
            }
            pthread_mutex_unlock(&cfg->ipc_tx_lock);
            return;
        }
        if (strncmp(msg->topic,"device/ycLinux/",15)==0 && len>6 && strcmp(msg->topic+len-6,"/audio")==0) {
            const unsigned char *data=msg->payload;
            if (len>=512 || msg->retain || msg->payloadlen!=650 || !data ||
                data[0]!=0xA1 || data[1]!=0xC0 || data[2]!=1 || data[3]!=0 || data[4]!=1) return;
            pthread_mutex_lock(&cfg->ipc_tx_lock);
            if (cfg->call_enabled && strcmp(msg->topic,cfg->call_audio_topic)==0) {
                if (cfg->call_rx_count==10) { cfg->call_rx_head=(cfg->call_rx_head+1)%10; --cfg->call_rx_count; ++cfg->call_rx_drops; }
                call_rx_frame_t *slot=&cfg->call_rx[(cfg->call_rx_head+cfg->call_rx_count)%10];
                memcpy(slot->pcm,data,650); memcpy(slot->topic,msg->topic,len+1);
                slot->arrival_ms=monotonic_millis(); ++cfg->call_rx_count;
                pthread_cond_signal(&cfg->ipc_tx_cond);
            }
            pthread_mutex_unlock(&cfg->ipc_tx_lock);
            return;
        }
    }

    if (!msg || !msg->topic) return;

    if (cfg->verbose) {
        fprintf(stderr, "[mqttd] RX topic=%s qos=%d retain=%d len=%d\n",
                msg->topic, msg->qos, msg->retain, msg->payloadlen);
    }

    /*
     * Never write a potentially large MQTT payload to Qt from libmosquitto's
     * network callback. Copy it into the IPC queue and return immediately so
     * MQTT keepalive and subsequent PUBLISH packets are not blocked by Qt.
     */
    int enqueue_result = ipc_tx_enqueue_mqtt_message(cfg, msg);
    if (enqueue_result != 0) {
        fprintf(stderr,
                "[mqttd] failed to queue RX topic=%s len=%d\n",
                msg->topic, msg->payloadlen);
    }
}


/**
 * @brief 请求正常断开并等待网络线程退出，成功后释放当前客户端。
 * @param cfg 非空运行配置，调用者已持有 mosq_lock；回调不能获取该锁。
 * @return 成功或原本无客户端时为 MOSQ_ERR_SUCCESS；否则返回 disconnect/loop_stop 错误码。
 * @note 仅主线程调用；loop_stop(false) 会等待网络线程退出，不设置硬超时。
 *       禁止强制取消网络线程，避免遗留 libmosquitto 内部发包锁。
 *       失败保留句柄和线程状态，调用者不得继续销毁句柄或释放其回调 userdata。
 */
static int mqtt_stop_and_destroy_locked(mqttd_cfg_t *cfg)
{
    if (!cfg->mosq) return MOSQ_ERR_SUCCESS;

    if (cfg->verbose) fprintf(stderr, "[mqttd] stopping MQTT: disconnect begin loopStarted=%d\n", cfg->mosq_loop_started);
    int rc = mosquitto_disconnect(cfg->mosq);
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) {
        fprintf(stderr, "[mqttd] disconnect failed: %s; retaining client\n", mosquitto_strerror(rc));
        return rc;
    }

    if (cfg->mosq_loop_started) {
        if (cfg->verbose) fprintf(stderr, "[mqttd] stopping MQTT: graceful loop_stop begin\n");
        rc = mosquitto_loop_stop(cfg->mosq, false);
        if (rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "[mqttd] graceful loop_stop failed: %s; retaining client\n", mosquitto_strerror(rc));
            return rc;
        }
        cfg->mosq_loop_started = 0;
    }

    mosquitto_destroy(cfg->mosq);
    cfg->mosq = NULL;
    if (cfg->verbose) fprintf(stderr, "[mqttd] stopping MQTT: client destroyed\n");
    return MOSQ_ERR_SUCCESS;
}

/**
 * @brief 完成旧连接的安全退出后，按当前配置创建并启动新 MQTT 客户端。
 * @param cfg 非空运行配置，调用者已持有 mosq_lock。
 * @return MOSQ_ERR_SUCCESS 表示网络线程启动成功，不代表 Broker 已接受连接；其他值为具体失败码。
 * @note 认证、TLS、异步连接或线程启动失败时保留尚未运行的句柄，由下一次 apply 或退出清理。
 *       只有 loop_start 成功才置 mosq_loop_started，避免停止从未启动的线程。
 */
static int mqtt_build_and_start_locked(mqttd_cfg_t *cfg)
{
    int stopped = mqtt_stop_and_destroy_locked(cfg);
    if (stopped != MOSQ_ERR_SUCCESS) return stopped;

    broadcast_reset(cfg);
    call_reset(cfg);
    pthread_mutex_lock(&cfg->ipc_tx_lock);
    cfg->call_tx_mid=0; // 旧网络线程已 join，库内唯一音频包已随旧客户端销毁。
    pthread_mutex_unlock(&cfg->ipc_tx_lock);
    cfg->mosq = mosquitto_new(cfg->client_id, cfg->clean_session, cfg);
    if (!cfg->mosq) {
        return MOSQ_ERR_NOMEM;
    }

    // Optional auth
    if (cfg->username && cfg->username[0]) {
        int r = mosquitto_username_pw_set(cfg->mosq, cfg->username, cfg->password);
        if (r != MOSQ_ERR_SUCCESS) return r;
    }

    // TLS (optional)
    if (cfg->tls_enable) {
        int r = mosquitto_tls_set(cfg->mosq,
                                  cfg->cafile,   // cafile
                                  NULL,          // capath
                                  cfg->certfile, // certfile
                                  cfg->keyfile,  // keyfile
                                  NULL);
        if (r != MOSQ_ERR_SUCCESS) return r;

        mosquitto_tls_insecure_set(cfg->mosq, cfg->tls_insecure);
    }

    mosquitto_connect_callback_set(cfg->mosq, on_connect);
    mosquitto_disconnect_callback_set(cfg->mosq, on_disconnect);
    mosquitto_message_callback_set(cfg->mosq, on_message);
    mosquitto_subscribe_callback_set(cfg->mosq, on_subscribe);
    mosquitto_publish_callback_set(cfg->mosq,on_call_publish);
    if (cfg->verbose) mosquitto_log_callback_set(cfg->mosq, on_log);

    mosquitto_reconnect_delay_set(cfg->mosq, 1, 60, true);
    int cr = mosquitto_connect_async(cfg->mosq, cfg->host, cfg->port, cfg->keepalive);
    if (cr != MOSQ_ERR_SUCCESS) return cr;

    int lr = mosquitto_loop_start(cfg->mosq);
    if (lr == MOSQ_ERR_SUCCESS) cfg->mosq_loop_started = 1;
    return lr;
}

/**
 * @brief 在主线程串行应用当前连接配置；等待旧线程正常退出后才建立新客户端。
 * @param cfg 非空运行配置；状态变更由 state_lock 保护，客户端生命周期由 mosq_lock 保护。
 * @return 网络线程启动结果；失败由原 apply 回执和重试机制处理。
 * @note 本调用可能等待 libmosquitto 退出，期间 IPC 主循环不处理新命令；不承诺固定退出时限。
 */

static int mqtt_apply_reconnect(mqttd_cfg_t *cfg)
{
    pthread_mutex_lock(&cfg->mosq_lock);

    // apply 时先把状态设为“未知/未连接”，Qt 会收到后续 conn/status
    pthread_mutex_lock(&cfg->state_lock);
    cfg->mqtt_connected = 0;
    cfg->last_conn_rc = -1;
    pthread_mutex_unlock(&cfg->state_lock);

    int r = mqtt_build_and_start_locked(cfg);
    pthread_mutex_unlock(&cfg->mosq_lock);

    return r;
}


static void schedule_apply_retry(mqttd_cfg_t *cfg, int delay_sec)
{
    if (!cfg) return;
    if (delay_sec <= 0) delay_sec = 10;

    cfg->retry_apply_enabled = 1;
    cfg->retry_apply_interval_sec = delay_sec;
    cfg->retry_apply_next_ts = time(NULL) + delay_sec;

    if (cfg->verbose) {
        fprintf(stderr, "[mqttd] schedule apply retry in %d sec\n", delay_sec);
    }
}

static void cancel_apply_retry(mqttd_cfg_t *cfg)
{
    if (!cfg) return;

    cfg->retry_apply_enabled = 0;
    cfg->retry_apply_next_ts = 0;

    if (cfg->verbose) {
        fprintf(stderr, "[mqttd] cancel apply retry\n");
    }
}


/*
 * on_log：mosquitto 内部日志回调（只有开启 verbose 时才注册）
 * level：日志级别（MOSQ_LOG_*）
 * str：日志内容
 */
static void on_log(struct mosquitto *mosq, void *userdata, int level, const char *str)
{
    (void)mosq;
    (void)userdata;

    // mosquitto internal logs (helpful for debugging)
    // level is one of MOSQ_LOG_*
    fprintf(stderr, "[mosq] %d %s\n", level, str);
}


/** @brief 启动单 MQTT 网络循环和单 IPC 写线程，通话期间以 5ms 节奏服务音频命令。
 * @param argc 命令行参数个数。
 * @param argv 命令行参数数组，沿用原选项和周期发布行为。
 * @return 0 正常退出；非零为参数、IPC 启动或无法安全停止网络线程等错误。
 * @note 不增加 MQTT 客户端；网络线程必须退出后才能释放共享配置。
 */
int main(int argc, char **argv)
{
    mqttd_cfg_t cfg;
    
    // 初始化默认配置
    set_defaults(&cfg);

    // default client id: mqttd-<pid>
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "mqttd-%d", (int)getpid());
        cfg.client_id = strdup(buf);
    }

    static struct option long_opts[] = {
        {"host", required_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"client-id", required_argument, 0, 'i'},
        {"username", required_argument, 0, 'u'},
        {"password", required_argument, 0, 'P'},
        {"keepalive", required_argument, 0, 'k'},
        {"topic", required_argument, 0, 't'},
        {"qos", required_argument, 0, 'q'},
        {"verbose", no_argument, 0, 'v'},

        {"clean", no_argument, 0, 1000},
        {"no-clean", no_argument, 0, 1001},

        {"pub-topic", required_argument, 0, 1100},
        {"pub-msg", required_argument, 0, 1101},
        {"pub-interval", required_argument, 0, 1102},

        {"tls", no_argument, 0, 1200},
        {"cafile", required_argument, 0, 1201},
        {"cert", required_argument, 0, 1202},
        {"key", required_argument, 0, 1203},
        {"insecure", no_argument, 0, 1204},

        {"ipc", required_argument, 0, 1300}, // IPC socket 路径

        {0, 0, 0, 0}
    };

    
    /*
     * getopt_long 解析：
     * - "h:p:i:u:P:k:t:q:v" 代表支持的短参数列表
     *   例如 h: 表示 -h 需要一个参数；v 不带参数
     * - 返回值 opt：若是短参数会返回 'h' 'p'...
     *   若是 long_opts 的自定义 val，则返回 1000/1100 等
     */
    int opt;
    while ((opt = getopt_long(argc, argv, "h:p:i:u:P:k:t:q:v", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'h':
                free(cfg.host);
                cfg.host = strdup(optarg);
                break;
            case 'p':
                cfg.port = atoi(optarg);
                break;
            case 'i':
                free(cfg.client_id);
                cfg.client_id = strdup(optarg);
                break;
            case 'u':
                free(cfg.username);
                cfg.username = strdup(optarg);
                break;
            case 'P':
                free(cfg.password);
                cfg.password = strdup(optarg);
                break;
            case 'k':
                cfg.keepalive = atoi(optarg);
                break;
            case 't':
                add_topic(&cfg, optarg);
                break;
            case 'q':
                cfg.qos = atoi(optarg);
                if (cfg.qos < 0) cfg.qos = 0;
                if (cfg.qos > 2) cfg.qos = 2;
                break;
            case 'v':
                cfg.verbose = true;
                break;

            case 1000:
                cfg.clean_session = true;
                break;
            case 1001:
                cfg.clean_session = false;
                break;

            case 1100:
                free(cfg.pub_topic);
                cfg.pub_topic = strdup(optarg);
                break;
            case 1101:
                free(cfg.pub_msg);
                cfg.pub_msg = strdup(optarg);
                break;
            case 1102:
                cfg.pub_interval_sec = atoi(optarg);
                if (cfg.pub_interval_sec < 0) cfg.pub_interval_sec = 0;
                break;

            case 1200:
                cfg.tls_enable = true;
                break;
            case 1201:
                free(cfg.cafile);
                cfg.cafile = strdup(optarg);
                break;
            case 1202:
                free(cfg.certfile);
                cfg.certfile = strdup(optarg);
                break;
            case 1203:
                free(cfg.keyfile);
                cfg.keyfile = strdup(optarg);
                break;
            case 1204:
                cfg.tls_insecure = true;
                break;
            case 1300:
                free(cfg.ipc_path); cfg.ipc_path = strdup(optarg);
                break;
            default:
                print_usage(argv[0]);
                free_cfg(&cfg);
                return 2;
        }
    }

    // 如果没有提供 -t topic,不订阅任何 topic,但仍可用于发布（pub）
    if (cfg.verbose) {
        fprintf(stderr, "[mqttd] host=%s port=%d client_id=%s keepalive=%d clean=%d qos=%d tls=%d\n",
                cfg.host, cfg.port, cfg.client_id, cfg.keepalive, cfg.clean_session, cfg.qos, cfg.tls_enable);
    }

    // Signals
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* 1) 启动 IPC server（给 Qt 连） */
    if (ipc_server_init(&cfg) != 0) {
        fprintf(stderr, "[ipc] init failed, continue without IPC\n");
    }

    if (ipc_tx_start(&cfg) != 0) {
        if (cfg.ipc_client_fd >= 0) close(cfg.ipc_client_fd);
        if (cfg.ipc_listen_fd >= 0) close(cfg.ipc_listen_fd);
        if (cfg.ipc_path) unlink(cfg.ipc_path);
        free_cfg(&cfg);
        return 1;
    }

    /* libmosquitto 全局初始化（进程级别） */
    mosquitto_lib_init();
    // 启动 MQTT（按当前 cfg 建立连接）
    int r0 = mqtt_apply_reconnect(&cfg);
    if (r0 != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[mqttd] initial connect failed: %s\n", mosquitto_strerror(r0));
        // 不退出,等 Qt 下发 set_config/apply 再重连
    }


    /* 4) 主循环：处理 IPC（Qt 命令） + 可选周期 publish */
    // Main loop: optional periodic publish
    while (g_running) {
        // 如果 Qt 还没连上，尝试 accept
        ipc_accept_if_needed(&cfg);

        // 如果 Qt 已连上，读取它发来的命令并回复（test/status）
        ipc_read_loop(&cfg);
        call_tick(&cfg);

        if (cfg.retry_apply_enabled) {
            time_t now = time(NULL);

            pthread_mutex_lock(&cfg.state_lock);
            int connected = cfg.mqtt_connected;
            pthread_mutex_unlock(&cfg.state_lock);

            if (connected) {
                cancel_apply_retry(&cfg);
            } else if (cfg.retry_apply_next_ts > 0 && now >= cfg.retry_apply_next_ts) {
                int rr = mqtt_apply_reconnect(&cfg);
                if (rr == MOSQ_ERR_SUCCESS) {
                    if (cfg.verbose) {
                        fprintf(stderr, "[mqttd] retry apply triggered: reconnecting\n");
                    }
                    // 这里先不 cancel，等真正 on_connect 成功后再取消更稳
                    cfg.retry_apply_next_ts = now + cfg.retry_apply_interval_sec;
                } else {
                    if (cfg.verbose) {
                        fprintf(stderr, "[mqttd] retry apply failed: %s\n", mosquitto_strerror(rr));
                    }
                    cfg.retry_apply_next_ts = now + cfg.retry_apply_interval_sec;
                }
            }
        }

        // apply 或重试可能已销毁旧句柄，周期发布必须在它们之后读取当前句柄。
        pthread_mutex_lock(&cfg.mosq_lock);
        struct mosquitto *m = cfg.mosq;
        pthread_mutex_unlock(&cfg.mosq_lock);

        if (cfg.pub_topic && cfg.pub_msg && cfg.pub_interval_sec > 0) {
            if(m){
                // 周期性发布
                int p = mosquitto_publish(m, NULL, cfg.pub_topic,
                                        (int)strlen(cfg.pub_msg), cfg.pub_msg,
                                        cfg.qos, false);
                if (p != MOSQ_ERR_SUCCESS) {
                    fprintf(stderr, "[mqttd] publish failed: %s\n", mosquitto_strerror(p));
                } else if (cfg.verbose) {
                    fprintf(stderr, "[mqttd] published: %s\n", cfg.pub_topic);
                }
                // 分秒 sleep，确保 g_running 变 0 时可以更快退出
                for (int i=0;i<cfg.pub_interval_sec && g_running;++i) {
                    for (int step=0;step<200 && g_running;++step) {
                        ipc_accept_if_needed(&cfg); ipc_read_loop(&cfg); call_tick(&cfg); usleep(5000);
                    }
                }
            }

        } else {
            pthread_mutex_lock(&cfg.ipc_tx_lock);
            int calling=cfg.call_enabled;
            pthread_mutex_unlock(&cfg.ipc_tx_lock);
            usleep(calling ? 5000 : 100000); // 空闲保留原节奏，通话 5ms 服务 20ms PCM。
        }
    }

    if (cfg.verbose) {
        fprintf(stderr, "[mqttd] stopping...\n");
    }
    
    pthread_mutex_lock(&cfg.mosq_lock);
    int stop_rc = mqtt_stop_and_destroy_locked(&cfg);
    pthread_mutex_unlock(&cfg.mosq_lock);
    if (stop_rc != MOSQ_ERR_SUCCESS) {
        // 仍可能有网络线程使用 cfg，不能执行 free_cfg；进程退出由系统回收资源。
        fprintf(stderr, "[mqttd] shutdown aborted after client stop failure: %s\n", mosquitto_strerror(stop_rc));
        return 1;
    }

    ipc_tx_stop(&cfg);
    
    // 关闭 IPC fd
    if (cfg.ipc_client_fd >= 0) close(cfg.ipc_client_fd);
    if (cfg.ipc_listen_fd >= 0) close(cfg.ipc_listen_fd);
    if (cfg.ipc_path) unlink(cfg.ipc_path);


    free_cfg(&cfg);
    return 0;
}
