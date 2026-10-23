<!--
 @file BOARD_RUN.md
 @brief RK3566 Web 编译、NetworkManager 自动启动部署、上传导入及排查。
 @author Du lin
 @date 2026-10-09
-->
# 虚拟机交叉编译与开发板自动启动部署

Qt/MQTT 由原 face-media 服务启动；独立 device-webd 服务等 Qt 稳定后检测 WiFi，配置 NetworkManager 热点，成功后启动网页服务器。所有进程使用 root，无 WiFi 或门户失败时不阻塞 Qt。

板端运行原 `MergedQtApp` 和 C++ `device-webd`。热点由 NetworkManager 共享连接提供，独立服务统一管理热点和服务器。Ycest 解压、替换及回滚由 Qt 内部 C++ 工作线程执行，按需调用系统 libarchive，不再部署或调用 Python 更新脚本。设备方准备 libarchive 编译头文件和运行库，服务器不引入该库。完整说明见 [Qt 资源更新](../docs/web_ycest_resources_20261020.md)。

完整链路为：手机 Wi-Fi → 独立 C++ HTTPS 服务 → Qt Unix socket → 原统一 SQLite/受控媒体目录。服务器可以独立启动，但登录及文件/人员业务依赖已启用新 Web IPC 的 Qt；不能只启动服务器就宣称链路已通。

本地 API 更新需要同时替换重新构建的 **Qt、device-webd 和 dist**。原 MQTT 与热点启动脚本保持不变。接入项目和暂不可用能力见 [实施记录](../docs/web_local_api_implementation_20261015.md)。

2026-10-16 的 eMMC 存储校验修正只涉及 Qt 业务：视频目录可直接位于 `/dev/mmcblk0p3` 等板载分区，无需将 `/mnt/UDISK` 单独挂载。此次修正重新构建、替换并重启 Qt 即可，HTTPS 服务器和网页产物无需重新构建。目录权限、文件系统只读状态、可用空间和原有配额检查继续保留。

## 1. 在开发机准备文件

工程根目录执行前端构建（完成后回到根目录）：

```sh
cd web/frontend
npm ci
npm run build
cd ../..
python web/tools/provision_account.py --username admin --output factory-account.json
```

密码在提示中隐藏输入，8–256 字符；文件只有随机盐与 PBKDF2 摘要。已存在文件不会覆盖，已有数据库账号不会重新初始化。本步骤在开发机执行，板端无需执行摘要生成；资源解压由 Qt 内部实现，不依赖 Python。

在虚拟机原 RK3566 BSP/交叉工具链环境重新编译 Qt，使用原实际 qmake 路径：

```sh
mkdir -p build-qt-web
cd build-qt-web
/你的板端Qt工具链/bin/qmake ../MergedQtApp.pro
make -j2
```

使用原交叉编译的 sysroot 环境先检查 `pkg-config --modversion gstreamer-pbutils-1.0`。新版 Qt 会链接板端 GStreamer pbutils 库；仅修改 Qt 代码时，独立 `device-webd` 无需重新编译。修改 `web/server` 代码时按第 2 节重新编译服务器。

将新版 ARM64 Qt 可执行文件直接部署为 `/home/cat/face_media/MergedQtApp`，沿用原 Qt 运行目录；不要先放入 `web-stage`。Web 文件单独部署到下面的目录，不复制 node_modules，不使用 Windows .exe 或本机链接检查程序代替板端程序：

```text
/usr/local/sbin/
  start_face_media.sh      ← Qt 原监督脚本，配置 Web 环境变量
  portal-network.sh        ← NetworkManager 检测、隔离、保存和重新激活热点
  device-web-start.sh      ← 等 Qt 稳定后启动热点及 Web 服务器

/home/cat/web-stage/
  device-webd              ← 虚拟机交叉编译的 ARM64 独立 C++ 服务器
  dist/                    ← web/frontend/dist
  factory-account.json     ← 开发机生成的工厂摘要
  portal.env               ← root 0600，网卡/连接/热点密码等参数
  web.json                 ← 从 web/deploy 复制并核对访问地址/路径
  tls/
    server-chain.pem
    server-key.pem
    ca.crt                 ← 私有 CA 调试时用于信任
  imports/                 ← Excel/人脸图片的 Web 上传中间文件
```

