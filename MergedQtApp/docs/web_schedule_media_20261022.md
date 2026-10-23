# Web 定时梯控与图片广告接入（2026-10-22）

## 定时梯控

Web 新增、删除和保存规则由独立的 `modules/web/web_floor_schedule.cpp/.h` 实现。规则校验、楼层位序、存储结构和分钟去重规则与原协议兼容，不调用或修改 MQTT 管理器、消息处理和原定时器。

- 继续使用原 `online_v1_floor_schedules`、`online_v1_floor_schedule_bits`、`online_v1_floor_limit_bits` 和 `online_v1_floor_schedule_last_minute`。
- Web 写入通过主线程桥交给 WebFloorSchedule，完成落库后才确认任务成功。`baseSchedules` 用于检查网页读取后是否已被 MQTT 或其他网页更新；有冲突时返回 409，要求刷新。
- Web 启用时创建独立的 5 秒检查定时器。首次保存后，在同一事务保存 `floor_schedule_local_enabled=1`（SQLite config）；该标记只由 Web 执行器读取，支持离线执行和重启恢复。原 MQTT 定时器仍保持原 online_v1 主题条件。
- 两个执行器都在 GUI 线程，通过同一 `online_v1_floor_schedule_last_minute` 去重；任一执行器先保存并发送后，另一执行器跳过该分钟。不会向串口重复发送同一分钟的共享规则。
- MQTT 原有下行方法、鉴权、全量覆盖、hash 删除、查询和回执格式保留。两种入口都读取同一份规则；后写入的有效规则以共享存储为准。
- Web 独立实现相同的星期、时刻、楼层、action、hash 唯一性和同一时刻动作冲突校验。支持 -8～-1、1～120，排除 0。
- 新增在页面形成草稿，点击保存后生效；删除既有规则会立即提交删除，未保存的新增草稿仅从页面移除。删除既有规则后仍保留其他新增草稿。
- 规则保存不直接改变所有楼层；若新规则命中当前分钟，沿用原即时检查执行。相同规则不重置分钟标记；重复请求不重复执行。删除规则不撤销已执行的楼层状态，与原 MQTT 删除一致。
- 规则和本地执行开关持久化，重启后 Web 执行器继续读取。串口发送仍走公共 `IcEventBridge`，不新增硬件执行确认协议。

已撤回上一轮 Web 接入对 `mqttmanager.cpp`、`mqttmanager.h`、`mqttservice.cpp` 的改动：删除新增的本地规则入口，恢复原保存函数签名和原执行条件，恢复原直接写 INI 的代码。此前已完成的 MQTT 图片下载、删除和回执功能保留。

## 图片广告与共享配置

媒体页在“本地播放／网络地址／停止”下方增加“仅视频／仅图片／视频与图片轮播”三个选项，并增加小图保存开关。网页和 MQTT 共用设备原 `net_cfg.ini` 的 `[playback] contentType`、`smallImageMode`，分别由各自入口保存；MQTT 保留原直接写入代码，Web 使用独立保存入口，没有 Web 专属配置副本。

`contentType` 支持 VIDEO、IMAGE、VIDEO_IMAGE，缺省/空/非法值归一为 VIDEO_IMAGE。Web 请求省略某个选择字段时保留现有值，原 MQTT 的缺省参数处理不变。Web 每次读取播放信息直接读取共享 INI 的这两个字段，不使用旧网页缓存替代设备配置。

小图选项只决定图片上传目录：

| 资源 | 选项 | 目录 |
|---|---|---|
| 图片 | smallImageMode=1 | `/mnt/UDISK/res/static/demoResources/images/pic` |
| 图片 | smallImageMode=0 | `/mnt/UDISK/res/static/demoResources/images/video` |
| 视频 | 任意 | `/mnt/UDISK/res/static/demoResources/images/video` |

pic 显示仍由设备原 `PicDisplay_en` 控制，与 contentType 无关；关闭小图保存选项不移动既有文件。上传任务创建时固定目标目录，之后全局模式变化不改变已在途任务的落盘位置。

