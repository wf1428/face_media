<!--
 @file BOARD_RUN.md
 @brief RK3566 Web 链路的前台手动启动、上传导入和排查步骤；不安装自动启动服务。
 @author Du lin
 @date 2026-10-09
-->
# 虚拟机交叉编译与开发板手动联调步骤

当前只手动启动独立 C++ 服务器并打通链路：不安装新的 systemd 服务/drop-in、不修改原启动脚本、不设置开机自启。各进程保留在独立终端前台运行，方便看到错误。

板端运行原 `MergedQtApp` 和新增 C++ `device-webd`。热点由系统 `hostapd/dnsmasq` 提供。Python 只在开发机生成一次工厂账号摘要或运行开发测试；板端上传、SQLite 和 Excel 解析不调用 Python，也不需要 Node/Vite。

完整链路为：手机 Wi-Fi → 独立 C++ HTTPS 服务 → Qt Unix socket → 原统一 SQLite/受控媒体目录。服务器可以独立启动，但登录及文件/人员业务依赖已启用新 Web IPC 的 Qt；不能只启动服务器就宣称链路已通。

## 1. 在开发机准备文件

工程根目录执行前端构建（完成后回到根目录）：

```sh
cd web/frontend
npm ci
npm run build
cd ../..
python web/tools/provision_account.py --username admin --output factory-account.json
```

密码在提示中隐藏输入，10–256 字符；文件只有随机盐与 PBKDF2 摘要。已存在文件不会覆盖，已有数据库账号不会重新初始化。本步骤在开发机执行，板端不需要 Python。

在虚拟机原 RK3566 BSP/交叉工具链环境重新编译 Qt，使用原实际 qmake 路径：

```sh
mkdir -p build-qt-web
cd build-qt-web
/你的板端Qt工具链/bin/qmake ../MergedQtApp.pro
make -j2
```

使用原交叉编译的 sysroot 环境先检查 `pkg-config --modversion gstreamer-pbutils-1.0`。新版 Qt 会链接板端 GStreamer pbutils 库；原独立 `device-webd` 无需重新编译。

将新版 ARM64 Qt 可执行文件直接部署为 `/home/cat/face_media/MergedQtApp`，沿用原 Qt 运行目录；不要先放入 `web-stage`。Web 文件单独部署到下面的目录，不复制 node_modules，不使用 Windows .exe 或本机链接检查程序代替板端程序：

```text
/home/cat/web-stage/
  device-webd              ← 虚拟机交叉编译的 ARM64 独立 C++ 服务器
  dist/                    ← web/frontend/dist
  factory-account.json     ← 开发机生成的工厂摘要
  hostapd.conf             ← 从 web/deploy 复制并填写网卡、SSID、热点密码
  dnsmasq.conf             ← 从 web/deploy 复制并填写同一网卡/地址
  web.json                 ← 从 web/deploy 复制并核对访问地址/路径
  tls/
    server-chain.pem
    server-key.pem
    ca.crt                 ← 私有 CA 调试时用于信任
  imports/                 ← Excel/人脸图片的 Web 上传中间文件
```

证书 SAN 覆盖实际访问地址。当前用 `192.168.50.1`，须包含该 IP；没有证书时按附录 B 在开发机生成调试证书。CA 私钥 `ca.key` 留在开发机。

## 2. 虚拟机交叉编译独立服务

在虚拟机工程根目录检查交叉编译器、CMake 和与板端匹配的 sysroot。当前 Qt 工程配置使用 `/opt/sysroot_ubuntu20`；若实际 BSP 路径不同，以下命令需同步改为真实路径：

```sh
command -v aarch64-linux-gnu-g++
cmake --version
test -d /opt/sysroot_ubuntu20
export PKG_CONFIG_SYSROOT_DIR=/opt/sysroot_ubuntu20
export PKG_CONFIG_LIBDIR=/opt/sysroot_ubuntu20/usr/lib/aarch64-linux-gnu/pkgconfig:/opt/sysroot_ubuntu20/usr/share/pkgconfig
pkg-config --modversion openssl
```