证书 SAN 覆盖实际访问地址。当前用 `192.168.10.1`，须包含该 IP；没有证书时按附录 B 在开发机生成调试证书。CA 私钥 `ca.key` 留在开发机。

## 2. 虚拟机交叉编译独立 C++ Web 服务器

本节只编译 `web/server`，产物是 ARM64 可执行文件 `build-web-arm64/device-webd`。第 1 节的 `npm run build` 生成前端 `dist`，Qt 则使用原 qmake 单独编译，两者都不是本节的 CMake 构建目标。

在虚拟机工程根目录检查交叉编译器、下载的 CMake 3.31.12、私有 ARM64 OpenSSL 3 静态库和与板端匹配的 sysroot。首次准备 CMake 和 OpenSSL 3 时先执行附录 A；当前 Qt 工程使用 `/opt/sysroot_ubuntu20`，若实际 BSP 路径不同，以下命令需同步改为真实路径：

```sh
command -v aarch64-linux-gnu-g++
"$HOME/.local/opt/cmake-3.31.12-linux-x86_64/bin/cmake" --version
test -d /opt/sysroot_ubuntu20
OPENSSL_ARM64="$HOME/.local/openssl3-arm64-stage/opt/device-web/openssl3"
test -s "$OPENSSL_ARM64/include/openssl/ssl.h"
test -s "$OPENSSL_ARM64/lib/libssl.a"
test -s "$OPENSSL_ARM64/lib/libcrypto.a"
export PKG_CONFIG_SYSROOT_DIR=/opt/sysroot_ubuntu20
export PKG_CONFIG_LIBDIR=/opt/sysroot_ubuntu20/usr/lib/aarch64-linux-gnu/pkgconfig:/opt/sysroot_ubuntu20/usr/share/pkgconfig
```

