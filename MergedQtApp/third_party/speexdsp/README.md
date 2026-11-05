# SpeexDSP 公共接口快照

维护人：Du lin；日期：2026-10-31。

来源：[Xiph SpeexDSP，SpeexDSP-1.2.1 标签](https://github.com/xiph/speexdsp/tree/SpeexDSP-1.2.1)。

本目录保留上游 `speex_echo.h`、`speex_preprocess.h`、`speexdsp_types.h` 和 `COPYING` 的原始内容、署名与许可证；不将其作者替换为项目作者。应用只用这些公共声明通过 QLibrary 加载实际运行库，不在本目录复制或改写 AEC 算法。

`speexdsp_config_types.h` 为本项目自有的构建适配，使用标准定宽整数提供 Linux 所需的类型配置；Windows 按上游头文件自有分支使用对应类型。兼容头不替代实际运行库。

本次下载的官方标签归档：

- URL：`https://codeload.github.com/xiph/speexdsp/tar.gz/refs/tags/SpeexDSP-1.2.1`
- SHA256：`d17ca363654556a4ff1d02cc13d9eb1fc5a8642c90b40bd54ce266c3807b91a7`

| 上游头文件 | 本次原始SHA256 |
| --- | --- |
| `speex_echo.h` | `ab18db97564a241c9e06d723351c2155bf04a23ac30e0c27a3d86dcc04de3426` |
| `speex_preprocess.h` | `87066ed01639bbc8d3297bb58dbd55bfe1b9cd6e26fc68a1b7f87aa9b61fa9c3` |
| `speexdsp_types.h` | `4f97187c057fd8493c787f2526752859cf57007d50fef8d2e03c9b79b2fed490` |

部署库及分发二进制时应一并保留COPYING中的许可和归属声明。Ubuntu交叉编译、ARM64库文件和配置方式见 `docs/voice_call_v2_speexdsp_20261031.md`。
