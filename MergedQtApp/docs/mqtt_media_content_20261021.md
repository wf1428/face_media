# MQTT 广告视频、图片与小图播放（2026-10-21）

`videoControl.data` 新增 `contentType`、`smallImageMode`。原有 `videoType`、音量、FTP 账号参数、直播切换、下载预检查及最终回执链路继续使用原有字段和入口。

## 下发与保存

```json
{
  "method": "videoControl",
  "id": "example-1",
  "deviceId": "设备ID",
  "data": {
    "videoType": "RECORDED",
    "videoPath": "风景.jpg",
    "contentType": "IMAGE",
    "smallImageMode": 0,
    "ftpPath": "FTP主机",
    "ftpPort": "21",
    "ftpUser": "FTP用户名",
    "ftpPwd": "FTP密码"
  }
}
```

`contentType` 控制 videoRect 区域的本地资源列表：

| 值 | videoRect 行为 |
| --- | --- |
| `VIDEO` | 仅播放视频 |
| `IMAGE` | 仅播放图片 |
| `VIDEO_IMAGE` | 视频与图片按文件名排序轮播 |

缺省、空值及非法值在设备内部使用 `VIDEO_IMAGE`，有效字符串会去除首尾空白并转为大写。收到 RECORDED 请求时保存两个选择项；LIVE/NONE 请求只有携带新字段时才更新它们。选择保存到设备原有 `net_cfg.ini`：

```ini
[playback]
contentType=VIDEO_IMAGE
smallImageMode=0
```

设备通常使用 `/mnt/UDISK/res/static/demoResources/images/logo/net_cfg.ini`。旧配置没有这些键时直接使用上述默认值，不要求手工迁移；Qt 重启或整机重新上电后重建播放列表时读取保存值。其他播放状态键保持原有逻辑。

## 下载及播放区域

| 文件 | smallImageMode | 下载目录 | 播放区域 |
| --- | --- | --- | --- |
| 图片 | `1` | `/mnt/UDISK/res/static/demoResources/images/pic` | 原有 pic 区域 |
| 图片 | `0` 或缺省 | `/mnt/UDISK/res/static/demoResources/images/video` | videoRect 区域，等比适配 |
| 视频 | 任意 | `/mnt/UDISK/res/static/demoResources/images/video` | videoRect 区域 |

`smallImageMode` **只区分图片的下载目录，不是播放开关**：缺省为 0，数值 1（兼容字符串 `"1"`）选择 pic 目录，0 选择 video 目录。MQTT 配置路径与 FTP 自动下载入口使用同一组图片格式判断；FTP 启动时不再将小图强制改回 video 目录。视频不会因为开启小图模式而进入 pic。

图片下载完成后先按真实扩展名和保存目录分类。位于 pic 的图片仅刷新 pic 缓存、选择该图片并恢复原有主区域播放；不会加入 videoRect 列表。位于 video 的资源只有被当前 `contentType` 允许时才立即播放，否则只保存文件并恢复允许的本地列表。类型切换后没有可播项时停止旧的视频管线，进入原有空列表检测。

按本轮确认，**pic 区域保持独立显示，`contentType=VIDEO` 也不关闭原有 pic 轮播**。pic 播放开关仅由原有 `PicDisplay_en` 控制：false 时不播放，true 时按原有规则播放；即使下载请求 smallImageMode=1，也不会强行开启 pic。显示位置、缩放方式、切换间隔、单图静态显示和人脸结果覆盖时的暂停/恢复规则继续生效。pic 文件取消 `01`、`02` 等编号限制，按任意文件名扫描常见图片；外部目录有图片时继续整组使用外部资源，未找到时使用 qrc 资源组。刷新时释放旧轮播定时器后重建，防止重复定时器。

支持识别 PNG、JPG/JPEG、BMP、GIF、WebP、TIF/TIFF，扩展名大小写均可；实际解码依赖设备安装的 Qt 图片插件。GIF 沿用原有图片显示方式，本轮没有新增动画图片播放机制。视频格式保留既有集合并统一不同入口的识别范围。

FTP 继续使用已有 curl 单文件下载、`.part` 临时文件、完成改名和空间检查流程。已存在且大小相同的文件不重复下载，但会通过现有媒体就绪入口应用本次选择。下载中的去重键新增归一化后的类型及小图标志，防止同名文件在不同目录/播放选择下被错误合并。下载进度提示根据文件类型显示图片或视频。

## 回执

- `videoControl` 回执从原请求的 `data` 构造，已下发的 `contentType`、`smallImageMode` 保持原 JSON 值与类型，不使用设备内部归一化值覆盖；旧请求未携带时补 `VIDEO_IMAGE`、`0`。status、ftpDownload、downloadResult 等字段按原链路生成。
- `sendPlayFileList.data.file` 扫描固定 video 目录的视频与图片，并补充固定 pic 目录的图片；仍上传 `{ "id": 1, "name": "文件基础名" }`，不改变字段结构、不按当前播放类型过滤库存。
- `sendPlayInfo.data.info` 沿用现有结构，文件信息仅来自固定 `/mnt/UDISK/res/static/demoResources/images/video` 目录的视频和图片。图片复用 `setCurrentRecordedFile()` 更新 `videoPath`（带扩展名）和 `name`（不带扩展名），下载日志继续来自原 FTP 回调。pic 初始化、轮播和人脸覆盖恢复不再更新当前录播文件；仅有 pic 时 `name`、`videoPath` 留空。服务层同时检查完整来源路径，pic 下载等待期间也不回显小图名称，不通过同名文件反查目录。音量、直播、日志和整屏截图沿用原逻辑，pic 下载、播放和开关不变。

