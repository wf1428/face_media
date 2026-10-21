# Ycest RAR 测试包

四个归档由本项目自建的小型资源目录使用 WinRAR 6.10 命令行生成，不包含第三方业务资源。
测试运行时只读取这些固定包，不要求安装 WinRAR，也不会安装到开发板。

```text
Ycest/logo/new.txt             UTF-8 文本：new resource + 换行
Ycest/logo/层级/中文.txt       UTF-8 文本：中文资源 + 换行，重复 1024 次
Ycest/logo/empty/              空目录
```

| 文件 | 格式 / 用途 |
| --- | --- |
| ycest-rar4.rar | RAR4，普通压缩 |
| ycest-rar5.rar | RAR5，普通压缩 |
| ycest-rar5-solid.rar | RAR5，固实压缩 |
| ycest-rar5-password.rar | RAR5，加密文件数据，应拒绝更新 |

生成方式（当前目录包含上面的 `Ycest`）：

```text
rar a -idq -y -r -m3 -md4m -ma4 ycest-rar4.rar Ycest
rar a -idq -y -r -m3 -md4m -ma5 ycest-rar5.rar Ycest
rar a -idq -y -r -m3 -md4m -ma5 -s ycest-rar5-solid.rar Ycest
rar a -idq -y -r -m3 -md4m -ma5 -pTestFixtureOnly ycest-rar5-password.rar Ycest
```

测试会另外在隔离临时目录构造损坏、路径穿越和 Unix 链接用例。