- 广告图片新增上传类别 `ad_image`，与人员人脸图片分别处理。支持 PNG、JPG/JPEG、BMP、GIF、WebP、TIF/TIFF，大小写均可，任意受控文件名；实际解码依赖设备 Qt 图片插件。GIF 沿用既有静态广告图片行为。
- 图片在 Web 工作线程进行真实内容解码校验，继续使用 `.part`、原子发布和提交幂等；不把图片送进 GStreamer 视频校验器。不新增 UI 线程解码循环。
- 广告图片与视频共用媒体预算、未完成上传互斥、资源包更新互斥。人员导入、资源包校验流程保持原入口。
- 上传发布后传递完整设备路径，按当前 contentType 筛选主区域，pic 图片进入独立区域；不符合当前类型的文件仅保存，不强行进入主区域。
- 文件列表扫描 video 视频/图片和 pic 图片，返回 `mediaType`、`smallImageMode`；下载、版本化 ID、防旧列表误删和视频占用保护沿用原流程。删除图片通过主线程通知现有播放器清理缓存。
- 页面当前播放文件仍来自真实主区域状态，pic 不替换当前录播文件信息。MQTT sendPlayInfo 的原目录限制保持不变。

## 验证与部署

开发机验证包括：Qt Web 实际服务集成、独立 WebFloorSchedule（不链接 MQTT）、真实图片解码/发布/目录冻结/下载删除、共享配置读取、主线程任务桥及幂等；恢复后的实际 MQTT 规则方法与独立 Web 执行器连接真实 DbStore 的隔离互操作测试，覆盖 MQTT 原执行条件与回执、Web 离线执行、重启、双执行器分钟去重、负楼层位序和更新冲突；原前端构建与 Chromium 页面操作验证保持有效，修正未改动网页。

测试输出位于 `.web-build/web-independent-schedule-tests.log`、`.web-build/floor-schedule-independent-tests/` 和 `.web-build/browser-schedule-media-20261022/`。隔离测试替代了硬件串口/播放器；不代表已完成开发板和真实电梯的运行验收。

部署需要重新编译 Qt 并更新 Web 静态文件。本轮未修改 device-webd 源码，其既有通用上传和标准梯控路由继续使用。原 INI 可保留，新开关随 Web 保存写入。

## Web接入最终文件清单

MQTT三个文件已恢复，不计入以下新增功能的净修改清单。

1. `common/media_content_policy.h`
2. `common/sql/web/web_upload_repository.cpp`
3. `components/multimedia/multimediademo.cpp`
4. `main.cpp`
5. `mainwindow.cpp`
6. `modules/web/web_local_api.cpp`
7. `modules/web/web_local_runtime.cpp`
8. `modules/web/web_local_runtime.h`
9. `modules/web/web_paths.cpp`
10. `modules/web/web_service.cpp`
11. `modules/web/web_service.h`
12. `web/frontend/src/api/local.js`
13. `web/frontend/src/api/index.js`
14. `web/frontend/src/views/elevator/ElevatorControl.vue`
15. `web/frontend/src/views/video/VideoManage.vue`
16. `web/frontend/src/i18n/locales/zh.json`
17. `web/frontend/src/i18n/locales/en.json`
18. `web/frontend/src/i18n/locales/vi.json`
19. `web/frontend/src/i18n/locales/th.json`
20. `web/tests/qt_web_tests.cpp`
21. `web/tests/test_platform.cpp`
22. `web/tests/browser_check.py`
23. `web/README.md`
24. `docs/web_local_api_implementation_20261015.md`
25. `docs/web_schedule_media_20261022.md`
26. `modules/web/web_upload_presenter.cpp`
27. `modules/web/web_floor_schedule.cpp`
28. `modules/web/web_floor_schedule.h`
29. `modules/web/web.pri`
30. `web/tests/qt_web_tests.pro`
