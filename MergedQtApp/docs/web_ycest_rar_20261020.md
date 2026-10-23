# Ycest RAR 支持（已迁入 Qt）

2026-10-21 起，资源更新使用 Qt 内部 C++ 模块，不再使用 Python 更新脚本。
旧脚本及其安装检查已移除；无需手工启动解压脚本或新增服务。

RAR4/RAR5 继续通过系统 libarchive 读取，同时支持 ZIP、TAR、TAR.GZ/TGZ。
libarchive 的构建头文件与目标设备库由设备方准备，服务器程序不引入该库。
Qt 运行时按需加载库；当前仅更新资源时使用，不影响其他 Qt 功能。

调用链、设备进度、目录事务、网页文案、编译和部署见
[Qt 内部资源更新说明](web_ycest_resources_20261020.md)。

`web/tests/fixtures` 的 RAR4/RAR5/固实/加密样本继续用于原生 Qt 测试。
开发机的 Python 测试脚本只负责造包及启动 Qt 测试，不实现或部署解压业务。
