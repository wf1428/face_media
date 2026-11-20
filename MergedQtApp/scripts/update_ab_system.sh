#!/bin/bash
# @file update_ab_system.sh
# @brief 保留原 Bash 手动升级入口，提供 MQTT OTA 准备、独立安装和启动健康检查。
# @author Du lin
# @date 2026-11-18
# @note 最近修改：2026-11-20；修改人：Du lin；ZIP/tar.gz 同时接受根目录及统一一级目录，扫描时交叉核对路径和目录布局，保持固定叶文件提取与原有升级逻辑；移除镜像及本机 board-model 文件依赖，保留可选清单与设备树的匹配；OTA 不再静态推断镜像 fstab/boot.scr 的槽绑定，沿用 BSP 引导及启动挂载；界面按阶段0..100刷新，本地细进度写 /run，整体持久进度仍按5%节流。
#       目标槽健康通过后原子记录版本，记录失败恢复旧槽；保留 MD5、可选清单设备树匹配、实际挂载核对和终态 journal。
#       --format-data 仍仅限显式本地维护使用；远程 OTA 始终保留共享数据。

SCRIPT=/usr/local/sbin/update_ab_system.sh
BASE=/userdata/var/lib/face-media/ota
RUNTIME=/run/face-media-ota
VERSION_FILE=/etc/face-media/firmware-version
HEALTH_SECONDS=180
STABLE_SECONDS=30
MAX_TRIALS=2
RESERVE_BYTES=67108864
STATE='' TASK_ID='' TASK_DIR='' TASK_READY=0 OTA_MODE=''
ERROR_CODE=-5 ERROR_MESSAGE='' WORK='' JOB_PID='' JOB_RC=0 GLOBAL_MOUNT=''
EXEC_FD='' RESOURCE_FD='' GATE_FD='' LAST_STAGE='' LAST_PERCENT=-10
PREP_PARENT=0 HEARTBEAT_TOKEN='' HEARTBEAT_SEEN=0
# 仅对本地界面阶段进度去重，不替代整体进度的持久化/上报节流基准。
LAST_GUI_STAGE='' LAST_GUI_PERCENT=-1
export LC_ALL=C

# @brief 保存净化后的协议错误，不执行退出；调用者用 || return 传播失败。
# @param $1 6.18 数值错误码。@param $2 不含凭据的错误消息。@return 始终 1。
fail() { ERROR_CODE=$1; ERROR_MESSAGE=$2; return 1; }

# @brief 检查当前模式依赖的系统命令；缺失时返回 -5，不自动安装软件。
need_tools() {
    local item
    for item in "$@"; do command -v "$item" >/dev/null 2>&1 || { fail -5 "缺少系统命令：$item"; return; }; done
}