需要 ARM64 C++17 交叉编译器、CMake ≥3.18、目标 ARM64 OpenSSL ≥3.0 的头文件和库。当前虚拟机反馈 CMake 3.16.3、目标 sysroot OpenSSL 1.1.1f，先按附录 A 在虚拟机私有安装 CMake，并交叉编译 OpenSSL 3 静态库。不要链接虚拟机的 x86 OpenSSL，也不要把缺少 HTTPS 的本机开发构建当作板端产物。CMake 对 OpenSSL 3 的支持见 [官方说明](https://cmake.org/cmake/help/latest/module/FindOpenSSL.html)。

若上述条件满足，在虚拟机工程根目录执行：

```sh
cmake -S web/server -B build-web-arm64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
  -DCMAKE_SYSROOT=/opt/sysroot_ubuntu20 \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
  -DOPENSSL_ROOT_DIR=/opt/sysroot_ubuntu20/usr \
  -DDEVICEWEB_TLS=ON \
  -DDEVICEWEB_TESTS=OFF
cmake --build build-web-arm64 -j2
file build-web-arm64/device-webd
aarch64-linux-gnu-readelf -d build-web-arm64/device-webd | grep NEEDED
```

`file` 必须显示 ARM aarch64。交叉编译得到的程序不能在普通 x86 虚拟机直接执行；复制到板端后用 `ldd` 检查所有动态库是否能找到。虚拟机运行的单元测试需要单独构建 x86 版本。将 `build-web-arm64/device-webd` 复制为板端 `/home/cat/web-stage/device-webd`。

Qt 交叉编译环境须有 `gstreamer-pbutils-1.0` 开发包；板端须有与原 Qt 播放环境匹配的 GStreamer 媒体解析插件，无须 `ffprobe`。板端还需现成的 `hostapd`、`dnsmasq`、`iproute2` 和支持 AP 的网卡驱动。通过有线 SSH/串口在板端检查 `iw dev`、`iw list` 和 `findmnt /mnt/UDISK`。本阶段不新增系统网络服务；若需通过 apt 安装 DNS 命令，使用 `dnsmasq-base` 后手动运行，避免安装另一个全局 DNS 服务。

## 3. 手动准备目录、账号与网页

首次调试沿用现有 Qt 的 root 运行用户，C++ 服务器也在 root 终端前台运行，便于先验证链路。权限收敛和自动化在后续再安排。

```sh
sudo -i
cd /home/cat/web-stage
install -d -m 0750 /run/mergedqt
install -d -m 0770 /home/cat/web-stage/imports
test -f /home/cat/web-stage/dist/index.html
test -s /home/cat/web-stage/tls/server-chain.pem
test -s /home/cat/web-stage/tls/server-key.pem
test -s /home/cat/web-stage/factory-account.json
test -s /home/cat/web-stage/web.json
chmod 0600 /home/cat/web-stage/factory-account.json /home/cat/web-stage/tls/server-key.pem
```

`web.json` 的 `www` 应指向 `web-stage/dist`，证书和私钥应指向 `web-stage/tls`。当前手动联调用 root 同时运行 Qt 和 daemon；若以后改回各自服务用户，还需给两者共享的 `imports`、视频 `_uploads` 和 IPC socket 设置相应权限。

确认媒体已真实挂载、原视频目录存在可写；不要在未挂载时创建备用系统分区目录：

```sh
findmnt /mnt/UDISK
test -d /mnt/UDISK/res/static/demoResources/images/video
```

当前调试只会根据新配置在原数据库中新增 Web 表，不删除或重新建人员数据库。第一次启用前可按设备原有备份流程备份统一数据库。

## 4. 一个终端前台运行新版 Qt

沿用原手动运行 Qt 的图形、驱动、SDK 环境。仓库现有监督服务为 `face-media.service`；若它正在自动拉起 Qt，手动调试前暂时停止以避免两份 Qt/监督器抢进程：

```sh
sudo systemctl stop face-media.service
```

这只停止已有服务，不安装新服务、不修改自启配置。该已有服务还管理 MQTT，停止后其 MQTT 也会退出；若需要同时维持原业务，另用原命令手动启动原 mqttd。Web 链路自身不转发 MQTT。

原服务停止后，在**虚拟机工程根目录**将新版 Qt 可执行文件直接复制到原位置，不经由 Web 暂存目录：

```sh
scp build-qt-web/MergedQtApp root@192.168.3.190:/home/cat/face_media/MergedQtApp
```

若板端 SSH 地址不同，替换命令中的地址。在准备好的板端原运行环境/root 终端内，仅为本次进程设置 Web 环境：

```sh
cd /home/cat/face_media
test -x ./MergedQtApp
ldd ./MergedQtApp | grep gstpbutils
export MERGEDQT_WEB_ENABLE=1
export MERGEDQT_WEB_SOCKET=/run/mergedqt/web.sock
export MERGEDQT_WEB_MEDIA_MOUNT=/mnt/UDISK
export MERGEDQT_WEB_IMPORTS=/home/cat/web-stage/imports
export MERGEDQT_WEB_FACTORY=/home/cat/web-stage/factory-account.json
./MergedQtApp
```

上述变量只在当前终端/子进程生效，不写入任何启动脚本。如果 Qt 原先需要 Wayland/EGLFS、库/插件路径，沿用你原来已能运行的环境；不要为测试改变图形后端。Weston/驱动/媒体挂载应已按原设备方式就绪。

看到 `[WEB] IPC started` 后，在另一终端检查：

```sh
ls -l /run/mergedqt/web.sock
```

没有 Socket 则先排查 Qt 输出、工厂摘要和环境变量，不能继续把登录失败当作 HTTP 服务问题。

## 5. 两个终端手动启动热点与 DHCP

先从 `iw dev/iw list` 确认实际网卡和 AP 能力。下例使用专用 `wlan1`；配置中的 interface 必须一致。正在承担原联网的 STA 网卡不能未经确认就直接切换 AP；单网卡需先确定共存方案或使用第二块网卡。

编辑 `/home/cat/web-stage/hostapd.conf`：填写 interface、SSID，以及 8–63 ASCII 字符热点口令，替换出厂占位符。编辑 `dnsmasq.conf`：同一 interface、DHCP 范围和网关为 `192.168.50.1`。租约文件保留样例的 `/run/device-hotspot/dnsmasq.leases`，启动前手动创建其目录。

若 NetworkManager 当前管理这块专用网卡，仅在本次测试中解除该接口管理，不关闭全局网络管理器：

```sh
sudo nmcli device set wlan1 managed no
```

只在实际使用 NetworkManager 时执行。root 终端 A：

```sh
ip link set wlan1 up
ip address replace 192.168.50.1/24 dev wlan1
hostapd /home/cat/web-stage/hostapd.conf
```

保持 A 前台运行，root 终端 B：

```sh
mkdir -p /run/device-hotspot
dnsmasq --keep-in-foreground --conf-file=/home/cat/web-stage/dnsmasq.conf
```

不调用 `hotspot-start.sh`。若端口被其他实例占用，先明确该实例归属，不直接停止原网络服务或全局改端口。

## 6. 单独前台启动 C++ HTTP/HTTPS 服务器

保持 Qt、hostapd、dnsmasq 终端运行，确认 web.json 为：

```json
{
  "_comment": "@file web.json | @brief 手动板端联调配置 | @author Du lin",
  "bind": "192.168.50.1",
  "port": 443,
  "portal_port": 80,
  "origin": "https://192.168.50.1",
  "socket": "/run/mergedqt/web.sock",
  "www": "/home/cat/web-stage/dist",
  "certificate": "/home/cat/web-stage/tls/server-chain.pem",
  "private_key": "/home/cat/web-stage/tls/server-key.pem",
  "development_http": false,
  "device_name": "手动联调设备",
  "device_id": "填写真实设备编号",
  "wifi_name": "与hostapd一致的SSID"
}
```

root 终端 C：

```sh
cd /home/cat/web-stage
./device-webd /home/cat/web-stage/web.json
```

这条命令就是独立启动服务器，不经过 Qt 启动脚本，也不经过 systemd 新服务。root 用于本次直接绑定 80/443；如端口已有服务，可将 port/portal_port 改成 8443/8080，origin 同时改成 `https://192.168.50.1:8443`，访问也须带相同端口。

在检查终端执行：

```sh
ip -4 addr show wlan1
ss -lntp
curl --noproxy '*' --connect-timeout 3 --max-time 8 -v --cacert /home/cat/web-stage/tls/ca.crt https://192.168.50.1/api/v1/device/info
curl --noproxy '*' --connect-timeout 3 --max-time 30 -v --cacert /home/cat/web-stage/tls/ca.crt https://192.168.50.1/api/v1/health
curl --noproxy '*' --connect-timeout 3 --max-time 8 -I http://192.168.50.1
```

`device/info` 不依赖 Qt，用于先验证直连、TLS 和服务器本身；它正常而 `health` 不正常时，再检查 Qt IPC socket 和 Qt 输出。health 应返回 code 200/ready true。`--noproxy '*'` 避免开发板环境代理劫持本地 IP 请求，连接及总超时避免无响应时一直等待。正式证书使用对应受信 CA；不将 `curl -k` 作为正式证书验收。当前防火墙须允许此 AP 的 DNS/DHCP 和 HTTP/HTTPS，不全局关闭防火墙。

## 7. 手机验证真实链路

手机连接热点，接受“无互联网仍保持连接”，检查获得 DHCP 地址。私有 CA 调试时先安装并信任 ca.crt。访问 `https://192.168.50.1`，使用创建的网页账号和密码登录；`http://192.168.50.1` 应引导到同一 HTTPS 页面。自动弹出不作为手动访问成功的前提。

按下面顺序验收，每一步实际核对板端结果：

1. 登录成功，刷新网页仍可恢复会话。
2. 上传一个真实小 MP4，设备完成 GStreamer 内容检查后列表显示它；核对正式路径、大小和客户端/板端 SHA256。再将文本文件仅改名为 `.mp4` 上传，确认被拒绝且未发布正式文件。
3. 下载视频，核对下载字节；删除不用的测试视频，实际文件消失。正在播放/下载的文件应返回忙。
4. 上传原模板 XLS/XLSX 与原名图片，文件完整上传后点击开始导入，核对新增/更新/失败项和原 Qt 人员列表。
5. 用已有本地/网络人员及卡号/密码重复样例验证原校验；人脸防重使用原网络阈值参数。
6. 中断一次上传并查询原任务，确认临时文件没有当作正式视频；失败任务可取消，不盲目重放人员批次。
7. 确认其他页面控件禁用，不触发播放、空调、电梯、MQTT 等未接入业务。

这里的手机/真实 SDK/硬件操作须实际执行后才能标记链路打通，编译通过不能替代这些结果。

## 8. 手动停止与恢复

在 C++、dnsmasq、hostapd 的各前台终端按 Ctrl+C 退出，结束 Qt 手动调试后可恢复设备原启动方式。若测试前由原服务运行：

```sh
sudo systemctl start face-media.service
```

若测试中给专用网卡增加了地址，且原先没有该地址，可手动移除 `192.168.50.1/24`；若曾对 NetworkManager 修改接口 managed 状态，只恢复其原状态。不删除数据库/人员文件，不删除未经确认的原网络地址。

当前未安装新的 .service/drop-in，未修改原脚本；部署模板仅保留为以后自动化参考。当前开发环境没有板端连接信息，尚未在真实板子执行这些命令。

## 附录 A：虚拟机私有安装 CMake 与 ARM64 OpenSSL 3

本附录只在 x86_64 虚拟机执行。虚拟机现有 CMake 3.16.3 和目标 sysroot OpenSSL 1.1.1f 保持原样。先从 [CMake 官方下载页](https://cmake.org/download/) 下载 Linux x86_64 预编译包；这里使用官方 3.31.12 示例：

```sh
cd ~/Downloads
curl -fLO https://cmake.org/files/v3.31/cmake-3.31.12-linux-x86_64.tar.gz
mkdir -p "$HOME/.local/opt"
tar -xzf cmake-3.31.12-linux-x86_64.tar.gz -C "$HOME/.local/opt"
"$HOME/.local/opt/cmake-3.31.12-linux-x86_64/bin/cmake" --version
```

从 [OpenSSL 官方下载页](https://openssl-library.org/source/) 获取 3.5.9 LTS 源码。官方源码链接可能跳转 GitHub；如果虚拟机无法访问，从能下载的开发电脑取得 `openssl-3.5.9.tar.gz` 后复制到虚拟机 `~/Downloads`。编译工具需有 Perl、make 和已检查的 `aarch64-linux-gnu-gcc`。以下仅生成静态库，目标安装前缀表示板端路径，`DESTDIR` 把实际文件留在虚拟机用户目录：

```sh
cd ~/Downloads
tar -xzf openssl-3.5.9.tar.gz
cd openssl-3.5.9
./Configure linux-aarch64 --cross-compile-prefix=aarch64-linux-gnu- \
  --prefix=/opt/device-web/openssl3 \
  --openssldir=/opt/device-web/openssl3/ssl \
  --libdir=lib no-shared no-module no-apps no-tests -fPIC
make -j2
make DESTDIR="$HOME/.local/openssl3-arm64-stage" install_sw
test -s "$HOME/.local/openssl3-arm64-stage/opt/device-web/openssl3/lib/libssl.a"
test -s "$HOME/.local/openssl3-arm64-stage/opt/device-web/openssl3/lib/libcrypto.a"
```

用新 CMake 和私有 ARM64 静态库编译服务器。OpenSSL 的 include/lib 显式指向私有构建，其他目标库继续由板端 sysroot 提供：

```sh
cd ~/face_media/MergedQtApp
OPENSSL_ARM64="$HOME/.local/openssl3-arm64-stage/opt/device-web/openssl3"
"$HOME/.local/opt/cmake-3.31.12-linux-x86_64/bin/cmake" -S web/server -B build-web-arm64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
  -DCMAKE_SYSROOT=/opt/sysroot_ubuntu20 \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
  -DOPENSSL_INCLUDE_DIR="$OPENSSL_ARM64/include" \
  -DOPENSSL_SSL_LIBRARY="$OPENSSL_ARM64/lib/libssl.a" \
  -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL_ARM64/lib/libcrypto.a" \
  -DOPENSSL_USE_STATIC_LIBS=TRUE \
  -DDEVICEWEB_TLS=ON \
  -DDEVICEWEB_TESTS=OFF
"$HOME/.local/opt/cmake-3.31.12-linux-x86_64/bin/cmake" --build build-web-arm64 -j2
file build-web-arm64/device-webd
aarch64-linux-gnu-readelf -d build-web-arm64/device-webd | grep NEEDED
```

`file` 应显示 ARM aarch64；`NEEDED` 中不能有 `libssl.so.3` 或 `libcrypto.so.3`，因为这两项已经静态链接。服务器仍会使用板端 libc、libstdc++ 等动态库，复制到板端后用 `ldd` 核对；不要在 x86 虚拟机运行 ARM64 测试程序。Qt 保持原 BSP 的 qmake/库，不升级其 OpenSSL。

## 附录 B：开发机生成私有 CA 调试证书

下面在具备 OpenSSL 的开发机/Linux 工厂环境执行。此为调试信任方案；量产使用产品的正式证书管理方案。依据 [OpenSSL req](https://docs.openssl.org/1.1.1/man1/req/) 和 [OpenSSL x509](https://docs.openssl.org/1.1.1/man1/x509/) 官方接口。请使用全新空目录；不要将不同批次生成的 CA、服务器证书和私钥混用。显式指定 CA 配置文件，避免系统 `openssl.cnf` 已定义 `v3_ca` 时，再通过 `-addext` 写入重复的 `Basic Constraints` 扩展。

```sh
mkdir device-cert
cd device-cert
umask 077
cat > ca.cnf <<'EOF'
[req]
distinguished_name = dn
x509_extensions = v3_ca
prompt = no

[dn]
CN = Device Local Test CA

[v3_ca]
subjectKeyIdentifier = hash
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
EOF
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 -config ca.cnf -keyout ca.key -out ca.crt
openssl req -new -newkey rsa:2048 -nodes -sha256 -subj '/CN=192.168.50.1' -keyout server-key.pem -out server.csr
cat > server.ext <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=IP:192.168.50.1
EOF
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out server-cert.pem -days 365 -sha256 -extfile server.ext
cat server-cert.pem ca.crt > server-chain.pem
openssl verify -CAfile ca.crt -purpose sslserver -verify_ip 192.168.50.1 server-cert.pem
openssl x509 -in ca.crt -noout -text | grep -c 'X509v3 Basic Constraints'
```

验证应分别输出 `server-cert.pem: OK` 和 `1`；未通过时不要部署。只将 `server-key.pem/server-chain.pem` 部署到板端，`ca.crt` 用于手机/客户端信任；`ca.key` 留在开发机。

## 常见启动问题

| 现象 | 先检查 |
| --- | --- |
| CMake 报 OpenSSL 版本不足/无法识别 | CMake ≥3.18、实际开发库版本；旧 BSP 使用私有库方案 |
| 没有 `/run/mergedqt/web.sock` | 新 Qt 是否安装；Web 环境是否启用；工厂摘要/目录是否可读；Qt 日志 |
| daemon 返回 503 或 socket 权限错误 | `/run/mergedqt` setgid、Socket cat 组、Qt 业务是否初始化成功 |
| 无法创建临时文件 | imports/媒体父目录共享组和 setgid、新缓存组写入权限、真实媒体挂载/只读状态 |
| GStreamer 视频检查失败 | Qt 输出、`gstreamer-pbutils-1.0` 与板端 demux/typefind 插件；确认文件已完整上传 |
| 热点启动失败 | AP 网卡、驱动 AP 能力、出厂占位口令、其他网卡管理器抢占 |
| HTTPS 证书错误 | 手机信任 CA、证书 SAN 包含真实访问 IP/域名、板端和手机时间 |
| 登录失败 | 工厂创建的真实用户名/密码；已有账号不会因更换摘要自动覆盖 |

此文档提供部署命令；当前没有连接真实开发板执行这些步骤，实际热点、完整 Qt、HTTPS、人脸 SDK 及断电恢复仍须板端验证。
