"""@file resource_package_check.py
@brief 开发机造包并执行真实 Qt/C++ 更新测试；不在设备运行，不实现解压业务。
@author Du lin
@date 2026-10-20
"""

import io
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".web-build/native-resource-checks"
BUILD.mkdir(parents=True, exist_ok=True)
EXECUTABLE = Path(
    os.environ.get(
        "WEB_RESOURCE_TEST_EXECUTABLE",
        str(ROOT / ".web-build/qt-tests/release/qt_web_tests.exe"),
    )
)
ENV = {**os.environ}
if os.name == "nt":
    ENV["PATH"] = os.pathsep.join(
        [
            str(ROOT / ".web-build-tools/sdk/5.15.2/mingw81_64/bin"),
            str(ROOT / ".web-build-tools/sdk/Tools/mingw810_64/bin"),
            ENV["PATH"],
        ]
    )
    ENV["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(
        ROOT / ".web-build-tools/sdk/5.15.2/mingw81_64/plugins/platforms"
    )


def rar4_stored(name, body, attributes=stat.S_IFREG | 0o644):
    """独立构造 RAR4 路径/链接边界样本，不包含任何生产解码逻辑。"""

    def block(kind, flags, payload):
        """计算样本头 CRC16。"""
        header = struct.pack("<BHH", kind, flags, len(payload) + 7) + payload
        return struct.pack("<H", zlib.crc32(header) & 65535) + header

    name = name.encode("utf-8")
    header = struct.pack(
        "<IIBIIBBHI",
        len(body),
        len(body),
        3,
        zlib.crc32(body),
        0,
        20,
        0x30,
        len(name),
        attributes,
    )
    return (
        b"Rar!\x1a\x07\x00"
        + block(0x73, 0, b"\0" * 6)
        + block(0x74, 0x8000, header + name)
        + body
        + block(0x7B, 0, b"")
    )