## 删除视频与图片

`deletePlayFile.data.name` 使用 `sendPlayFileList` 中的原始基础名（不带扩展名）；`data.id` 使用同一条文件记录的 ID，兼容数值及整数字符串。例如：

```json
{
  "method": "deletePlayFile",
  "id": "delete-001",
  "deviceId": "设备ID",
  "data": { "name": "风景", "id": "3" }
}
```

删除与文件列表共用目录、格式、排序及中文名称修复规则，支持 video 中的视频/图片和 pic 中的图片。ID 与名称必须同时匹配最近一次文件列表快照中的同一记录；不同扩展名或不同目录的同名文件不会互相替代。首次查询列表前收到删除时，使用同样的扫描规则初始化快照；ID 为本地列表序号，不是平台任意分配的资源 ID。

同一份列表连续删除时保留剩余记录的原 ID，不因目录项目减少或新增而移动；重复删除不会落到下一个同名文件。重新获取列表后以新回传的 ID 为准。兼容旧版未携带 ID 的请求，但只允许名称唯一时删除；错误 ID、名称不匹配、同名歧义或删除失败仅记录失败日志。删除协议继续不增加独立 MQTT 回执，平台可调用 `getPlayFileList` 确认结果。

删除成功后通过既有设置页/主界面的信号转发通知播放器：移除媒体列表和 pic 已解码缓存，删除正在显示的主区域文件时切换剩余项，删除最后一项时清空文件信息并进入原有空列表监测。删除非当前项不重新启动正在播放的视频；小图缓存刷新不改变独立主区域、人脸覆盖暂停、下载暂停和直播状态。pic 外部组为空时仍按原规则回退内置资源。

## JPG 黑屏原因

原目录扫描本来能够识别 JPG，但是 `playDownloadedVideoNow()` 又将所有下载文件加入纯视频列表，并只查找视频类型项。已有 JPG 图片项因此匹配失败，被再次追加成 `VideoRectMedia_Video`。这对应日志中同一路径从图片项切到追加的视频项，随后进入 GStreamer/RGA 的现象。

现在下载完成入口只使用重建后的真实分类项，既不将 JPG 加入纯视频列表，也不追加重复的视频项；视频入口另加图片保护。该修复针对图片被误送视频解码器造成的黑屏，RGA 对实际视频的 DMA-BUF 要求和禁止 CPU 回退策略保持原有逻辑。

## 验证与部署

Windows Qt 5.15.2 本地隔离验证通过：

1. 实际 MQTT 服务代码（仅移除 Windows 不具备的 Linux 头文件，平台路径/时钟函数用测试实现）验证保存与重新读取、图片/视频下载目录、缺省/空值/非法值、回执原值类型、任务去重键、原有音量请求、文件清单和图片播放信息。新增验证：pic 及同名小图不进入 sendPlayInfo、pic 待下载状态也不回显文件名，音量/日志/截图/直播保留；数值/字符串 ID、ID/名称错配、跨目录和扩展名同名、连续/重复删除、插入文件后原 ID 不变、全部常见图片格式删除、旧版无 ID 及启动后首次删除。
2. 从实际 MultimediaDemo 提取的列表重建、播放分派、下载完成、pic 初始化、类型切换、状态通知、截图、人脸结果恢复及删除处理代码，在 QWidget 环境下运行；仅替换硬件视频后端和外部界面对象。复现 JPG 下载后的路径，确认图片不会送入视频后端、不会重复追加；验证三种筛选、类型切换后旧视频停止、小图独立播放、任意文件名、重复刷新无重复定时器、持久化读取、延迟下载完成回调、图片及仅小图截图、人脸覆盖恢复。新增验证删除非当前项不重启主区域、删除当前图片后切换、直播保留、删除小图清理缓存且保留人脸覆盖暂停、删除最后主区域项清空并进入监测、模块暂停时不抢先启动。
3. 实际 FTP 自动下载启动及路径归一化代码提取验证：八种图片扩展名、中文和空格文件名分别在两种 smallImageMode 下进入正确目录，视频保持 video 目录，缺省 0，以及原有自动下载开关。

隔离测试与构建记录位于 `.web-build/media-content-tests/`，硬件后端测试会记录所有收到的文件路径并断言不存在图片。没有连接开发板或真实 MQTT/FTP 平台；Windows SDK 缺少目标 Qt SerialPort/GStreamer 等依赖，未完成 RK3566 整体构建。部署需使用目标交叉编译环境重编译 Qt 应用；旧 net_cfg.ini 可直接保留，新选择由默认值和后续 MQTT 请求补入。设备验收需覆盖真实 FTP 下载、三种类型切换、小图独立显示、sendPlayInfo/文件列表回执以及带配置重新上电。