# @return 当前启动的单调秒数；不受设备时间校准影响。
mono_now() { local tick rest; read -r tick rest </proc/uptime || return; printf '%s\n' "${tick%%.*}"; }
# @return 当前 Linux 启动 ID。
boot_id() { cat /proc/sys/kernel/random/boot_id; }
# @brief 校验正 signed-long 十进制字符串，避免 Bash 溢出和 jq 的浮点精度损失。
positive_long() {
    [[ $1 =~ ^[1-9][0-9]*$ && ${#1} -le 19 ]] || return 1
    [[ ${#1} -lt 19 || ! $1 > 9223372036854775807 ]]
}
# @brief 校验平台发布版本/本槽已登记版本字符串；未确认的目标版本不能代替当前记录。
valid_version() { [[ $1 =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$ ]]; }
# @return 当前 rootfs 已登记的固件版本；缺失/无效返回失败，由调用者作为未登记处理。
real_version() {
    local value; [[ -r $VERSION_FILE ]] || return 1
    value=$(head -c 256 "$VERSION_FILE"); valid_version "$value" || return 1
    printf '%s\n' "$value"
}

# @brief 从 JSON 根对象词法读取 taskId，数字和内部字符串均不经过浮点转换。
# @note jq 仍负责完整 JSON 校验；这里拒绝重复根 taskId、转义 ID 和非正十进制 ID。
raw_task_id() {
    awk '
    { text=text $0 "\n" }
    END {
        depth=0; found=0; n=length(text)
        for(i=1;i<=n;i++) {
            c=substr(text,i,1)
            if(c=="\"") {
                start=i++; escaped=0
                for(;i<=n;i++) {
                    c=substr(text,i,1)
                    if(c=="\\") { i++; escaped=1; continue }
                    if(c=="\"") break
                }
                if(depth!=1 || escaped || substr(text,start,i-start+1)!="\"taskId\"") continue
                j=i+1; while(substr(text,j,1)~/[ \t\r\n]/ && j<=n)j++
                if(substr(text,j,1)!=":")continue
                j++; while(substr(text,j,1)~/[ \t\r\n]/ && j<=n)j++
                quoted=substr(text,j,1)=="\""; if(quoted)j++
                begin=j; while(substr(text,j,1)~/[0-9]/ && j<=n)j++
                id=substr(text,begin,j-begin)
                if(quoted) { if(substr(text,j,1)!="\"")exit 1; j++ }
                while(substr(text,j,1)~/[ \t\r\n]/ && j<=n)j++
                if(id!~/^[1-9][0-9]*$/ || substr(text,j,1)!~/[,}]/ || ++found!=1)exit 1
            } else if(c=="{" || c=="[")depth++
            else if(c=="}" || c=="]")depth--
        }
        if(found!=1)exit 1
        printf "%s\n",id
    }'
}

# @brief 将小型 JSON 原子替换并同步文件及父文件系统；失败保留可恢复状态。
# @param $1 固定内部路径。@param $2 不含 FTP 的 JSON 正文。
atomic_json() {
    local path=$1 data=$2 tmp parent
    parent=${path%/*}; mkdir -p -- "$parent" || { fail -5 '无法创建任务状态目录'; return; }
    [[ ! -L $path ]] || { fail -5 '状态文件不能是符号链接'; return; }
    tmp=$(mktemp "$parent/.ota-state.XXXXXXXX") || { fail -5 '无法创建状态临时文件'; return; }
    if ! printf '%s\n' "$data" >"$tmp" || ! sync -f "$tmp" || ! mv -f -- "$tmp" "$path" || ! sync -f "$parent"; then
        rm -f -- "$tmp"; fail -5 'OTA 状态同步失败'; return
    fi
}
# @brief 更新内存状态；过滤表达式均为脚本常量，参数用 jq --arg/--argjson 传入。
state_update() {
    local filter=$1 next; shift
    next=$(printf '%s' "$STATE" | jq -c "$@" "$filter") || { fail -5 'OTA 状态格式错误'; return; }
    STATE=$next
}
# @brief 同步任务状态后发布活动任务索引，taskId 在内部以精确十进制字符串保存。
state_save() {
    positive_long "$TASK_ID" || { fail -5 'OTA 任务标识无效'; return; }
    atomic_json "$TASK_DIR/state.json" "$STATE" || return
    atomic_json "$BASE/active.json" "{\"taskId\":\"$TASK_ID\"}"
}
# @brief 加载活动任务，兼容旧的数字型 taskId；状态只读后转为无损内部字符串。
load_active() {
    local index id raw
    [[ -f $BASE/active.json && ! -L $BASE/active.json ]] || return 2
    index=$(cat "$BASE/active.json") || return 1
    printf '%s' "$index" | jq -e 'type=="object"' >/dev/null 2>&1 || { fail -5 'OTA 索引损坏'; return; }
    id=$(printf '%s' "$index" | raw_task_id) && positive_long "$id" || { fail -5 'OTA 索引任务号无效'; return; }
    [[ -f $BASE/tasks/$id/state.json && ! -L $BASE/tasks/$id/state.json ]] || { fail -5 'OTA 状态文件缺失'; return; }
    [[ $(stat -c %s "$BASE/tasks/$id/state.json") -le 262144 ]] || { fail -5 'OTA 状态过大'; return; }
    raw=$(cat "$BASE/tasks/$id/state.json") || return 1
    [[ $(printf '%s' "$raw" | raw_task_id) == "$id" ]] || { fail -5 'OTA 索引与任务不一致'; return; }
    STATE=$(printf '%s' "$raw" | jq -ce --arg id "$id" 'select(type=="object")|.taskId=$id') || { fail -5 'OTA 状态损坏'; return; }
    TASK_ID=$id; TASK_DIR=$BASE/tasks/$id; TASK_READY=1
}
# @brief 使用 flock 与 Qt/Web/USB 共享同一 inode；调用者保存并关闭输出描述符。
# @param $1 固定锁路径。@param $2 接收描述符的变量名，仅由脚本常量提供。
acquire_lock() {
    local path=$1 fd
    [[ ! -L $path ]] || { fail -5 '锁文件不能是符号链接'; return; }
    mkdir -p -- "${path%/*}" || { fail -5 '锁目录不可写'; return; }
    exec {fd}<>"$path" || { fail -5 '锁文件不可写'; return; }
    if ! flock -xn "$fd"; then exec {fd}>&-; fail -5 '另一个升级或资源更新正在执行'; return; fi
    printf -v "$2" '%s' "$fd"
}
# @brief 释放已持有的文件锁，不删除锁文件。
release_lock() { local fd=$1; [[ $fd =~ ^[0-9]+$ ]] && exec {fd}>&-; return 0; }
# @brief 在任何 OTA 目录创建前拒绝单分区或未挂载的共享分区。
ensure_shared() {
    [[ -d /userdata ]] && mountpoint -q /userdata || { fail -5 '远程 OTA 需要已挂载的 /userdata 与 A/B 分区'; return; }
    mkdir -p -- "$BASE" "$RUNTIME" || { fail -5 '共享 OTA 目录不可写'; return; }
}
# @brief 仅清理受控任务目录中的大文件，保留状态与待报结果。
cleanup_artifacts() {
    local directory=$1 canonical root
    root=$(readlink -f "$BASE/tasks") || return 0
    canonical=$(readlink -f "$directory") || return 0
    [[ $canonical == "$root"/* && ${canonical#"$root"/} != */* ]] || return 1
    positive_long "${canonical##*/}" || return 1
    [[ ! -L $directory/images ]] || { fail -5 'OTA 镜像目录不能是符号链接'; return; }
    rm -rf -- "$directory/images"
    rm -f -- "$directory/package.part" "$directory/package.bin"
}
# @brief 回收已终结任务的包文件，避免上次启动执行器失败遗留大文件占空间。
cleanup_finished_tasks() {
    local entry
    for entry in "$BASE"/tasks/*; do
        [[ -d $entry && ! -L $entry && -f $entry/state.json ]] || continue
        positive_long "${entry##*/}" || continue
        [[ $(jq -r '.phase // ""' <"$entry/state.json" 2>/dev/null) == finished ]] && cleanup_artifacts "$entry"
    done
    return 0
}
# @brief 保存失败终态；只有整包 MD5 错误增加其独立失败计数，不写 journal。
finish_failure() {
    local code=$1 message=$2
    state_update '.phase="finished"|.result="failed"|.resultCode=$code|.errorMsg=$message|if $code==-4 then .md5Failures=((.md5Failures//0)+1) else . end' --argjson code "$code" --arg message "$message" || return
    state_save || return; cleanup_artifacts "$TASK_DIR"
}
# @brief 将执行器权重映射成界面阶段百分比；额外字节分数避免先截断整体进度造成跳变。
# @param $1 阶段。$2 整体起始百分比。$3 已处理字节（默认0）。$4 总字节（默认1）。$5 本作业整体跨度（默认0）。
# @return 输出当前阶段的0..100；没有字节计量的等待阶段输出0，不按时间模拟增长。
gui_stage_percent() {
    local stage=$1 base=$2 written=0 total=1 span=0 start range value
    if (($#>=3)); then written=$3; fi
    if (($#>=4)); then total=$4; fi
    if (($#>=5)); then span=$5; fi
    case $stage in
        downloading) start=5; range=50 ;;
        verifying) start=60; range=25 ;;
        installing) start=86; range=10 ;;
        *) printf '0\n'; return 0 ;;
    esac
    ((total>0)) || total=1
    value=$(( ((base-start)*100 + written*span*100/total) / range ))
    ((value<0)) && value=0; ((value>100)) && value=100
    printf '%s\n' "$value"
}
# @brief 保持原整体进度与持久化节流，同时向本地 GUI 提供更细的阶段进度。
# @param $1 平台阶段。$2 整体百分比。$3 可选内部 phase。$4 可选精确阶段百分比0..100。
# @note 本地进度写到 /run 的任务文件，不写 journal；显示文件失败不影响升级状态或结果。
progress() {
    local stage=$1 percent=$2 phase='' ui_percent='' old reported=0 payload directory temporary
    if (($#>=3)); then phase=$3; fi
    if (($#>=4)); then ui_percent=$4; fi
    [[ -n $ui_percent ]] || ui_percent=$(gui_stage_percent "$stage" "$percent")
    ((ui_percent<0)) && ui_percent=0; ((ui_percent>100)) && ui_percent=100
    old=$(printf '%s' "$STATE" | jq -r '.percent//0'); ((percent<old)) && percent=$old
    state_update '.state=$stage|.percent=$percent|if $phase!="" then .phase=$phase else . end' --arg stage "$stage" --argjson percent "$percent" --arg phase "$phase" || return
    if [[ $stage != "$LAST_STAGE" ]] || ((percent>=LAST_PERCENT+5)); then
        state_save || return
        LAST_STAGE=$stage; LAST_PERCENT=$percent; reported=1
    fi
    if [[ $stage != "$LAST_GUI_STAGE" ]] || ((ui_percent!=LAST_GUI_PERCENT || reported)); then
        payload=$(printf '%s' "$STATE" | jq -c --argjson localPercent "$ui_percent" '{taskId,state,percent,attempt,beforeBootId}|.stagePercent=$localPercent') || return 0
        if positive_long "$TASK_ID"; then
            directory=$RUNTIME/progress; temporary=$directory/$TASK_ID.json.tmp.$$
            if mkdir -p -- "$directory" 2>/dev/null; then
                if ! printf '%s\n' "$payload" >"$temporary" || ! mv -f -- "$temporary" "$directory/$TASK_ID.json"; then
                    rm -f -- "$temporary" 2>/dev/null || true
                fi
            fi
        fi
        [[ $OTA_MODE == prepare ]] && printf '%s\n' "$payload"
        LAST_GUI_STAGE=$stage; LAST_GUI_PERCENT=$ui_percent
    fi
    return 0
}
# @brief 读取挂载分区实际容量并同时执行可用空间与 95% 保护线校验。
check_space() {
    local directory=$1 needed=$2 size free available block total used room
    read -r size free available block < <(stat -f -c '%b %f %a %S' "$directory") || { fail -3 '无法读取存储容量'; return; }
    [[ $size =~ ^[0-9]+$ && $free =~ ^[0-9]+$ && $available =~ ^[0-9]+$ && $block =~ ^[0-9]+$ ]] || { fail -3 '存储容量数据无效'; return; }
    total=$((size*block)); used=$(((size-free)*block)); room=$((total/100*95-used))
    ((available*block<room)) && room=$((available*block))
    ((room>=RESERVE_BYTES && needed<=room-RESERVE_BYTES)) || { fail -3 '升级空间不足或将超过共享分区 95% 保护阈值'; return; }
}
# @brief 下载准备阶段检查取消、单调截止时间和原 Qt 父进程；写入阶段不接受取消。
prepare_guard() {
    [[ $OTA_MODE == prepare ]] || return 0
    [[ ! -e $TASK_DIR/cancel ]] || { fail -9 '平台已取消升级'; return; }
    local deadline now parent
    deadline=$(printf '%s' "$STATE" | jq -r '.deadlineMonotonic|floor'); now=$(mono_now)
    ((now<deadline)) || { fail -1 'OTA 执行预算已耗尽'; return; }
    if ((PREP_PARENT>1)); then
        parent=$(awk '/^PPid:/{print $2}' "/proc/$$/status" 2>/dev/null)
        [[ $parent == "$PREP_PARENT" ]] || { fail -1 'OTA 准备的 Qt 父进程已退出'; return; }
    fi
}
# @brief 终止自己创建的独立作业进程组，禁止对子进程以外的 PID 发信号。
stop_job() {
    local pid=$JOB_PID; JOB_PID=''
    [[ $pid =~ ^[0-9]+$ ]] && ((pid>1)) || return 0
    kill -TERM -- "-$pid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}
# @brief 后台运行可监控命令；子进程组监视本 Shell，父进程硬退出也会结束准备工作。
# @param $1 监控文件或 @io。@param $2 预期字节数。@param $3/$4 整体进度基数/跨度。
# @param $5 stdout 文件或 /dev/null。@param 后续参数 固定程序及独立 argv，不拼接平台命令。
run_job() {
    local watch=$1 total=$2 base=$3 span=$4 output=$5 written=0 worker='' status current_stage ui_percent
    shift 5; rm -f -- "$WORK/child.pid"
    setsid bash -c '
        parent=$1; pidfile=$2; shift 2
        "$@" <&0 & worker=$!
        printf "%s\n" "$worker" >"$pidfile"
        while kill -0 "$worker" 2>/dev/null; do
            if ! kill -0 "$parent" 2>/dev/null; then kill -TERM -- "-$$"; exit 1; fi
            sleep 1
        done
        wait "$worker"
    ' ota-job "$$" "$WORK/child.pid" "$@" <&0 >"$output" 2>/dev/null &
    JOB_PID=$!
    while kill -0 "$JOB_PID" 2>/dev/null; do
        prepare_guard || { stop_job; return 1; }
        if [[ -n $watch && $total -gt 0 ]]; then
            if [[ $watch == @io ]]; then
                [[ -f $WORK/child.pid ]] && read -r worker <"$WORK/child.pid"
                [[ $worker =~ ^[0-9]+$ ]] && written=$(awk '/^wchar:/{print $2}' "/proc/$worker/io" 2>/dev/null)
            elif [[ -f $watch ]]; then written=$(stat -c %s "$watch"); fi
            [[ $written =~ ^[0-9]+$ ]] || written=0
            if ((written>total)); then stop_job; fail -5 '文件超过声明的字节数'; return; fi
            current_stage=$(printf '%s' "$STATE" | jq -r '.state')
            ui_percent=$(gui_stage_percent "$current_stage" "$base" "$written" "$total" "$span")
            progress "$current_stage" "$((base+written*span/total))" '' "$ui_percent" || { stop_job; return; }
            [[ $OTA_MODE == prepare ]] && { check_space "$TASK_DIR" 0 || { stop_job; return; }; }
        fi
        sleep 1
    done
    wait "$JOB_PID"; status=$?; JOB_PID=''; JOB_RC=$status
    ((status==0))
}
# @brief 使用 md5sum 读取完整文件或恰好指定的块设备字节数，结果写 HASH。
hash_file() {
    local path=$1 limit=${2:-}
    if [[ -n $limit ]]; then
        run_job '' 0 0 0 "$WORK/hash.txt" bash -o pipefail -c 'head -c "$2" -- "$1" | md5sum' ota-hash "$path" "$limit" || { [[ -n $ERROR_MESSAGE ]] || fail -5 '分区 MD5 回读失败'; return 1; }
    else
        run_job '' 0 0 0 "$WORK/hash.txt" bash -c 'md5sum <"$1"' ota-hash "$path" || { [[ -n $ERROR_MESSAGE ]] || fail -5 '文件 MD5 校验失败'; return 1; }
    fi
    read -r HASH _ <"$WORK/hash.txt"
    [[ $HASH =~ ^[a-f0-9]{32}$ ]] || { fail -5 'MD5 输出无效'; return; }
}

# @brief 从当前根设备及同一父盘的 PARTLABEL 发现 A/B，不使用平台指定设备。
discover_partitions() {
    local source label disk device parent part kind boot_source data_source
    declare -gA DEV=()
    source=$(findmnt -n -o SOURCE --mountpoint /) && source=$(readlink -f "$source") || { fail -5 '无法确认根分区'; return; }
    label=$(blkid -s PARTLABEL -o value "$source") || { fail -5 '根分区标签不可读'; return; }
    [[ $label == rootfs_a || $label == rootfs_b ]] || { fail -5 '当前根分区不是 A/B 系统'; return; }
    CURRENT_SLOT=${label: -1}; TARGET_SLOT=a; [[ $CURRENT_SLOT == a ]] && TARGET_SLOT=b
    disk=$(lsblk -n -o PKNAME "$source"); disk=${disk##*/}
    [[ -n $disk && $disk != *$'\n'* ]] || { fail -5 '系统父盘不明确'; return; }
    while read -r device parent kind; do
        [[ $kind == part && ${parent##*/} == "$disk" ]] || continue
        part=$(blkid -s PARTLABEL -o value "$device" 2>/dev/null) || continue
        case $part in boot|boot_a|boot_b|rootfs_a|rootfs_b|data|userdata) ;; *) continue ;; esac
        [[ -z ${DEV[$part]:-} ]] || { fail -5 '系统磁盘存在重复分区标签'; return; }
        DEV[$part]=$device
    done < <(lsblk -rnp -o NAME,PKNAME,TYPE)
    for part in boot boot_a boot_b rootfs_a rootfs_b; do
        [[ -n ${DEV[$part]:-} ]] || { fail -5 '同一系统盘缺少 A/B 或全局 boot 分区'; return; }
    done
    boot_source=$(findmnt -n -o SOURCE --mountpoint /boot) && boot_source=$(readlink -f "$boot_source") || { fail -5 '/boot 未挂载'; return; }
    [[ $source == "${DEV[rootfs_$CURRENT_SLOT]}" && $boot_source == "${DEV[boot_$CURRENT_SLOT]}" ]] || { fail -5 'boot 与 rootfs 当前槽不一致'; return; }
    if [[ -d /userdata ]]; then
        data_source=$(findmnt -n -o SOURCE --mountpoint /userdata) && data_source=$(readlink -f "$data_source") || { fail -5 '/userdata 没有挂载'; return; }
        [[ $data_source == "${DEV[data]:-}" || $data_source == "${DEV[userdata]:-}" ]] || { fail -5 '/userdata 不属于系统 A/B 磁盘'; return; }
    fi
}
# @brief 拒绝已挂载或挂载查询失败的目标，不卸载当前业务文件系统。
unmounted() {
    local status
    findmnt -rn -S "$1" >/dev/null 2>&1; status=$?
    ((status==1)) || { fail -5 '目标分区已挂载或无法确认挂载状态'; return; }
}
# @return 块设备容量（字节）。
device_size() { blockdev --getsize64 "$1"; }
# @brief 规范化归档路径，只允许根或一个一级目录；拒绝路径跳转、控制字符和 unzip 通配符。
# @param $1 归档中的原始路径。@return 输出规范路径，归档根目录输出 .；非法返回 1。
archive_path() {
    local name=$1 component
    [[ -n $name && $name != /* && $name != *\\* && $name != *:* && $name != *[[:cntrl:]]* ]] || return 1
    case $name in *'*'*|*'?'*|*'['*|*']'*) return 1 ;; esac
    while [[ $name == ./* ]]; do name=${name#./}; done
    [[ -n $name && $name != . ]] || { printf '.\n'; return 0; }
    [[ $name != */ ]] || name=${name%/}
    [[ -n $name && $name != -* && $name != .. && $name != . ]] || return 1
    if [[ $name == */* ]]; then
        component=${name%%/*}; local leaf=${name#*/}
        [[ -n $component && $component != . && $component != .. && -n $leaf && $leaf != . && $leaf != .. && $leaf != */* ]] || return 1
    fi
    printf '%s\n' "$name"
}
# @brief 根目录或统一一级目录内只允许固定镜像和元数据；输出统一的叶文件名。
# @param $1 待验证路径。@return 合法时输出固定叶文件名；否则返回 1。
archive_name() {
    local name; name=$(archive_path "$1") || return 1; name=${name##*/}
    case $name in boot_a.img|boot_b.img|rootfs_a.img|rootfs_b.img|package.ini|MD5SUMS) printf '%s\n' "$name" ;; *) return 1 ;; esac
}
# @brief 扫描完整归档后确定根/统一一级目录布局；记录原始条目名，展开仍写受控平铺目录。
# @param $1 压缩包路径。$2 格式 zip 或 tar.gz。
# @return 成功返回 0 并填充 ENTRY_NAME/ENTRY_SIZE；失败设置协议错误并返回非零。
scan_archive() {
    local archive=$1 format=$2 name normalized bytes mode count=0 raw path prefix='' selected=0 directory entry_prefix
    declare -gA ENTRY_NAME=() ENTRY_SIZE=()
    local -A listed=() seen=() directories=()
    if [[ $format == zip ]]; then
        run_job '' 0 0 0 "$WORK/archive.list" unzip -Z -l "$archive" || { fail -5 'ZIP 目录读取失败'; return; }
        # 第一标志 B/T 表示加密；-/x/l/X 是扩展字段和数据描述符。目录只用于布局识别。
        if awk '
            $1 ~ /^[-?dlcbps]/ {
                count++; kind=substr($1,1,1)
                if($4!~/^[0-9]+$/)exit 4
                if($5~/^[BT]/)exit 2
                if($5!~/^[bt][-xlX]$/)exit 4
                if(kind!="-" && kind!="d" && !(kind=="?" && length($1)==10 && substr($1,2)~/^[rwxstST-]+$/))exit 3
                name=$0; for(i=0;i<9;i++)sub(/^[[:space:]]*[^[:space:]]+[[:space:]]+/,"",name)
                type=(kind=="d" || name~/\/$/ || name=="." || name=="./")?"d":"f"
                print type "\t" $4 "\t" name
            }
            END {if(!count)exit 4}
        ' "$WORK/archive.list" >"$WORK/archive.entries"; then :
        else
            case $? in
                2) fail -5 'ZIP 包含加密条目，不支持密码保护的升级包' ;;
                3) fail -5 'ZIP 含链接或特殊文件' ;;
                *) fail -5 'ZIP 条目信息无法识别' ;;
            esac
            return 1
        fi
    else
        run_job '' 0 0 0 /dev/null gzip -t "$archive" || { fail -5 '压缩包完整性检查失败'; return; }
        run_job '' 0 0 0 "$WORK/archive.list" tar --numeric-owner --quoting-style=literal -tvzf "$archive" || { fail -5 'TAR 目录读取失败'; return; }
        awk '
            {kind=substr($1,1,1); if((kind!="-" && kind!="d") || $3!~/^[0-9]+$/)exit 1
             name=$0; for(i=0;i<5;i++)sub(/^[[:space:]]*[^[:space:]]+[[:space:]]+/,"",name)
             print (kind=="d"?"d":"f") "\t" $3 "\t" name; count++}
            END {if(!count)exit 1}
        ' "$WORK/archive.list" >"$WORK/archive.entries" || { fail -5 'TAR 含链接或特殊文件，或条目信息无效'; return; }
    fi
    # 独立名称列表与元数据逐项交叉检查，防止空白/换行和长列表解析歧义。
    if [[ $format == zip ]]; then unzip -Z -1 "$archive" >"$WORK/archive.names" 2>/dev/null
    else tar --quoting-style=literal -tzf "$archive" >"$WORK/archive.names" 2>/dev/null; fi || { fail -5 '归档文件列表无效'; return; }
    local names=0
    while IFS= read -r raw; do
        path=$(archive_path "$raw") || { fail -5 '压缩包包含不安全路径或多层目录'; return; }
        [[ -z ${listed[$path]:-} ]] || { fail -5 '压缩包包含重复路径'; return; }
        listed[$path]=1; ((names+=1)); ((names<=64)) || { fail -5 '升级包条目过多'; return; }
    done <"$WORK/archive.names"
    while IFS=$'\t' read -r mode bytes name; do
        ((count+=1)); ((count<=64)) || { fail -5 '升级包条目过多'; return; }
        path=$(archive_path "$name") || { fail -5 '压缩包路径无效'; return; }
        [[ -n ${listed[$path]:-} && -z ${seen[$path]:-} ]] || { fail -5 '归档名称与条目信息不一致或路径重复'; return; }
        seen[$path]=1
        if [[ $mode == d ]]; then
            [[ $bytes == 0 ]] || { fail -5 '归档目录条目含非空数据'; return; }
            [[ $path != . ]] || continue
            [[ $path != */* ]] || { fail -5 '升级包仅支持统一一级目录'; return; }
            directories[$path]=1; continue
        fi
        normalized=$(archive_name "$path") || { fail -5 '压缩包包含非约定文件名'; return; }
        entry_prefix=''; [[ $path != */* ]] || entry_prefix=${path%/*}
        if ((selected==0)); then prefix=$entry_prefix; selected=1
        else [[ $prefix == "$entry_prefix" ]] || { fail -5 '镜像和元数据必须位于根目录或同一个一级目录'; return; }; fi
        [[ -z ${ENTRY_NAME[$normalized]:-} ]] || { fail -5 '压缩包包含重复镜像或元数据'; return; }
        positive_long "$bytes" || { fail -5 '镜像或元数据长度无效'; return; }
        case $normalized in
            *.img)
                local capacity; capacity=$(device_size "${DEV[${normalized%.img}]}" ) || { fail -5 '目标分区容量不可读'; return; }
                ((bytes<=capacity)) || { fail -5 '镜像超过对应分区容量'; return; } ;;
            *) ((bytes<=65536)) || { fail -5 '升级元数据过大'; return; } ;;
        esac
        ENTRY_NAME[$normalized]=$name; ENTRY_SIZE[$normalized]=$bytes
    done <"$WORK/archive.entries"
    ((count==names)) || { fail -5 '归档文件名与条目元数据不一致'; return; }
    for directory in "${!directories[@]}"; do
        [[ -n $prefix && $directory == "$prefix" ]] || { fail -5 '压缩包包含与镜像布局无关的目录'; return; }
    done
    for name in boot_a.img boot_b.img rootfs_a.img rootfs_b.img; do
        [[ -n ${ENTRY_NAME[$name]:-} ]] || { fail -5 '缺少完整 A/B 镜像'; return; }
    done
}
# @brief 仅提取固定的目标槽文件；流 MD5、同步落盘后的文件 MD5 必须一致。
extract_entry() {
    local name=$1 archive=$2 format=$3 base=$4 span=$5 output=$TASK_DIR/images/$1 expected
    [[ ! -e $output && ! -L $output ]] || { fail -5 '提取目标已存在'; return; }
    run_job "$output" "${ENTRY_SIZE[$name]}" "$base" "$span" "$WORK/stream.md5" bash -o pipefail -c '
        if [[ $1 == zip ]]; then unzip -p "$2" "$3"; else tar -xzOf "$2" -- "$3"; fi |
            tee "$4" | md5sum
    ' ota-extract "$format" "$archive" "${ENTRY_NAME[$name]}" "$output" || { [[ -n $ERROR_MESSAGE ]] || fail -5 '升级包解压失败'; return 1; }
    [[ $(stat -c %s "$output") == "${ENTRY_SIZE[$name]}" ]] || { fail -5 '解压长度不一致'; return; }
    read -r expected _ <"$WORK/stream.md5"
    sync -f "$output" || { fail -5 '镜像文件同步失败'; return; }
    hash_file "$output" || return
    [[ $HASH == "$expected" ]] || { fail -5 '解压文件 MD5 回读不一致'; return; }
    case $name in *.img)
        state_update '.images[$name]={size:$size,md5:$md5}' --arg name "$name" --argjson size "${ENTRY_SIZE[$name]}" --arg md5 "$HASH" || return ;;
    esac
}
# @brief 按字面解析可选 package.ini；不 source、不执行包内代码。
ini_value() {
    awk -v wanted="$2" '
        function trim(s){gsub(/^[ \t\r]+|[ \t\r]+$/,"",s);return s}
        /^[ \t]*[;#]/ {next}
        /^[ \t]*\[/ {section=tolower(trim($0));next}
        section=="[package]" && index($0,"=") {
            p=index($0,"="); key=tolower(trim(substr($0,1,p-1)))
            if(key==tolower(wanted)){if(found++)exit 1;value=trim(substr($0,p+1))}
        }
        END{if(!found)exit 1;print value}
    ' "$1"
}
# @return 本机设备树 compatible/model，包括无厂商前缀别名；不读取发布标识文件。
board_models() {
    local file item
    for file in /proc/device-tree/model /proc/device-tree/compatible; do
        [[ -r $file ]] || continue
        while IFS= read -r item; do
            [[ -n $item ]] || continue; printf '%s\n' "$item"
            [[ $item == *,* ]] && printf '%s\n' "${item#*,}"
        done < <(tr '\000' '\n' <"$file"; printf '\n')
    done
}
# @brief 检查可选元数据和 MD5SUMS；缺失时仍使用已验证归档的流 MD5。
verify_metadata() {
    local file=$TASK_DIR/images/package.ini value name digest actual unused
    if [[ -f $file ]]; then
        [[ $(ini_value "$file" formatVersion) == 1 && $(ini_value "$file" imageFormat) == raw && $(ini_value "$file" version) == "$(printf '%s' "$STATE" | jq -r '.version')" ]] || { fail -5 '升级包版本或格式不匹配'; return; }
        value=$(ini_value "$file" architecture) || value=aarch64
        [[ $value == aarch64 ]] || { fail -5 '升级包架构不匹配'; return; }
        value=$(ini_value "$file" boardModel) || value=''
        [[ -z $value ]] || board_models | grep -Fxq -- "$value" || { fail -5 '升级包产品身份不匹配'; return; }
    fi
    file=$TASK_DIR/images/MD5SUMS
    if [[ -f $file ]]; then
        local -A seen=()
        while read -r digest name unused; do
            [[ -n $digest ]] || continue
            # MD5SUMS 可由 Windows 工具生成；只去掉行尾 CR。
            name=${name%$'\r'}; name=${name#\*}
            [[ $digest =~ ^[a-fA-F0-9]{32}$ && -z $unused ]] || { fail -5 'MD5SUMS 格式错误'; return; }
            case $name in boot_a.img|boot_b.img|rootfs_a.img|rootfs_b.img) ;; *) fail -5 'MD5SUMS 文件名错误'; return ;; esac
            [[ -z ${seen[$name]:-} ]] || { fail -5 'MD5SUMS 重复文件'; return; }; seen[$name]=${digest,,}
        done <"$file"
        for name in boot_$TARGET_SLOT.img rootfs_$TARGET_SLOT.img; do
            actual=$(printf '%s' "$STATE" | jq -r --arg name "$name" '.images[$name].md5')
            [[ ${seen[$name]:-} == "$actual" ]] || { fail -5 '选中镜像的 MD5SUMS 校验失败'; return; }
        done
    fi
}
# @brief 从 ext 镜像读取固定文件到 IMAGE_OUTPUT；限制大小，不挂载镜像。
image_read() {
    local image=$1 path=$2
    IMAGE_OUTPUT=$WORK/image-file
    run_job '' 0 0 0 "$IMAGE_OUTPUT" debugfs -R "cat $path" "$image" || { fail -5 '镜像文件读取失败'; return; }
    [[ -s $IMAGE_OUTPUT && $(stat -c %s "$IMAGE_OUTPUT") -le 2097152 ]] || { fail -5 '镜像缺少必需运行文件'; return; }
}
# @brief 独立的 fstab 静态匹配辅助；仅显式调用时检查，OTA 预检不使用此规则。
fstab_binding() {
    local fstab=$1 mountpoint=$2 key=$3 uuid
    local -a sources=(); mapfile -t sources < <(awk -v target="$mountpoint" '!/^[ \t]*#/ && $2==target {print $1}' "$fstab")
    ((${#sources[@]}==1)) || return 1
    uuid=$(blkid -s PARTUUID -o value "${DEV[$key]}") || return 1
    [[ ${sources[0]} == "PARTLABEL=$key" || ${sources[0]} == "PARTUUID=$uuid" || ${sources[0]} == "${DEV[$key]}" ]]
}
# @brief 独立的 boot.scr 静态匹配辅助；OTA 预检不使用此规则，允许 BSP 通过变量选择根分区。
boot_binding() {
    local file=$1 slot=$2 uuid value count=0
    local pattern="(root=[\"']?|setenv[[:space:]]+rootdev[[:space:]]+[\"']?)(PARTLABEL=[A-Za-z0-9_-]+|PARTUUID=[A-Za-z0-9_-]+|/dev/[A-Za-z0-9_/-]+)"
    uuid=$(blkid -s PARTUUID -o value "${DEV[rootfs_$slot]}") || return 1
    while IFS= read -r value; do
        ((count+=1))
        [[ $value == "PARTLABEL=rootfs_$slot" || $value == "PARTUUID=$uuid" || $value == "${DEV[rootfs_$slot]}" ]] || return 1
    done < <(grep -aEo "$pattern" "$file" | sed -E "s/^root=[\"']?//;s/^setenv[[:space:]]+rootdev[[:space:]]+[\"']?//")
    ((count>0))
}
# @brief 校验 raw ext2/ext4、架构和独立健康入口；BSP 决定启动及挂载，不从镜像配置静态推断槽绑定。
# @note 不读取镜像 board-model 或版本文件；版本由平台指定，实际槽位及挂载仍由 discover_partitions 核对。
verify_rootfs() {
    local root=$TASK_DIR/images/rootfs_$TARGET_SLOT.img boot=$TASK_DIR/images/boot_$TARGET_SLOT.img file value
    for file in "$root" "$boot"; do
        [[ $(od -An -tx1 -j1080 -N2 "$file" | tr -d ' \n') == 53ef ]] || { fail -5 '仅支持原始 ext2/ext4 镜像'; return; }
    done
    image_read "$root" /usr/bin/dash || image_read "$root" /bin/dash || return
    value=$(od -An -tx1 -N20 "$IMAGE_OUTPUT" | tr -d ' \n')
    [[ $value == 7f454c460201* && ${value:36:4} == b700 ]] || { fail -5 'rootfs 不是 AArch64 小端系统'; return; }
    # LubanCat BSP 根据全局 boot 槽标记、启动参数及启动流程选择系统与挂载。
    # 镜像可使用 /dev/root、缺省 /boot 和 /userdata 项，boot.scr 也可动态拼接 root=。
    # 不读取或改写这些配置；刷写前和重启后的真实分区/挂载核对沿用原流程。
    image_read "$root" "$SCRIPT" || return
    grep -q '^#!/bin/bash' "$IMAGE_OUTPUT" && grep -q -- '--ota-health' "$IMAGE_OUTPUT" || { fail -5 'rootfs 未包含 Shell OTA 执行器'; return; }
    image_read "$root" /etc/systemd/system/face-media-ota-health.service || return
    grep -q -- '--ota-health' "$IMAGE_OUTPUT" || { fail -5 'rootfs 缺少独立健康服务'; return; }
    debugfs -R 'stat /etc/systemd/system/multi-user.target.wants/face-media-ota-health.service' "$root" 2>/dev/null | grep -q 'Type: symlink' || { fail -5 'rootfs 未启用 OTA 健康服务'; return; }
    for file in /usr/bin/jq /usr/bin/sqlite3; do
        debugfs -R "stat $file" "$root" 2>/dev/null | grep -Eq 'Type: (regular|symlink)' || { fail -5 'rootfs 缺少 OTA 所需 jq/sqlite3 命令'; return; }
    done
    ERROR_MESSAGE=''
}

# @brief 规范化平台请求，保留 taskId 原始十进制；FTP 只留在本 Shell 内存。
# @note FTP 字符串按码点排除 NUL(0)、LF(10)、CR(13)，避免正则转义误拒绝普通 u/0。
read_request() {
    local raw id
    raw=$(head -c 65537); ((${#raw}<=65536)) || { fail -5 'OTA 请求过大'; return; }
    id=$(printf '%s' "$raw" | raw_task_id) && positive_long "$id" || { fail -5 'OTA taskId 无效'; return; }
    REQUEST=$(printf '%s' "$raw" | jq -ce --arg id "$id" '
        .taskId=$id |
        .policy=(.policy//{}) | .policy.timeoutSec=(.policy.timeoutSec//1800) | .policy.retryMax=(.policy.retryMax//2) |
        select(type=="object" and (.action=="upgrade" or .action=="rollback") and
          (.version|type=="string" and test("^[A-Za-z0-9][A-Za-z0-9._+-]{0,95}$")) and
          (.md5|type=="string" and test("^[a-fA-F0-9]{32}$")) and
          (.size|type=="number" and floor==. and .>0) and (.ftp|type=="object") and
          ([.ftp.host,.ftp.user,.ftp.pwd,.ftp.root,.ftp.file]|all(.[];type=="string" and length>0 and length<=2048 and (explode|all(.[];.!=0 and .!=10 and .!=13)))) and
          (.ftp.host|test("^[A-Za-z0-9.:_-]+$")) and
          (.ftp.port|type=="number" and floor==. and .>=1 and .<=65535) and
          .ftp.root=="." and (.ftp.file|test("[/\\\\]")|not) and
          (.ftp.file|ascii_downcase|test("\\.(zip|tar\\.gz)$")) and
          (.policy.timeoutSec|type=="number" and floor==. and .>=1 and .<=2147483647) and
          (.policy.retryMax|type=="number" and floor==. and .>=0 and .<=5)) |
        .md5|=ascii_downcase | .policy={timeoutSec:.policy.timeoutSec,retryMax:.policy.retryMax}
    ' 2>/dev/null) || { fail -5 'OTA 请求字段无效'; return; }
}
# @return 数字点分版本的降级判断；无法排序的发布标识不猜测顺序。
is_downgrade() {
    [[ $1 =~ ^[0-9]+(\.[0-9]+)*$ && $2 =~ ^[0-9]+(\.[0-9]+)*$ ]] || return 1
    local -a old=() next=(); local a b i length
    IFS=. read -ra old <<<"$1"; IFS=. read -ra next <<<"$2"
    length=${#old[@]}; ((${#next[@]}>length)) && length=${#next[@]}
    for ((i=0;i<length;i++)); do
        a=${old[i]:-0}; b=${next[i]:-0}
        while [[ $a == 0* && ${#a} -gt 1 ]]; do a=${a#0}; done
        while [[ $b == 0* && ${#b} -gt 1 ]]; do b=${b#0}; done
        ((${#b}<${#a})) && return 0; ((${#b}>${#a})) && return 1
        [[ $b < $a ]] && return 0; [[ $b > $a ]] && return 1
    done
    return 1
}
# @brief curl stdin 配置转义；不把用户和密码放在 argv、环境或磁盘中。
curl_escape() { CURL_ESC=$1; CURL_ESC=${CURL_ESC//\\/\\\\}; CURL_ESC=${CURL_ESC//\"/\\\"}; }
# @brief 对 FTP 包名作逐字节 URL 编码，保持 UTF-8 文件名。
url_encode() {
    local value=$1 char code i; URL_ENCODED=''
    for ((i=0;i<${#value};i++)); do
        char=${value:i:1}
        case $char in [A-Za-z0-9._~-]) URL_ENCODED+=$char ;; *) printf -v code '%%%02X' "'$char"; URL_ENCODED+=$code ;; esac
    done
}
# @brief 按 curl 配置语法生成当前一次性凭据，调用处仅将输出接到 stdin。
ftp_config() {
    local host=$FTP_HOST url
    [[ $host == *:* ]] && host="[$host]"
    url_encode "$FTP_FILE"; url="ftp://$host:$FTP_PORT/$URL_ENCODED"
    curl_escape "$url"; printf 'url = "%s"\n' "$CURL_ESC"
    curl_escape "$FTP_USER:$FTP_PASSWORD"; printf 'user = "%s"\n' "$CURL_ESC"
}
# @brief PASV/BINARY 下载；下载中校验空间、取消及预算，完成后核对精确字节数。
download_package() {
    local expected=$1 remaining=$2 part=$TASK_DIR/package.part
    local -a fields=(); mapfile -t fields < <(printf '%s' "$REQUEST" | jq -r '.ftp|.host,(.port|tostring),.user,.pwd,.file')
    FTP_HOST=${fields[0]}; FTP_PORT=${fields[1]}; FTP_USER=${fields[2]}; FTP_PASSWORD=${fields[3]}; FTP_FILE=${fields[4]}
    check_space "$TASK_DIR" "$expected" || return
    progress downloading 5 downloading || return
    if ! run_job "$part" "$expected" 5 50 /dev/null curl --config - --ftp-pasv --silent --show-error --fail --connect-timeout 20 --max-time "$remaining" --max-filesize "$expected" --output "$part" < <(ftp_config); then
        unset FTP_PASSWORD FTP_USER fields REQUEST
        [[ -n $ERROR_MESSAGE ]] && return 1
        case $JOB_RC in 5|6|7|28|52|55|56) fail -2 'FTP 网络不可达或连接中断' ;; 23) fail -3 'FTP 下载文件写入失败或空间不足' ;; 63) fail -5 'FTP 文件超过声明大小' ;; *) fail -1 'FTP 下载失败' ;; esac
        return
    fi
    unset FTP_PASSWORD FTP_USER fields REQUEST
    [[ $(stat -c %s "$part") == "$expected" ]] || { fail -1 'FTP 文件未完整下载'; return; }
    sync -f "$part" && mv -f -- "$part" "$TASK_DIR/package.bin" || { fail -3 '升级包保存失败'; return; }
    progress downloading 55 downloading 100
}
# @brief 一次下发对应一次准备，平台重试必须使用新通知的凭据，不自行循环重试。
ota_prepare() {
    local started=$(mono_now) prior='' prior_id='' prior_phase='' prior_result='' prior_code=0 status current id timeout archive_type size md5 action version percent=0 attempt=1 md5_failures=0 name total remaining
    read_request || return
    id=$(printf '%s' "$REQUEST" | jq -r '.taskId')
    ensure_shared || return; acquire_lock "$BASE/executor.lock" EXEC_FD || return
    if load_active; then
        prior=$STATE; prior_id=$TASK_ID; prior_phase=$(printf '%s' "$prior" | jq -r '.phase'); TASK_READY=0; TASK_ID=''; TASK_DIR=''
        [[ $prior_phase == finished ]] || { fail -5 '已有 OTA 任务正在执行'; return; }
        if [[ $prior_id == "$id" ]]; then
            printf '%s' "$REQUEST" | jq -e --argjson prior "$prior" '.version==$prior.version and .md5==$prior.md5 and .action==$prior.action and .size==$prior.size' >/dev/null || { fail -5 '同一任务不能更换固件'; return; }
            prior_result=$(printf '%s' "$prior" | jq -r '.result'); prior_code=$(printf '%s' "$prior" | jq -r '.resultCode')
            [[ $prior_result == failed && ( $prior_code == -1 || $prior_code == -2 || $prior_code == -4 ) ]] || { fail -5 '该任务不能重复安装'; return; }
            percent=$(printf '%s' "$prior" | jq -r '.percent//0'); attempt=$(printf '%s' "$prior" | jq -r '(.attempt//0)+1'); md5_failures=$(printf '%s' "$prior" | jq -r '.md5Failures//0')
            ((md5_failures<2)) || { fail -4 '压缩包 MD5 已达到最多一次重试'; return; }
        fi
    else status=$?; ((status==2)) || return 1; fi
    cleanup_finished_tasks || return; discover_partitions || return
    # 首次接入可以没有登记版本；不借用目标版本或平台 fromVersion 冒充旧版本。
    current=$(real_version) || current=''
    action=$(printf '%s' "$REQUEST" | jq -r '.action'); version=$(printf '%s' "$REQUEST" | jq -r '.version')
    if [[ $action == upgrade ]] && is_downgrade "$current" "$version"; then fail -8 '普通升级任务不能降级，请使用 rollback'; return; fi
    systemctl is-enabled face-media-ota-health.service >/dev/null 2>&1 || { fail -5 '请先安装并启用 OTA 健康服务'; return; }
    timeout=$(printf '%s' "$REQUEST" | jq -r '.policy.timeoutSec'); size=$(printf '%s' "$REQUEST" | jq -r '.size'); md5=$(printf '%s' "$REQUEST" | jq -r '.md5')
    archive_type=$(printf '%s' "$REQUEST" | jq -r 'if (.ftp.file|ascii_downcase|endswith(".zip")) then "zip" else "tar.gz" end')
    TASK_ID=$id; TASK_DIR=$BASE/tasks/$id; mkdir -p -- "$TASK_DIR" || { fail -3 '任务目录不可写'; return; }
    cleanup_artifacts "$TASK_DIR" || return
    STATE=$(printf '%s' "$REQUEST" | jq -c --arg old "$current" --arg from "$CURRENT_SLOT" --arg to "$TARGET_SLOT" --arg boot "$(boot_id)" --arg type "$archive_type" --argjson pid "$$" --argjson deadline "$((started+timeout))" --argjson percent "$percent" --argjson attempt "$attempt" --argjson failures "$md5_failures" '{taskId,version,md5,action,size,policy,notificationId:(.notificationId//""),deviceId:(.deviceId//"")}|.oldVersion=$old|.fromSlot=$from|.toSlot=$to|.beforeBootId=$boot|.archiveType=$type|.preparePid=$pid|.deadlineMonotonic=$deadline|.percent=$percent|.attempt=$attempt|.md5Failures=$failures|.phase="received"|.state="received"|.writeStarted=false|.images={}') || { fail -5 '无法初始化任务'; return; }
    TASK_READY=1; progress received 0 received || return; prepare_guard || return
    remaining=$((started+timeout-$(mono_now))); ((remaining>0)) || { fail -1 '任务预算已耗尽'; return; }
    download_package "$size" "$remaining" || return
    progress verifying 60 verifying || return; hash_file "$TASK_DIR/package.bin" || return
    [[ $HASH == "$md5" ]] || { fail -4 '升级压缩包 MD5 不匹配'; return; }
    scan_archive "$TASK_DIR/package.bin" "$archive_type" || return
    mkdir -p -- "$TASK_DIR/images" || { fail -3 '镜像目录不可写'; return; }
    total=$((ENTRY_SIZE[boot_$TARGET_SLOT.img]+ENTRY_SIZE[rootfs_$TARGET_SLOT.img])); check_space "$TASK_DIR/images" "$total" || return
    progress verifying 65 extracting || return
    extract_entry "boot_$TARGET_SLOT.img" "$TASK_DIR/package.bin" "$archive_type" 65 5 || return
    extract_entry "rootfs_$TARGET_SLOT.img" "$TASK_DIR/package.bin" "$archive_type" 70 10 || return
    for name in package.ini MD5SUMS; do [[ -n ${ENTRY_NAME[$name]:-} ]] && { extract_entry "$name" "$TASK_DIR/package.bin" "$archive_type" 80 0 || return; }; done
    verify_metadata || return; verify_rootfs || return; prepare_guard || return
    remaining=$((started+timeout-$(mono_now))); ((remaining>=HEALTH_SECONDS+60)) || { fail -1 '剩余预算不足以完成写入和启动确认'; return; }
    state_update '.remainingSec=$remaining' --argjson remaining "$remaining" || return
    progress verifying 85 prepared || return; state_save || return
    printf '{"prepared":true}\n'
}
# @brief 仅挂载同盘小型全局 boot 分区到私有路径，写槽标记后同步并卸载。
select_slot() {
    local slot=$1 other=a
    [[ $slot == a || $slot == b ]] || { fail -5 '槽位无效'; return; }; [[ $slot == a ]] && other=b
    unmounted "${DEV[boot]}" || return
    GLOBAL_MOUNT=$WORK/global-boot; mkdir -p -- "$GLOBAL_MOUNT" || { fail -5 '临时挂载目录创建失败'; return; }
    mount -o rw "${DEV[boot]}" "$GLOBAL_MOUNT" >/dev/null 2>&1 || { fail -5 '全局 boot 分区挂载失败'; return; }
    [[ -f $GLOBAL_MOUNT/boot.scr ]] || { fail -5 '全局 boot 分区缺少 BSP 引导脚本'; return; }
    touch "$GLOBAL_MOUNT/boot_$slot" && sync -f "$GLOBAL_MOUNT" && rm -f -- "$GLOBAL_MOUNT/boot_$other" && sync -f "$GLOBAL_MOUNT" || { fail -5 '槽标记同步失败'; return; }
    umount "$GLOBAL_MOUNT" >/dev/null 2>&1 || { fail -5 '全局 boot 分区卸载失败'; return; }; GLOBAL_MOUNT=''
}
# @brief 块设备类型检查，测试可在 source 后替换硬件边界。
is_block_device() { [[ -b $1 ]]; }
# @brief 沿用原脚本 dd 刷写；源 MD5 复核、fsync、按镜像字节数回读后才允许切槽。
write_image() {
    local image=$1 device=$2 size=$3 expected=$4 base=$5 capacity
    unmounted "$device" || return
    is_block_device "$device" && [[ -f $image && ! -L $image && $(stat -c %s "$image") == "$size" ]] || { fail -5 '镜像或目标块设备无效'; return; }
    capacity=$(device_size "$device") || return 1; ((size<=capacity)) || { fail -5 '镜像超过分区容量'; return; }
    hash_file "$image" || return; [[ $HASH == "$expected" ]] || { fail -5 '待写镜像已变化'; return; }
    run_job @io "$size" "$base" 5 /dev/null dd if="$image" of="$device" bs=1M conv=notrunc,fsync status=none || { fail -5 '目标镜像写入失败'; return; }
    blockdev --flushbufs "$device" >/dev/null 2>&1 || { fail -5 '分区缓存刷新失败'; return; }
    hash_file "$device" "$size" || return; [[ $HASH == "$expected" ]] || { fail -5 '分区回读 MD5 不一致'; return; }
    progress installing "$((base+5))" installing
}
# @brief 异步请求系统重启；请求失败不能声称升级完成。
request_reboot() { systemctl --no-block reboot >/dev/null 2>&1 || { fail -5 '系统拒绝重启请求'; return; }; }
# @brief 独立安装入口；任务及启动代次复核、取消门和资源锁均在写盘前完成。
ota_install() {
    local wanted=$1 phase from before deadline name size md5 index=0
    positive_long "$wanted" || { fail -5 '任务 ID 无效'; return; }
    ensure_shared || return; acquire_lock "$BASE/executor.lock" EXEC_FD || return; load_active || return
    phase=$(printf '%s' "$STATE" | jq -r '.phase')
    [[ $TASK_ID == "$wanted" && $phase == prepared ]] || { TASK_READY=0; fail -5 '没有对应的已准备 OTA 任务'; return; }
    discover_partitions || return
    from=$(printf '%s' "$STATE" | jq -r '.fromSlot'); before=$(printf '%s' "$STATE" | jq -r '.beforeBootId')
    [[ $from == "$CURRENT_SLOT" && $before == "$(boot_id)" ]] || { fail -5 '准备后启动槽或启动代次发生变化'; return; }
    deadline=$(printf '%s' "$STATE" | jq -r '.deadlineMonotonic // 0 | floor')
    ((deadline==0 || deadline-$(mono_now)>=HEALTH_SECONDS+60)) || { fail -1 '安装前预算不足，等待平台重新下发'; return; }
    acquire_lock /userdata/mnt/UDISK/res/static/demoResources/images/.ycest-update.lock RESOURCE_FD || return
    acquire_lock "$TASK_DIR/.ota-cancel.lock" GATE_FD || return
    [[ ! -e $TASK_DIR/cancel ]] || { fail -9 '安装开始前已取消升级'; return; }
    state_update '.writeStarted=true' || return; progress installing 86 installing || return
    release_lock "$GATE_FD"; GATE_FD=''
    for name in boot_$TARGET_SLOT.img rootfs_$TARGET_SLOT.img; do
        size=$(printf '%s' "$STATE" | jq -r --arg name "$name" '.images[$name].size'); md5=$(printf '%s' "$STATE" | jq -r --arg name "$name" '.images[$name].md5')
        positive_long "$size" && [[ $md5 =~ ^[a-f0-9]{32}$ ]] || { fail -5 '任务镜像记录无效'; return; }
        write_image "$TASK_DIR/images/$name" "${DEV[${name%.img}]}" "$size" "$md5" "$((86+index*5))" || return
        ((index+=1))
    done
    state_update '.trials=0' || return; progress rebooting 99 awaiting_boot || return
    if ! select_slot "$TARGET_SLOT" || ! request_reboot; then
        local saved_code=$ERROR_CODE saved_message=$ERROR_MESSAGE
        if select_slot "$CURRENT_SLOT"; then state_update '.phase="installing"' || return; ERROR_CODE=$saved_code; ERROR_MESSAGE=$saved_message; return 1; fi
        state_update '.phase="recovering"|.errorMsg="切槽标记恢复失败，等待重启后确认实际分区"' || return
        state_save || return; request_reboot; return 1
    fi
}
# @brief 限时只读检查共享主库；SQLite 批处理用单个 SQL 参数，兼容板载 CLI。
# @return quick_check 为 ok 且 network_person 表存在时成功，不执行任何写入 SQL。
database_ready() {
    local check
    check=$(timeout 3 sqlite3 -readonly -cmd '.timeout 2000' /userdata/home/cat/face_media/access_control.db "PRAGMA quick_check(1); SELECT count(*) FROM sqlite_master WHERE type='table' AND name='network_person';" 2>/dev/null) || return 1
    [[ $check == $'ok\n1' ]]
}
# @brief 观察 Qt PID/心跳和只读数据库；非空 wanted 仅用于核对已知的旧槽版本。
# @param $1 可选已登记版本；候选槽和未登记旧槽传空，不依赖镜像中的版本文本。
qt_ready() {
    local wanted=${1:-} pid sequence timestamp token now
    [[ -z $wanted || $(real_version) == "$wanted" ]] || return 1
    read -r pid sequence timestamp </run/face-media/heartbeat 2>/dev/null || return 1
    [[ $pid =~ ^[0-9]+$ && $sequence =~ ^[0-9]+$ && $pid -gt 1 && $sequence -ge 1 && -d /proc/$pid ]] || return 1
    [[ $(cat /run/face-media/app.pid 2>/dev/null) == "$pid" ]] || return 1
    token=$pid:$sequence; now=$(mono_now)
    if [[ $token != "$HEARTBEAT_TOKEN" ]]; then HEARTBEAT_TOKEN=$token; HEARTBEAT_SEEN=$now; fi
    ((now-HEARTBEAT_SEEN<=15)) || return 1
    database_ready
}
# @brief 最长 180 秒等待连续 30 秒健康；候选槽传空版本，成功后才记录平台版本。
observe_health() {
    local version=${1:-} deadline=$(($(mono_now)+HEALTH_SECONDS)) stable=-1 now
    while (($(mono_now)<deadline)); do
        now=$(mono_now)
        if qt_ready "$version"; then ((stable<0)) && stable=$now; ((now-stable>=STABLE_SECONDS)) && return 0
        else stable=-1; fi
        sleep 2
    done
    return 1
}
# @brief 只在不同启动代次的终态写当前槽 journal；不输出下载/刷写阶段日志。
journal_outcome() {
    local current=$(boot_id) before token result data
    before=$(printf '%s' "$STATE" | jq -r '.beforeBootId//""'); [[ $current != "$before" ]] || return 0
    result=$(printf '%s' "$STATE" | jq -r '.result//""'); token=$current:$result
    [[ $(printf '%s' "$STATE" | jq -r '.journalOutcome//""') != "$token" ]] || return 0
    data=$(printf '%s' "$STATE" | jq -c '{taskId,result,resultCode,fromSlot,toSlot,version,actualVersion,errorMsg}')
    local priority=err; [[ $result == success ]] && priority=info
    if printf '%s\n' "$data" | timeout 10 systemd-cat -t face-media-ota -p "$priority"; then state_update '.journalOutcome=$token' --arg token "$token" && state_save; fi
    return 0
}
# @brief 仅在本次候选槽健康通过后的 confirming 阶段记录平台版本，不改旧槽。
# @param $1 本任务已验证格式的目标版本。@return 成功为 0；失败为 1 并设置 ERROR_CODE=-6，保留恢复来源。
record_confirmed_version() {
    local value=$1 parent=${VERSION_FILE%/*} temporary phase before target
    phase=$(printf '%s' "$STATE" | jq -r '.phase'); before=$(printf '%s' "$STATE" | jq -r '.beforeBootId')
    target=$(printf '%s' "$STATE" | jq -r '.toSlot')
    [[ $OTA_MODE == health && $phase == confirming && $CURRENT_SLOT == "$target" && $(boot_id) != "$before" ]] &&
        valid_version "$value" && [[ $value == "$(printf '%s' "$STATE" | jq -r '.version')" ]] || { fail -6 '版本记录时机或目标槽不匹配'; return; }
    [[ ! -L $parent && ! -L $VERSION_FILE && ( ! -e $VERSION_FILE || -f $VERSION_FILE ) ]] || { fail -6 '版本记录路径无效'; return; }
    mkdir -p -- "$parent" || { fail -6 '版本目录不可写'; return; }
    temporary=$(mktemp "$parent/.firmware-version.XXXXXXXX") || { fail -6 '版本临时文件创建失败'; return; }
    if ! printf '%s\n' "$value" >"$temporary" || ! chmod 0644 "$temporary" || ! sync -f "$temporary" ||
        ! mv -fT -- "$temporary" "$VERSION_FILE" || ! sync -f "$parent"; then
        rm -f -- "$temporary"; fail -6 '版本文件保存或同步失败'; return
    fi
}

# @brief 在实际启动槽保存结果及已登记版本；未登记留空，不用目标或旧任务版本推断。
finish_boot() {
    local recorded
    recorded=$(real_version) || recorded=''
    if [[ $1 == success ]]; then
        [[ -n $recorded && $recorded == "$(printf '%s' "$STATE" | jq -r '.version')" ]] || { fail -6 '确认成功前尚未保存目标版本'; return; }
    fi
    state_update '.phase="finished"|.result=$result|.resultCode=$code|.errorMsg=$message|.actualVersion=$version|.outcomeBootId=$boot|if $result=="success" then .percent=100 else . end' --arg result "$1" --argjson code "$2" --arg message "$3" --arg version "$recorded" --arg boot "$(boot_id)" || return
    state_save || return; journal_outcome; cleanup_artifacts "$TASK_DIR"
}
# @brief 开机独立检查；先预设回旧槽，健康后提交新槽；Qt 无法启动时仍能回退。
# @note 健康服务之前的坏内核/根挂载失败仍依赖 BSP 的引导计数和看门狗。
ota_health() {
    local status phase before from to current=$(boot_id) version trials last reason
    ensure_shared || return; acquire_lock "$BASE/executor.lock" EXEC_FD || return
    if ! load_active; then status=$?; [[ -z $ERROR_MESSAGE ]] && return 0; return 1; fi
    phase=$(printf '%s' "$STATE" | jq -r '.phase')
    if [[ $phase == finished ]]; then
        [[ $(printf '%s' "$STATE" | jq -r '.outcomeBootId//""') == "$current" ]] && journal_outcome
        cleanup_artifacts "$TASK_DIR"; return
    fi
    before=$(printf '%s' "$STATE" | jq -r '.beforeBootId'); [[ $before != "$current" ]] || return 0
    discover_partitions || return
    case $phase in received|downloading|verifying|extracting|prepared|installing) finish_failure -5 'OTA 在切槽前因重启中断'; return ;; esac
    from=$(printf '%s' "$STATE" | jq -r '.fromSlot'); to=$(printf '%s' "$STATE" | jq -r '.toSlot')
    if [[ $CURRENT_SLOT == "$from" ]]; then
        version=$(printf '%s' "$STATE" | jq -r '.oldVersion//""')
        reason=$(printf '%s' "$STATE" | jq -r '.recoveryReason//""')
        [[ -z $reason ]] || reason="：$reason"
        if observe_health "$version"; then select_slot "$CURRENT_SLOT" && finish_boot rolled_back -7 "新固件未确认，已恢复并确认原分区$reason"
        else finish_boot failed -6 '回退分区启动后健康检查未通过'; fi
        return
    fi
    [[ $CURRENT_SLOT == "$to" ]] || { finish_boot failed -6 '启动槽与任务记录不一致'; return; }
    if [[ $phase == recovering ]]; then select_slot "$from" && request_reboot; return; fi
    trials=$(printf '%s' "$STATE" | jq -r '.trials//0'); last=$(printf '%s' "$STATE" | jq -r '.lastTrialBootId//""')
    [[ $last != "$current" ]] && ((trials+=1))
    state_update '.trials=$trials|.lastTrialBootId=$boot|.phase="trial"' --argjson trials "$trials" --arg boot "$current" || return
    state_save || return; select_slot "$from" || return
    version=$(printf '%s' "$STATE" | jq -r '.version')
    if ((trials<=MAX_TRIALS)) && observe_health ''; then
        state_update '.phase="confirming"' && state_save || return
        if record_confirmed_version "$version"; then
            select_slot "$to" && finish_boot success 0 ''
        else
            reason=$ERROR_MESSAGE
            state_update '.phase="recovering"|.recoveryReason=$reason|.errorMsg=$reason' --arg reason "$reason" && state_save && request_reboot
        fi
    else
        state_update '.phase="recovering"' && state_save && request_reboot
    fi
}
# @brief 安装并启用独立健康单元；本 Shell 文件不再调用其他语言执行器。
install_support() {
    cat >/etc/systemd/system/face-media-ota-health.service <<'OTA_HEALTH_UNIT'
# @author Du lin
# @date 2026-11-18
[Unit]
Description=Face Media OTA boot health and recovery
After=local-fs.target
Before=face-media.service
ConditionPathIsMountPoint=/userdata
ConditionPathExists=/userdata/var/lib/face-media/ota/active.json
[Service]
Type=simple
ExecStart=/usr/local/sbin/update_ab_system.sh --ota-health
Restart=on-failure
RestartSec=5
StandardOutput=null
StandardError=null
TimeoutStopSec=10
[Install]
WantedBy=multi-user.target
OTA_HEALTH_UNIT
    systemctl daemon-reload && systemctl enable face-media-ota-health.service >/dev/null 2>&1 || { fail -5 '健康服务安装失败'; return; }
}
# @brief 取消或 Qt 退出时先终止作业；进入写入后只保留安全恢复状态。
on_signal() {
    if [[ $OTA_MODE == prepare && -e $TASK_DIR/cancel ]]; then ERROR_CODE=-9; ERROR_MESSAGE='平台已取消升级'
    elif [[ $OTA_MODE == prepare ]]; then ERROR_CODE=-1; ERROR_MESSAGE='OTA 准备进程已中断'
    else ERROR_CODE=-5; ERROR_MESSAGE='OTA 执行进程被中断'; fi
    stop_job; exit 1
}
# @brief EXIT 清理只作用于本次作业目录；失败终态不覆盖不确定的已切槽状态。
ota_cleanup() {
    local status=$1 phase=''
    trap - EXIT TERM INT HUP
    stop_job
    [[ -z $GLOBAL_MOUNT ]] || umount "$GLOBAL_MOUNT" >/dev/null 2>&1 || true
    if ((status!=0 && TASK_READY)); then
        phase=$(printf '%s' "$STATE" | jq -r '.phase//""' 2>/dev/null)
        if [[ $OTA_MODE == prepare || ( $OTA_MODE == install && $phase != awaiting_boot && $phase != recovering ) ]]; then
            finish_failure "$ERROR_CODE" "${ERROR_MESSAGE:-OTA 执行失败}" || true
        fi
    fi
    if ((status!=0)); then
        if command -v jq >/dev/null 2>&1; then jq -cn --argjson code "$ERROR_CODE" --arg message "${ERROR_MESSAGE:-OTA 执行失败}" '{result:"failed",resultCode:$code,errorMsg:$message}'
        else printf '{"result":"failed","resultCode":-5,"errorMsg":"OTA 需要 jq 命令"}\n'; fi
    fi
    unset REQUEST FTP_PASSWORD FTP_USER
    release_lock "$GATE_FD"; release_lock "$RESOURCE_FD"; release_lock "$EXEC_FD"
    if [[ -n $WORK && $WORK == "$RUNTIME"/work.* && ! -L $WORK ]]; then rm -rf -- "$WORK"; fi
    return "$status"
}
# @brief Qt/systemd 三个 OTA 入口；仅按需运行，结束即退出，凭据不进入命令行。
ota_entry() {
    set +x; set -o pipefail; umask 077
    OTA_MODE=$1; shift; PREP_PARENT=$PPID
    trap 'ota_cleanup $?' EXIT; trap on_signal TERM INT HUP
    ((EUID==0)) || { fail -5 '需要 root 权限执行 A/B 更新'; return; }
    need_tools bash jq awk sed grep head cat stat readlink mkdir mktemp mv rm sync flock findmnt lsblk blkid blockdev mount umount mountpoint timeout setsid md5sum dd od cp tr touch tee chmod systemctl || return
    mkdir -p -- "$RUNTIME" || { fail -5 '运行目录不可写'; return; }
    WORK=$(mktemp -d "$RUNTIME/work.XXXXXXXX") || { fail -5 '无法创建临时作业目录'; return; }
    case $OTA_MODE in
        prepare) (($#==0)) || { fail -5 '准备入口参数无效'; return; }; need_tools curl tar unzip gzip debugfs || return; ota_prepare ;;
        install) (($#==1)) || { fail -5 '安装入口参数无效'; return; }; ota_install "$1" ;;
        health) (($#==0)) || { fail -5 '健康入口参数无效'; return; }; need_tools sqlite3 systemd-cat || return; ota_health ;;
        support) (($#==0)) || { fail -5 '服务安装参数无效'; return; }; install_support ;;
        *) fail -5 '未知 OTA 入口' ;;
    esac
}

# @brief 原附件手动升级流程；单独子 Shell 保持原 set -e、退出码和维护参数。
legacy_entry() (
set -e

# @brief 保留原脚本的参数说明及七种手动调用示例。
show_help() {
    cat << EOF
============================================================
 LubanCat-RK A/B 系统升级脚本
============================================================

【脚本简介】
本脚本用于LubanCat-RK平台的A/B系统升级(OTA/本地)及槽位切换。
脚本会自动识别当前运行的系统槽位(Slot A 或 Slot B)及所在物理磁盘，
并将新固件写入到同一块磁盘的另一个非活动槽位中，
最后通过修改全局boot分区的标志文件来通知U-Boot在下次重启时切换到新系统。

【参数说明】
仅切换操作参数:
  --switch-only              : 仅切换A/B系统槽位并重启。
                               使用此参数时，无需提供 --mode, --target 等参数。
                               脚本会自动识别当前系统，并切换到另一个槽位。
升级模式必填参数(与 --switch-only 互斥):
  --mode <online|local>      : 指定升级模式。
                               'online' 表示从HTTP服务器下载镜像。
                               'local'  表示使用本地已存在的镜像文件。
  --target <boot|rootfs|all> : 指定需要升级的组件。
                               'boot'   : 仅升级boot分区 (内核/设备树)。
                               'rootfs' : 仅升级rootfs分区 (根文件系统)。
                               'all'    : 同时升级boot和rootfs分区。
  --boot <path_or_name>      : 指定boot镜像的路径(本地模式)或文件名(在线模式)。
  --rootfs <path_or_name>    : 指定rootfs镜像的路径(本地模式)或文件名(在线模式)。
                               (注意：如果--target指定了boot或rootfs，对应的镜像参数必须提供)

在线模式(online)必填参数:
  --url <server_url>         : 指定OTA服务器的基础URL(例如: http://192.168.1.100:8080)。
                               脚本会自动拼接文件名，如: http://192.168.1.100:8080/boot_b.img
  --dir <download_dir>       : 指定在线下载时，镜像文件在本地板卡上的存放目录。

可选参数:
  --format-data              : 升级完成后，在重启前自动格式化userdata (data)分区为ext4。
                               适用于大版本更新或需要清空用户数据的场景。
  --keep                     : 保留升级使用的镜像文件。默认情况下不加此参数，
                               升级完成后会自动删除镜像文件以释放存储空间。
  --no-reboot                : 升级/切换完成后不自动重启系统。
                               默认情况下会自动重启。用于外部脚本调用以接管后续重启/关机处理。
【使用示例】

示例 1: 在线升级全部组件(boot + rootfs)到目标槽位，并清空data分区
  sudo update_ab_system.sh \\
      --mode online \\
      --target all \\
      --boot boot_a.img \\
      --rootfs rootfs_a.img \\
      --url http://10.255.200.107:8080 \\
      --dir /userdata/ota_tmp \\
      --format-data

示例 2: 在线仅升级boot分区
  sudo update_ab_system.sh \\
      --mode online \\
      --target boot \\
      --boot boot_a.img \\
      --url http://10.255.200.107:8080 \\
      --dir /userdata/ota_tmp

示例 3: 在线仅升级rootfs分区
  sudo update_ab_system.sh \\
      --mode online \\
      --target rootfs \\
      --rootfs boot_a.img \\
      --url http://10.255.200.107:8080 \\
      --dir /userdata/ota_tmp

示例 4: 本地升级全部组件(镜像已提前通过scp/U盘传到板卡上)
  sudo update_ab_system.sh \\
      --mode local \\
      --target all \\
      --boot /userdata/boot_a.img \\
      --rootfs /userdata/rootfs_a.img

示例 5: 本地仅升级boot分区，且希望保留本地的镜像文件不被删除
  sudo update_ab_system.sh \\
      --mode local \\
      --target boot \\
      --boot /mnt/boot_a.img \\
      --keep

示例 6: 本地仅升级rootfs分区，且希望保留本地的镜像文件不被删除
  sudo update_ab_system.sh \\
      --mode local \\
      --target rootfs \\
      --rootfs /mnt/rootfs_b.img \\
      --keep

示例 7: 仅切换系统槽位并重启
  sudo update_ab_system.sh --switch-only

【注意事项】
1. 必须使用root权限运行本脚本。
2. 升级过程会自动处理槽位切换，无需手动干预U-Boot环境变量。
============================================================
EOF
}

# 1.权限检查
if [ "$EUID" -ne 0 ]; then
    echo "错误: 请使用root权限运行此脚本"
    exit 1
fi

# 2.参数解析
MODE=""
BOOT_PATH=""
ROOTFS_PATH=""
DOWNLOAD_DIR=""
SERVER_URL=""
FORMAT_DATA=false
KEEP_IMAGES=false
SWITCH_ONLY=false
NO_REBOOT=false
TARGET_COMPONENTS="all"

while [[ "$#" -gt 0 ]]; do
    case $1 in
        --mode) MODE="$2"; shift ;;
        --target) TARGET_COMPONENTS="$2"; shift ;;
        --boot) BOOT_PATH="$2"; shift ;;
        --rootfs) ROOTFS_PATH="$2"; shift ;;
        --dir) DOWNLOAD_DIR="$2"; shift ;;
        --url) SERVER_URL="$2"; shift ;;
        --format-data) FORMAT_DATA=true ;;
        --keep) KEEP_IMAGES=true ;;
        --switch-only) SWITCH_ONLY=true ;;
        --no-reboot) NO_REBOOT=true ;;
        -h|--help) show_help; exit 0 ;;
        *) echo "未知参数: $1"
           echo "提示: 请使用 '$0 --help' 查看详细的参数说明和使用示例。"
           exit 1 ;;
    esac
    shift
done

# 3.参数校验
if [ "$SWITCH_ONLY" = false ]; then
    if [ -z "$MODE" ]; then
        echo "错误: 缺少 --mode 参数。"
        echo "提示: 请使用 '$0 --help' 查看详细的参数说明和使用示例。"
        exit 1
    fi

    if [[ "$TARGET_COMPONENTS" != "boot" && "$TARGET_COMPONENTS" != "rootfs" && "$TARGET_COMPONENTS" != "all" ]]; then
        echo "错误: --target 必须是 'boot', 'rootfs' 或 'all'"
        exit 1
    fi

    #根据target决定需要升级的组件
    UPGRADE_BOOT=false
    UPGRADE_ROOTFS=false

    if [[ "$TARGET_COMPONENTS" == "boot" || "$TARGET_COMPONENTS" == "all" ]]; then
        UPGRADE_BOOT=true
        if [ -z "$BOOT_PATH" ]; then
            echo "错误: 升级boot必须提供 --boot 参数"
            exit 1
        fi
    fi

    if [[ "$TARGET_COMPONENTS" == "rootfs" || "$TARGET_COMPONENTS" == "all" ]]; then
        UPGRADE_ROOTFS=true
        if [ -z "$ROOTFS_PATH" ]; then
            echo "错误: 升级rootfs必须提供 --rootfs 参数"
            exit 1
        fi
    fi

    if [ "$MODE" != "online" ] && [ "$MODE" != "local" ]; then
        echo "错误: --mode 必须是 'online' 或 'local'"
        exit 1
    fi

    if [ "$MODE" == "online" ]; then
        if [ -z "$SERVER_URL" ] || [ -z "$DOWNLOAD_DIR" ]; then
            echo "错误: 在线升级模式必须提供 --url 和 --dir 参数"
            exit 1
        fi
    fi
fi

#================= 系统LED闪烁控制 =================
LED_PATH=""
if [ -d "/sys/class/leds/sys_led" ]; then
    LED_PATH="/sys/class/leds/sys_led"
elif [ -d "/sys/class/leds/sys_status_led" ]; then
    LED_PATH="/sys/class/leds/sys_status_led"
fi

LED_BLINK_PID=""

# @brief 原有 LED 提示；不支持状态 LED 的板型直接跳过。
start_led_blink() {
    [ -z "$LED_PATH" ] && return 0
    echo none > "$LED_PATH/trigger" 2>/dev/null || true
    local max_bright=$(cat "$LED_PATH/max_brightness" 2>/dev/null || echo 255)
    (
        local state=0
        while true; do
            if [ "$state" -eq 0 ]; then
                echo "$max_bright" > "$LED_PATH/brightness" 2>/dev/null
                state=1
            else
                echo 0 > "$LED_PATH/brightness" 2>/dev/null
                state=0
            fi
            sleep 0.5
        done
    ) &
    LED_BLINK_PID=$!
    echo ">>> 检测到LED设备: $LED_PATH，后台控制1秒周期循环闪烁"
}

# @brief 原有 LED 提示；不支持状态 LED 的板型直接跳过。
stop_led_blink() {
    [ -z "$LED_PATH" ] && return 0
    if [ -n "$LED_BLINK_PID" ]; then
        kill "$LED_BLINK_PID" 2>/dev/null
        wait "$LED_BLINK_PID" 2>/dev/null || true
        LED_BLINK_PID=""
    fi
    echo 1 > "$LED_PATH/brightness" 2>/dev/null || true
}

# @brief 原有 LED 提示；不支持状态 LED 的板型直接跳过。
heartbeat_led_blink() {
    [ -z "$LED_PATH" ] && return 0
    if [ -n "$LED_BLINK_PID" ]; then
        kill "$LED_BLINK_PID" 2>/dev/null
        wait "$LED_BLINK_PID" 2>/dev/null || true
        LED_BLINK_PID=""
    fi
    echo heartbeat > "$LED_PATH/trigger" 2>/dev/null || true
}

start_led_blink
#==========================================================

# 4.动态识别当前槽位与目标槽位
CURRENT_ROOT_DEV=$(findmnt -n -o SOURCE /)
CURRENT_ROOT_LABEL=$(blkid -s PARTLABEL -o value "$CURRENT_ROOT_DEV")

CURRENT_DISK="/dev/$(lsblk -n -o PKNAME "$CURRENT_ROOT_DEV")"
echo ">>> 检测到当前系统所在物理磁盘: $CURRENT_DISK"

if [ "$CURRENT_ROOT_LABEL" == "rootfs_a" ]; then
    CURRENT_SLOT="a"
    TARGET_SLOT="b"
    echo ">>> 当前运行系统: Slot A ($CURRENT_ROOT_DEV)"
elif [ "$CURRENT_ROOT_LABEL" == "rootfs_b" ]; then
    CURRENT_SLOT="b"
    TARGET_SLOT="a"
    echo ">>> 当前运行系统: Slot B ($CURRENT_ROOT_DEV)"
else
    echo "错误: 无法识别当前运行的系统分区 ($CURRENT_ROOT_LABEL)"
    stop_led_blink
    exit 1
fi

echo ">>> 目标切换/升级系统: Slot ${TARGET_SLOT}"

# @brief 与 Qt OTA/资源更新互斥；维护模式无任务索引时不要求 jq。
need_tools flock findmnt lsblk readlink sync || { echo "错误: $ERROR_MESSAGE"; exit 1; }
mkdir -p "$RUNTIME"
if mountpoint -q /userdata; then legacy_lock=$BASE; legacy_images=/userdata/mnt/UDISK/res/static/demoResources/images
else legacy_lock=$RUNTIME; legacy_images=/mnt/UDISK/res/static/demoResources/images; fi
acquire_lock "$legacy_lock/executor.lock" EXEC_FD || { echo "错误: $ERROR_MESSAGE"; exit 1; }
acquire_lock "$legacy_images/.ycest-update.lock" RESOURCE_FD || { echo "错误: $ERROR_MESSAGE"; exit 1; }
if [[ -s $BASE/active.json ]]; then
    need_tools jq || { echo "错误: $ERROR_MESSAGE"; exit 1; }
    load_active || { echo "错误: $ERROR_MESSAGE"; exit 1; }
    [[ $(printf '%s' "$STATE" | jq -r '.phase') == finished ]] || { echo '错误: OTA 任务执行中'; exit 1; }
fi
LEGACY_MOUNT=''
# @brief 卸载本次私有挂载并停止未完成的 LED 闪烁；保持成功后的 heartbeat 模式。
legacy_cleanup() {
    [[ -z $LEGACY_MOUNT ]] || { umount "$LEGACY_MOUNT" >/dev/null 2>&1 || true; rmdir "$LEGACY_MOUNT" 2>/dev/null || true; }
    if [[ -n $LED_BLINK_PID ]]; then stop_led_blink; fi
    return 0
}
trap legacy_cleanup EXIT

if [ "$SWITCH_ONLY" = false ]; then
    echo ">>> 本次升级组件: ${TARGET_COMPONENTS}"

    TARGET_BOOT_DEV=$(blkid -t PARTLABEL="boot_${TARGET_SLOT}" -o device | grep "^${CURRENT_DISK}" | head -n 1)
    TARGET_ROOTFS_DEV=$(blkid -t PARTLABEL="rootfs_${TARGET_SLOT}" -o device | grep "^${CURRENT_DISK}" | head -n 1)

    if [ -z "$TARGET_BOOT_DEV" ] || [ -z "$TARGET_ROOTFS_DEV" ]; then
        echo "错误: 无法在 $CURRENT_DISK 上找到目标槽位 (Slot ${TARGET_SLOT}) 的boot或rootfs分区！"
        stop_led_blink
        exit 1
    fi

    echo ">>> 目标 boot 分区: $TARGET_BOOT_DEV"
    echo ">>> 目标 rootfs 分区: $TARGET_ROOTFS_DEV"

    # 5.获取镜像文件
    if [ "$MODE" == "online" ]; then
        echo "========================================="
        echo " [在线升级] 正在从服务器下载镜像..."
        echo "========================================="
        mkdir -p "$DOWNLOAD_DIR"

        if [ "$UPGRADE_BOOT" = true ]; then
            BOOT_URL="${SERVER_URL%/}/boot_${TARGET_SLOT}.img"
            echo "下载: $BOOT_URL"
            echo "保存路径：${DOWNLOAD_DIR}/boot_${TARGET_SLOT}.img"
            rm -f -- "${DOWNLOAD_DIR}/boot_${TARGET_SLOT}.img"
            wget -P "$DOWNLOAD_DIR" "$BOOT_URL" || { echo "boot.img 下载失败"; stop_led_blink; exit 1; }
            BOOT_PATH="${DOWNLOAD_DIR}/boot_${TARGET_SLOT}.img"
        fi

        if [ "$UPGRADE_ROOTFS" = true ]; then
            ROOTFS_URL="${SERVER_URL%/}/rootfs_${TARGET_SLOT}.img"
            echo "下载: $ROOTFS_URL"
            echo "保存路径：${DOWNLOAD_DIR}/rootfs_${TARGET_SLOT}.img"
            rm -f -- "${DOWNLOAD_DIR}/rootfs_${TARGET_SLOT}.img"
            wget -P "$DOWNLOAD_DIR" "$ROOTFS_URL" || { echo "rootfs.img 下载失败"; stop_led_blink; exit 1; }
            ROOTFS_PATH="${DOWNLOAD_DIR}/rootfs_${TARGET_SLOT}.img"
        fi
    else
        echo "========================================="
        echo " [本地升级] 校验本地镜像文件..."
        echo "========================================="
        if [ "$UPGRADE_BOOT" = true ] && [ ! -f "$BOOT_PATH" ]; then
            echo "错误: 本地 boot 镜像不存在: $BOOT_PATH"
            stop_led_blink
            exit 1
        fi
        if [ "$UPGRADE_ROOTFS" = true ] && [ ! -f "$ROOTFS_PATH" ]; then
            echo "错误: 本地 rootfs 镜像不存在: $ROOTFS_PATH"
            stop_led_blink
            exit 1
        fi
    fi

    # 6.按需写入目标分区
    echo "========================================="
    echo " [写入分区] 正在刷入目标系统..."
    echo "========================================="

    ALL_WRITES_SUCCESS=true

    if [ "$UPGRADE_BOOT" = true ]; then
        if findmnt -rn -S "$TARGET_BOOT_DEV" >/dev/null 2>&1; then
            echo ">>> 目标 boot 分区已挂载，正在卸载..."
            if ! umount "$TARGET_BOOT_DEV"; then
                echo "错误: 无法卸载 $TARGET_BOOT_DEV，可能被占用。"
                ALL_WRITES_SUCCESS=false
            fi
        fi

        if [ "$ALL_WRITES_SUCCESS" = true ]; then
            echo ">>> 写入 boot_${TARGET_SLOT} 到 $TARGET_BOOT_DEV"
            if ! unmounted "$TARGET_BOOT_DEV" || ! dd if="$BOOT_PATH" of="$TARGET_BOOT_DEV" bs=1M conv=fsync status=progress; then
                echo "错误: 写入 boot 分区失败！"
                ALL_WRITES_SUCCESS=false
            fi
        fi
    fi

    if [ "$UPGRADE_ROOTFS" = true ] && [ "$ALL_WRITES_SUCCESS" = true ]; then
        if findmnt -rn -S "$TARGET_ROOTFS_DEV" >/dev/null 2>&1; then
            echo ">>> 目标 rootfs 分区已挂载，正在卸载..."
            if ! umount "$TARGET_ROOTFS_DEV"; then
                echo "错误: 无法卸载 $TARGET_ROOTFS_DEV，可能被占用。"
                ALL_WRITES_SUCCESS=false
            fi
        fi

        if [ "$ALL_WRITES_SUCCESS" = true ]; then
            echo ">>> 写入 rootfs_${TARGET_SLOT} 到 $TARGET_ROOTFS_DEV"
            if ! unmounted "$TARGET_ROOTFS_DEV" || ! dd if="$ROOTFS_PATH" of="$TARGET_ROOTFS_DEV" bs=1M conv=fsync status=progress; then
                echo "错误: 写入 rootfs 分区失败！"
                ALL_WRITES_SUCCESS=false
            fi
        fi
    fi

    #如果上面任何一步失败，直接中止
    if [ "$ALL_WRITES_SUCCESS" = false ]; then
        echo "====================================================="
        echo "刷写过程中发生严重错误，已中止操作！"
        echo "不会切换启动槽位，系统将在下次重启时保持当前状态。"
        echo "====================================================="
        stop_led_blink
        exit 1
    fi

    echo ">>> 刷写完成，正在同步磁盘缓存..."
    sync

    # 7.按需清理镜像文件
    if [ "$KEEP_IMAGES" = false ]; then
        echo ">>> 清理升级镜像文件 ..."
        [ "$UPGRADE_BOOT" = true ] && rm -f "$BOOT_PATH"
        [ "$UPGRADE_ROOTFS" = true ] && rm -f "$ROOTFS_PATH"
    else
        echo ">>> 保留升级镜像文件 ..."
    fi
fi

# 8.切换启动槽位
echo "========================================="
echo " [切换槽位] 配置 U-Boot 下次从 Slot ${TARGET_SLOT} 启动..."
echo "========================================="

GLOBAL_BOOT_DEV=$(blkid -t PARTLABEL="boot" -o device | grep "^${CURRENT_DISK}" | head -n 1)
if [ -z "$GLOBAL_BOOT_DEV" ]; then
    echo "错误: 无法在 $CURRENT_DISK 上找到 PARTLABEL=boot 的全局boot分区！"
    stop_led_blink
    exit 1
fi

unmounted "$GLOBAL_BOOT_DEV" || { echo "错误: $ERROR_MESSAGE"; exit 1; }
LEGACY_MOUNT=$(mktemp -d "$RUNTIME/legacy-boot.XXXXXXXX")
mount "$GLOBAL_BOOT_DEV" "$LEGACY_MOUNT"
touch "$LEGACY_MOUNT/boot_${TARGET_SLOT}"
sync -f "$LEGACY_MOUNT"
rm -f "$LEGACY_MOUNT/boot_${CURRENT_SLOT}"
sync -f "$LEGACY_MOUNT"
umount "$LEGACY_MOUNT"
rmdir "$LEGACY_MOUNT"
LEGACY_MOUNT=''

if [ "$SWITCH_ONLY" = false ]; then

    # 9.根据参数决定是否格式化data分区
    if [ "$FORMAT_DATA" = true ]; then
        echo "========================================="
        echo " [格式化] 正在格式化 userdata (data) 分区..."
        echo "========================================="

        DATA_PART=$(blkid -t PARTLABEL="data" -o device | grep "^${CURRENT_DISK}" | head -n 1)

        if [ -n "$DATA_PART" ]; then
            echo ">>> 找到 data 分区: $DATA_PART"
            release_lock "$RESOURCE_FD"; RESOURCE_FD=""
            release_lock "$EXEC_FD"; EXEC_FD=""
            if findmnt -rn -S "$DATA_PART" >/dev/null 2>&1; then umount "$DATA_PART"; fi
            unmounted "$DATA_PART" || { echo "错误: $ERROR_MESSAGE"; exit 1; }
            mkfs.ext4 -F -E lazy_itable_init=0,lazy_journal_init=0 "$DATA_PART"
            sync
            echo "data 分区格式化完成！"
        else
            echo "警告: 未在 $CURRENT_DISK 上找到 PARTLABEL=data 的分区，跳过格式化。"
        fi
    fi
fi

echo "========================================="
if [ "$SWITCH_ONLY" = true ]; then
    echo "槽位切换完成！系统将在重启后进入 Slot ${TARGET_SLOT}。"
else
    echo "升级完成！系统将在重启后进入 Slot ${TARGET_SLOT}。"
fi
heartbeat_led_blink
# 10.根据参数决定是否重启
if [ "$NO_REBOOT" = true ]; then
    echo ">>> 已指定 --no-reboot，跳过自动重启，流程结束。"
    exit 0
else
    echo ">>> 正在重启系统..."
    sleep 2
    reboot
fi

)

# 允许测试 source 本文件并替换硬件边界；正常执行只接受固定入口，不读取路径覆盖环境。
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    case ${1:-} in
        --ota-prepare) shift; ota_entry prepare "$@" ;;
        --ota-install) shift; ota_entry install "$@" ;;
        --ota-health) shift; ota_entry health "$@" ;;
        --install-support) shift; ota_entry support "$@" ;;
        *) legacy_entry "$@" ;;
    esac
fi
