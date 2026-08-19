# 人脸图片同步质量校验实现说明

日期：2026-08-06  
范围：online_v1 `face.requestImages` / `face.responseImages` 后续链路  
验证方式：仅静态复核，未编译、未运行

## 1. 实现目标

平台返回 `face.responseImages` 后，设备先把图片保存到人员对应目录，再逐张执行：

1. 图片文件和图片格式检查；
2. 单人脸检测；
3. 人脸检测置信度达到 `faceDetectThreshold`；
4. 人脸质量达到 `faceQualityThreshold`；
5. 人脸特征提取成功。

此同步校验链路不执行活体检测。只有该人员的全部图片都通过，才确认人员图片同步成功。

## 2. 状态与数据流

`face.responseImages` → Base64 解码和图片落盘 → `IMAGE_PENDING_VALIDATION` / `FACE_VALIDATING` → FaceGate 推理线程离线校验 → 全部通过后保存特征 → `IMAGE_READY` / `READY` → 发布 `Imagesresult`。

图片质量校验运行在 FaceGate 推理线程中，不改变摄像头实时识别、活体状态机、MQTT topic 选择和二维码/IC 卡逻辑。

## 3. 成功条件

成功必须同时满足：

- 响应中的每张图片都能安全落盘；
- 每张图片只检测到一张人脸；
- 每张图片的检测置信度和人脸质量均达到当前配置阈值；
- 每张图片均成功提取非空特征及模型版本；
- 数据库中该人员不存在未达到 `IMAGE_READY` 的人脸记录；
- 特征和人员最终状态事务提交成功。

成功后 `network_face` 保存图片路径、特征、模型版本及 `IMAGE_READY` 状态，`network_person.sync_state` 更新为 `READY`。

## 4. `Imagesresult` 回执

回执发布到配置的 `device/ycLinux/.../event` 主题。基本信封字段沿用现有 online_v1 格式：`method`、`id`、`deviceId`、`data`、`time`。

```json
{
  "method": "Imagesresult",
  "id": "response-message-id",
  "deviceId": "device-id",
  "data": {
    "personId": "person-id",
    "result": true,
    "msg": "人脸图像同步成功。"
  },
  "time": "2026-08-06 12:00:00"
}
```

失败时 `result` 为 `false`，`msg` 返回具体失败原因。主要可见提示包括：

| 失败类型 | `msg` 示例 |
|---|---|
| 未检测到人脸 | `未检测到人脸，请重新录入人脸。` |
| 多张人脸 | `图像中检测到多张人脸，请重新录入单人脸图像。` |
| 人脸检测置信度不足 | `人脸识别质量过低，请重新录入人脸。` |
| 人脸质量不足 | `人脸质量过低，请重新录入人脸。` |
| 特征提取失败 | `人脸特征提取失败，请重新录入人脸。` |
| 服务不可用 | `人脸识别服务未就绪，请稍后重新录入人脸。` |
| 超时 | `人脸图像校验超时，请重新录入人脸。` |

## 5. 失败回滚

任一图片失败即视为该人员整批失败。回滚事务删除该人员的 `network_person`、`network_face`、IC 卡、二维码、规则、楼层、使用量、二维码事务、同步幂等记录和 tombstone，并在事务成功后清理 `network_faces/<personId>` 目录。

删除 `sync_inbox` 的目的，是允许平台修正图片后重新下发同一个人员；否则相同请求可能被幂等记录直接命中。

文件清理限制在根据安全化 `personId` 计算出的人员图片目录内，避免删除目录外文件。

## 6. 并发与超时

- 同一人员已有图片校验任务时，重复响应不会覆盖正在处理的数据，而是返回“正在校验”。
- 每次任务使用独立 token 关联请求与推理结果。
- 30 秒未返回校验结果时按失败处理并回滚。
- 超时后的迟到结果会被忽略，避免再次修改已经回滚的数据。

## 7. 当前边界

同步图片提取出的特征保存在 `network_face`，用于确认同步图片合格；本次没有把网络人员自动加入 FaceGate 本地识别 gallery。因此现有闸机识别人员来源和其他业务逻辑保持不变。

## 8. 静态复核项

- 新增信号、槽、声明、定义和调用均有对应引用；
- 新增 Qt 类型具备直接头文件，并注册跨线程使用的 `QJsonArray`、`QStringList`；
- 质量校验调用 `extractFeatureFromImage`，调用链中没有活体检测接口；
- 成功前检查全部 `network_face` 状态，部分图片响应不能被误判为整体成功；
- 失败路径在数据库提交后再清理限定目录内的图片；
- 未执行编译或运行验证，需在目标板手工确认 InspireFace 模型、阈值配置、SQLite 和 MQTT 联调结果。