服务器使用下载的 CMake 和交叉编译的 OpenSSL 3 静态库；sysroot 自带的 OpenSSL 1.1.1f 不用于此服务器。不要链接虚拟机的 x86 OpenSSL，也不要把缺少 HTTPS 的本机开发构建当作板端产物。CMake 对 OpenSSL 3 的支持见 [官方说明](https://cmake.org/cmake/help/latest/module/FindOpenSSL.html)。

首次配置 `build-web-arm64` 时，在虚拟机工程根目录执行：

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
"$HOME/.local/opt/cmake-3.31.12-linux-x86_64/bin/cmake" --build build-web-arm64 --target device-webd -j2
file build-web-arm64/device-webd
aarch64-linux-gnu-readelf -d build-web-arm64/device-webd | grep NEEDED
```

以后只修改 `web/server` 代码时，沿用现有构建目录，仅执行上面的 `--build` 命令；无需重新运行配置命令，也无需重编前端和 Qt。`file` 必须显示 ARM aarch64，`NEEDED` 中不应出现 `libssl.so.3` 或 `libcrypto.so.3`。交叉编译得到的程序不能在普通 x86 虚拟机直接执行；复制到板端后用 `ldd` 检查所有动态库是否能找到。虚拟机运行的单元测试需要单独构建 x86 版本。将 `build-web-arm64/device-webd` 复制为板端 `/home/cat/web-stage/device-webd`。

Qt 交叉编译环境须有 `gstreamer-pbutils-1.0` 开发包；板端须有与原 Qt 播放环境匹配的 GStreamer 媒体解析插件，无须 `ffprobe`。板端还需现成的 NetworkManager/nmcli、共享模式 DNS/DHCP 支持、iptables、iproute2、timeout、flock 和支持 AP 的网卡驱动。通过有线 SSH/串口在板端检查 `iw dev`、`iw list` 和 `findmnt /mnt/UDISK`。DNS/DHCP 由 NetworkManager 管理，不启动另一个独立 dnsmasq 服务。

## 3. 自动启动部署

先按前两节重编并部署 Qt（本轮热点接口有改动）、原 C++ device-webd 和完整前端 dist。证书由用户维护，本轮不修改 TLS 文件。

在开发板完整源码工程根目录以 root 执行：

```sh
sh web/deploy/install-portal.sh
```

该脚本将 `start_face_media.sh`、`device-web-start.sh`、`portal-network.sh` 统一安装到 `/usr/local/sbin`，配置继续放在 `/home/cat/web-stage`，并安装统一 `device-webd.service`，启用 `face-media.service` 与 `device-webd.service` 开机运行。它不立即重启 Qt/MQTT。旧的 `device-hotspot.service` 被停用；旧独立热点配置、旧 Web drop-in 和 web-stage 下的两个旧管理脚本仅删除明确的已知路径，不递归清理其他文件。需要沿用旧独立热点密码和自定义 SSID时，先迁移到新配置。

2026-10-21 路径迁移：更新上述脚本、`device-webd.service` 和重新编译的 Qt；服务的 ExecStart/ExecReload 及 Qt 默认热点入口均已改为 `/usr/local/sbin`。两个脚本不再按自身目录查找配置，而是读取原 `/home/cat/web-stage/portal.env`。若外部自定义了 `MERGEDQT_WEB_HOTSPOT_HELPER`，需同步改为新路径。安装器已执行 daemon-reload；在计划维护窗口重启 Qt 和门户，或重启开发板，使运行中的旧进程采用新入口。无需因路径迁移重建 Web 服务器或 dist。

编辑 `/home/cat/web-stage/portal.env`（root 0600）：默认 `AP_INTERFACE=auto` 选择空闲且支持 AP 的无线网卡；`AP_CONNECTION=yc-device-portal` 为受管连接名。新建连接须填写有效 `AP_PASSWORD`；可将连接名指定为现有专用 NM 热点以保留原密码。热点地址为 `192.168.10.1/24`，与 web.json 的 bind/origin 一致；勿继续运行旧的 hostapd/dnsmasq。若此前手动将专用 WiFi 设为 NM unmanaged，先恢复该接口的 managed 状态；不要改变其他联网接口。

板端需要 NetworkManager/nmcli、共享连接的 DNS/DHCP 支持、iptables、iproute2、timeout 和 flock。Web 账号已存在时仍使用原账号；空库仍通过原 `factory-account.json` 初始化，不需要添加任何启动前 `test -s` 检查。

## 4. Qt 先启动，门户后台等待

`face-media.service` 沿用原驱动、MQTT、Weston、Qt 监督流程；Qt 每次拉起前自动创建 `/run/mergedqt` 和 `imports` 并导出 Web 所需环境。准备失败不退出原监督脚本。Web 启动/网卡检测完全在独立服务，Qt 不等待它。

统一服务等待真实 Socket 与 `/run/mergedqt/web-ready`，同一 Qt PID 与设备号稳定 3 秒后才检测无线网卡。最多等待 60 秒；无可用 WiFi 正常跳过，不启动服务器。原有上行 WiFi 连接默认不会被抢占。Qt 的就绪发布不依赖热点，避免循环等待。

首次部署后，在计划的维护窗口重启 Qt，再启动门户，或重启开发板让两者自动运行：

```sh
systemctl restart face-media.service
systemctl start device-webd.service
```

第一条命令会按原服务行为同时重启其管理的 MQTT/Qt，正常运行时不需要重复执行。后续开机无需手动创建目录、export、设置热点或启动 Web。不要再手动另起第二份 Qt。

## 5. NetworkManager 热点和网页修改

所有校验通过后，脚本使用实际网卡创建/配置 NM AP 共享连接，写入 `/etc/NetworkManager/dnsmasq-shared.d/91-portal-dns.conf`：

```text
address=/#/192.168.10.1
```

为实际热点网卡插入带 `yc-device-portal` 标记的 FORWARD DROP 规则，热点激活后再检查；关闭热点 IPv6。DNS/隔离/地址配置失败时不启动 Web。重复执行不累积规则，停止时保留原网络规则。NM 负责 DNS/DHCP，本服务不另起独立 hostapd/dnsmasq。

默认 SSID 为 `YC_<mqtt client_id 后五位>_0000`。网页保存时写入 NM 配置和持久化 `portal-ssid`；提示手机断开后延迟 **3 秒**，通过统一服务的 ExecReload 仅重新激活此 NM 连接。Qt、Web 服务器和统一服务都不重启。自定义名称重启设备后仍保留。

## 6. 日志和状态检查

所有进程使用 root。统一服务日志直接写标准输出/错误，由 journal 收集：

```sh
systemctl status face-media.service device-webd.service --no-pager
journalctl -u device-webd.service -f
ls -l /run/mergedqt/web.sock /run/mergedqt/web-ready
nmcli device status
nmcli connection show yc-device-portal
iptables -vnL FORWARD --line-numbers
curl --noproxy '*' --connect-timeout 3 --max-time 30 --cacert /home/cat/web-stage/tls/ca.crt https://192.168.10.1/api/v1/health
```

没有 WiFi 时统一服务正常跳过属于预期，Qt 继续运行。若配置中更改了连接名，上述 nmcli 命令同步修改。HTTP 80 引导到 HTTPS；DNS 指向不保证所有手机自动弹窗，保留固定 IP 手动访问入口。

## 7. 手机验证真实链路

手机连接热点，接受“无互联网仍保持连接”，检查获得 DHCP 地址。私有 CA 调试时先安装并信任 ca.crt。访问 `https://192.168.10.1`，使用创建的网页账号和密码登录；`http://192.168.10.1` 应引导到同一 HTTPS 页面。自动弹出不作为手动访问成功的前提。

按下面顺序验收，每一步实际核对板端结果：

1. 登录成功，刷新网页仍可恢复会话。
2. 上传一个真实小 MP4，设备完成 GStreamer 内容检查后列表显示它；核对正式路径、大小和客户端/板端 SHA256。再将文本文件仅改名为 `.mp4` 上传，确认被拒绝且未发布正式文件。
3. 下载视频，核对下载字节；删除不用的测试视频，实际文件消失。正在播放/下载的文件应返回忙。
4. 上传原模板 XLS/XLSX 与原名图片，文件完整上传后点击开始导入，核对新增/更新/失败项和原 Qt 人员列表。
5. 用已有本地/网络人员及卡号/密码重复样例验证原校验；人脸防重使用原网络阈值参数。
6. 中断一次上传并查询原任务，确认临时文件没有当作正式视频；失败任务可取消，不盲目重放人员批次。
7. 确认其他页面控件禁用，不触发播放、空调、电梯、MQTT 等未接入业务。

这里的手机/真实 SDK/硬件操作须实际执行后才能标记链路打通，编译通过不能替代这些结果。

## 8. 停止门户

```sh
systemctl stop device-webd.service
```

仅停止门户 Web 子进程及本功能的热点连接，清理本功能的 tagged 转发规则和 DNS 配置；保留自定义 SSID 和 NM 连接，不影响 face-media、Qt 或 MQTT。

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

完成 CMake 和 OpenSSL 3 静态库准备后，回到第 2 节配置并编译服务器。服务器仍会使用板端 libc、libstdc++ 等动态库；Qt 保持原 BSP 的 qmake/库，不升级其 OpenSSL。

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
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 36500 -config ca.cnf -keyout ca.key -out ca.crt
openssl req -new -newkey rsa:2048 -nodes -sha256 -subj '/CN=192.168.10.1' -keyout server-key.pem -out server.csr
cat > server.ext <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=IP:192.168.10.1
EOF
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out server-cert.pem -days 36500 -sha256 -extfile server.ext
cat server-cert.pem ca.crt > server-chain.pem
openssl verify -CAfile ca.crt -purpose sslserver -verify_ip 192.168.10.1 server-cert.pem
openssl x509 -in ca.crt -noout -text | grep -c 'X509v3 Basic Constraints'
```

验证应分别输出 `server-cert.pem: OK` 和 `1`；未通过时不要部署。只将 `server-key.pem/server-chain.pem` 部署到板端，`ca.crt` 用于手机/客户端信任；`ca.key` 留在开发机。

## 常见启动问题

| 现象 | 先检查 |
| --- | --- |
| CMake 报 OpenSSL 版本不足/无法识别 | CMake ≥3.18、实际开发库版本；旧 BSP 使用私有库方案 |
| 没有 `/run/mergedqt/web.sock` | 新 Qt 是否安装；Web 环境是否启用；工厂摘要/目录是否可读；Qt 日志 |
| daemon 返回 503 或 socket 错误 | Qt Web 初始化是否成功、路径是否一致、当前服务是否使用 root |
| 无法创建临时文件 | imports/媒体目录是否存在、路径和读写权限、所在文件系统只读状态、空间和配额；无需独立挂载 |
| GStreamer 视频检查失败 | Qt 输出、`gstreamer-pbutils-1.0` 与板端 demux/typefind 插件；确认文件已完整上传 |
| 热点启动失败 | NM 管理状态、AP 能力、是否被上行连接占用、热点密码或同网段冲突 |
| HTTPS 证书错误 | 手机信任 CA、证书 SAN 包含真实访问 IP/域名、板端和手机时间 |
| 登录失败 | 工厂创建的真实用户名/密码；已有账号不会因更换摘要自动覆盖 |

此文档提供部署命令；当前没有连接真实开发板执行这些步骤，实际热点、完整 Qt、HTTPS、人脸 SDK 及断电恢复仍须板端验证。

## 浏览器引导、中文热点与密码更新（2026-10-16）

HTTP 80 的热点探测入口现在返回独立浏览器引导页，不再 302 到登录页。HTTPS 同时提供 `/portal`；`/api/v1/captive` 的 user-portal-url 指向该引导页。`portal-config.json` 的目标地址来自 web.json 的 origin，按钮自动带入此地址，不能由请求的 Host 或查询参数覆盖。

安卓用户点击“打开浏览器并进入设备管理”后，通过 Intent 请求默认浏览器；手机系统可能拦截，所以页面保留地址展示及复制入口。页面不在加载时反复尝试跳转。普通浏览器仍可直接访问原 `/#/login`。小米 14 的实际应用唤起须在手机验收，开发机无法模拟系统门户的许可。

设备设置可以同时修改热点名称和密码，也能仅修改密码。新密码留空保持原值；支持 8–63 位可打印 ASCII 或 64 位十六进制 PSK。设置写入专用 NetworkManager 连接，延迟 3 秒通过原 ExecReload 重新激活，手机需使用新设置重连。HTTP/Qt 不回显原密码或新密码。修改只影响此设备热点，不改变网页登录密码。

SSID 的读取与回滚备份明确使用 `LC_ALL=C.UTF-8` 调用 nmcli，其他机器判定仍保持 C locale。板端须提供 C.UTF-8（此次板端已验证）。直接调用脚本修改新密码时，通过 `save SSID --password-stdin` 的标准输入发送一行密码；保留原 `save SSID` 兼容方式。

登录页“记住密码”使用浏览器密码管理器：标准表单 username/current-password 自动填充，加上浏览器支持时的 PasswordCredential 保存/读取；只有认证成功且勾选后才主动请求保存。localStorage 仅保存勾选偏好，浏览器可能要求用户确认，不能保证自动保存；取消勾选不会删除浏览器密码库中已有条目。眼睛按钮切换显示/隐藏，不改变密码内容。

部署本次功能更新需重新构建、替换 **Qt、device-webd 和完整 dist**，并更新 `/usr/local/sbin/portal-network.sh`。dist 中必须包含 `portal.html/portal.js/portal.css`。首次从旧 web-stage 脚本路径迁移时，按第 3 节同步更新服务单元和 Qt 启动脚本；证书与 portal.env 保持原部署位置。
