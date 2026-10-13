<!-- @file README.md | @brief 固定版本依赖与许可证，不改第三方源码注释。 @author Du lin @date 2026-10-09 -->

本目录保留上游完整源码/版权注释，属于第三方依赖，不套用项目作者署名。

| 文件 | 上游版本与来源 | SHA256 |
| --- | --- | --- |
| httplib.h | [cpp-httplib v0.18.7](https://github.com/yhirose/cpp-httplib/tree/v0.18.7)，MIT | `4770f8ea3d3fcd27b0b713c783d9b53bce3baddbd5adad13714011df96142526` |
| json.hpp | [nlohmann/json v3.11.3](https://github.com/nlohmann/json/tree/v3.11.3)，MIT | `9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6` |

HTTPS 构建需要 OpenSSL 3.0 及以上；Qt 本身不因此改为 OpenSSL 3。交叉编译必须给独立 daemon 提供对应版本的 sysroot，不能混用主机库。