class ResourcePackages(unittest.TestCase):
    """所有删除/替换均由生产 C++ 在明确的隔离临时目录执行。"""

    def setUp(self):
        """准备独立设备资源目录，避免接触实际系统资源。"""
        self.temporary = tempfile.TemporaryDirectory(dir=str(BUILD))
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name).resolve()
        assert self.base.parent == BUILD.resolve()
        self.root = self.base / "images"
        for name in ("logo", "pic", "tl", "video"):
            (self.root / name).mkdir(parents=True)
            (self.root / name / "old.txt").write_text(name)
        self.archive, self.result = self.base / "upload.part", self.base / "result.json"

    def package(self, entries):
        """ZIP 文件名和内容完全由测试数据控制。"""
        with zipfile.ZipFile(str(self.archive), "w", zipfile.ZIP_DEFLATED) as package:
            for name, body in entries.items():
                package.writestr(name, body)

    def run_qt(self, recovery=False):
        """调用 Qt 测试入口；其中直接运行正式 WebResourceUpdate::execute。"""
        process = subprocess.run(
            [
                str(EXECUTABLE),
                "resource-recover" if recovery else "resource-task",
                str(self.archive),
                str(self.root),
                str(self.result),
            ],
            capture_output=True,
            env=ENV,
            timeout=60,
        )
        self.assertIn(
            process.returncode, (0, 1), process.stderr.decode(errors="replace")
        )
        result = json.loads(process.stdout.decode("utf-8"))
        self.assertEqual(result["state"] == "READY", process.returncode == 0, result)
        return result

    def unchanged(self):
        """逐个核对旧资源，避免只检查接口状态而遗漏误删除。"""
        for name in ("logo", "pic", "tl", "video"):
            self.assertEqual((self.root / name / "old.txt").read_text(), name)

    def test_selective_replacement(self):
        """替换提供的 logo 并新增目录，缺失的三个目录保持原样。"""
        self.package(
            {
                "Ycest/logo/new/deep.txt": "中文",
                "Ycest/logo/.hidden": "hidden",
                "Ycest/extra/empty/": "",
            }
        )
        result = self.run_qt()
        self.assertEqual(result["state"], "READY", result)
        self.assertEqual(result["updatedDirectories"], ["extra", "logo"])
        self.assertTrue(result["restartScheduled"])
        self.assertFalse((self.root / "logo/old.txt").exists())
        self.assertEqual(
            (self.root / "logo/new/deep.txt").read_text(encoding="utf-8"), "中文"
        )
        self.assertTrue((self.root / "extra/empty").is_dir())
        for name in ("pic", "tl", "video"):
            self.assertEqual((self.root / name / "old.txt").read_text(), name)
        self.assertFalse((self.root / "Ycest").exists())
        self.assertFalse((self.root / ".ycest-update").exists())

    def test_tar_gz_and_empty_directory(self):
        """TAR.GZ 的空目录和视频子目录同样按一级目录替换。"""
        with tarfile.open(str(self.archive), "w:gz") as package:
            directory = tarfile.TarInfo("Ycest/pic")
            directory.type = tarfile.DIRTYPE
            package.addfile(directory)
            file = tarfile.TarInfo("Ycest/video/nested/new.mp4")
            file.size = 5
            package.addfile(file, io.BytesIO(b"video"))
        self.assertEqual(self.run_qt()["state"], "READY")
        self.assertEqual(list((self.root / "pic").iterdir()), [])
        self.assertEqual((self.root / "video/nested/new.mp4").read_bytes(), b"video")
        self.assertTrue((self.root / "logo/old.txt").exists())

    def test_real_rar_formats(self):
        """实际压缩的 RAR4/RAR5/固实包支持中文和空目录。"""
        for name in ("ycest-rar4.rar", "ycest-rar5.rar", "ycest-rar5-solid.rar"):
            with self.subTest(name=name):
                shutil.copyfile(ROOT / "web/tests/fixtures" / name, self.archive)
                (self.root / "logo/old.txt").write_text("old")
                result = self.run_qt()
                self.assertEqual(result["state"], "READY", result)
                self.assertEqual(
                    (self.root / "logo/层级/中文.txt").read_text(encoding="utf-8"),
                    "中文资源\n" * 1024,
                )
                self.assertFalse((self.root / "logo/old.txt").exists())
                self.assertTrue((self.root / "logo/empty").is_dir())

    def test_invalid_paths_and_roots(self):
        """坏路径、错误根目录和根层文件都在正式目录替换前拒绝。"""
        for name in (
            "Ycest/../escape",
            "/Ycest/logo/x",
            "Ycest/file",
            "Other/logo/a",
            "Ycest/.ycest-update/x",
        ):
            with self.subTest(name=name):
                self.package({"Ycest/logo/new.txt": "new", name: "bad"})
                self.assertEqual(self.run_qt()["state"], "FAILED")
                self.unchanged()

    def test_links_rejected(self):
        """ZIP 符号链接、TAR 硬链接、RAR 链接都不能发布。"""
        member = zipfile.ZipInfo("Ycest/logo/link")
        member.create_system = 3
        member.external_attr = (stat.S_IFLNK | 0o777) << 16
        with zipfile.ZipFile(str(self.archive), "w") as package:
            package.writestr(member, "../../outside")
        self.assertEqual(self.run_qt()["state"], "FAILED")
        with tarfile.open(str(self.archive), "w") as package:
            member = tarfile.TarInfo("Ycest/logo/link")
            member.type, member.linkname = tarfile.LNKTYPE, "/etc/passwd"
            package.addfile(member)
        self.assertEqual(self.run_qt()["state"], "FAILED")
        self.archive.write_bytes(
            rar4_stored("Ycest/logo/link", b"../../outside", stat.S_IFLNK | 0o777)
        )
        self.assertEqual(self.run_qt()["state"], "FAILED")
        self.unchanged()

    def test_encrypted_and_corrupt_rar(self):
        """加密或损坏 RAR 不会触发重启。"""
        for name in ("ycest-rar5-password.rar", "ycest-rar5.rar"):
            data = (ROOT / "web/tests/fixtures" / name).read_bytes()
            self.archive.write_bytes(
                data if "password" in name else data[: len(data) // 2]
            )
            self.assertEqual(self.run_qt()["state"], "FAILED")
            self.assertFalse((self.base / "restart.log").exists())
            self.unchanged()

    def test_duplicate_paths_and_corrupt_data(self):
        """不完整包和重名条目不得误报成功。"""
        self.archive.write_bytes(b"not an archive")
        self.assertEqual(self.run_qt()["state"], "FAILED")
        with zipfile.ZipFile(str(self.archive), "w") as package:
            package.writestr("Ycest/logo/a", "one")
            package.writestr("Ycest/logo/a", "two")
        self.assertEqual(self.run_qt()["state"], "FAILED")
        self.unchanged()

    def test_zip_data_descriptor(self):
        """非寻址写入的普通 ZIP 也可更新，不能依赖本地头预先填写长度。"""

        class Streaming(io.BytesIO):
            """强制 zipfile 使用 data descriptor。"""

            def seek(self, *args):
                """模拟手机或网络流无法回填 ZIP 头的写入环境。"""
                raise io.UnsupportedOperation()

        stream = Streaming()
        with zipfile.ZipFile(stream, "w", zipfile.ZIP_DEFLATED) as package:
            package.writestr("Ycest/logo/new.txt", "descriptor" * 1024)
        self.archive.write_bytes(stream.getvalue())
        result = self.run_qt()
        self.assertEqual(result["state"], "READY", result)

    def test_expansion_limit(self):
        """超大声明长度不会真的分配 2 GiB，也不会清理原资源。"""
        entry = tarfile.TarInfo("Ycest/logo/oversized.bin")
        entry.size = 2 * 1024**3 + 1
        self.archive.write_bytes(entry.tobuf() + b"\0" * 1024)
        self.assertEqual(self.run_qt()["state"], "FAILED")
        self.unchanged()

    def test_interrupted_recovery(self):
        """兼容旧事务日志，启动恢复只回滚，不重新解压或安排重启。"""
        work = self.root / ".ycest-update"
        (work / "backup").mkdir(parents=True)
        (work / "stage").mkdir()
        (self.root / "logo").rename(work / "backup/logo")
        (self.root / "logo").mkdir()
        (self.root / "logo/new").write_text("new")
        (work / "journal.json").write_text(
            json.dumps(
                {
                    "phase": "REPLACING",
                    "directories": [{"name": "logo", "existed": True}],
                }
            )
        )
        self.assertEqual(self.run_qt(True)["state"], "FAILED")
        self.unchanged()
        self.assertFalse((self.base / "restart.log").exists())

    def test_committed_recovery(self):
        """文件提交后 Qt 尚未登记时，只登记结果，不回滚已提交更新。"""
        work = self.root / ".ycest-update"
        (work / "backup").mkdir(parents=True)
        (self.root / "logo").rename(work / "backup/logo")
        (self.root / "logo").mkdir()
        (self.root / "logo/new").write_text("new")
        (work / "journal.json").write_text(
            json.dumps(
                {
                    "phase": "COMMITTED",
                    "result": str(self.result).replace("\\", "/"),
                    "directories": [{"name": "logo", "existed": True}],
                }
            )
        )
        self.assertEqual(self.run_qt(True)["state"], "READY")
        self.assertTrue((self.root / "logo/new").exists())
        self.assertFalse((self.base / "restart.log").exists())


def fixtures():
    """为 Qt 上传及回滚集成测试造包；设备不部署这个测试脚本。"""
    directory = ROOT / ".web-build/fixtures"
    directory.mkdir(exist_ok=True)
    for name, content in (
        ("resource.zip", {"Ycest/logo/new.txt": "new resource"}),
        ("resource-bad.zip", {"Ycest/../escape.txt": "bad"}),
        (
            "resource-pair.zip",
            {"Ycest/logo/new.txt": "new", "Ycest/pic/new.txt": "new"},
        ),
    ):
        with zipfile.ZipFile(str(directory / name), "w") as package:
            for path, body in content.items():
                package.writestr(path, body)
    shutil.copyfile(
        ROOT / "web/tests/fixtures/ycest-rar5.rar", directory / "resource.rar"
    )


if __name__ == "__main__":
    fixtures()
    unittest.main()
