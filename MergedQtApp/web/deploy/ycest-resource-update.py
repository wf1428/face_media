#!/usr/bin/env python3
"""@file ycest-resource-update.py
@brief 安全解压 Ycest 资源包；仅替换包中提供的一级目录，缺失目录保持原样。
@author Du lin
@date 2026-10-20

文件先写入目标文件系统的暂存目录；目录替换保留回滚日志，失败或下次恢复时还原。
成功后通过独立 systemd 定时任务调用原 Weston 入口重启 Qt；不修改挂载与网络/MQTT 逻辑。
"""

import argparse
import contextlib
import ctypes
import ctypes.util
import json
import locale
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import tarfile
import zipfile

MAX_EXPANDED = 2 * 1024**3
MAX_ENTRIES = 20000
RESERVE = 64 * 1024**2
WORK = ".ycest-update"
RAR_SIGNATURES = (b"Rar!\x1a\x07\x00", b"Rar!\x1a\x07\x01\x00")


class RarArchive:
    """通过系统 libarchive 读取单卷 RAR4/RAR5，不让解压库直接写磁盘。"""

    def __init__(self, path=None):
        """延迟加载 RAR 库；ZIP/TAR 更新不依赖该库，也无需 pip 模块。"""
        library = (
            "archiveint.dll"
            if os.name == "nt"
            else ctypes.util.find_library("archive") or "libarchive.so.13"
        )
        try:
            self.library = ctypes.CDLL(library)
            self._bind("archive_read_new", ctypes.c_void_p)
            self._bind("archive_read_free", ctypes.c_int, ctypes.c_void_p)
            for name in (
                "archive_read_support_filter_none",
                "archive_read_support_format_rar",
                "archive_read_support_format_rar5",
            ):
                self._bind(name, ctypes.c_int, ctypes.c_void_p)
            self._bind(
                "archive_read_open_filename",
                ctypes.c_int,
                ctypes.c_void_p,
                ctypes.c_char_p,
                ctypes.c_size_t,
            )
            if os.name == "nt":
                self._bind(
                    "archive_read_open_filename_w",
                    ctypes.c_int,
                    ctypes.c_void_p,
                    ctypes.c_wchar_p,
                    ctypes.c_size_t,
                )
            self._bind(
                "archive_read_next_header",
                ctypes.c_int,
                ctypes.c_void_p,
                ctypes.POINTER(ctypes.c_void_p),
            )
            self._bind(
                "archive_read_data",
                ctypes.c_ssize_t,
                ctypes.c_void_p,
                ctypes.c_void_p,
                ctypes.c_size_t,
            )
            self._bind("archive_error_string", ctypes.c_char_p, ctypes.c_void_p)
            for name in (
                "archive_entry_pathname_utf8",
                "archive_entry_symlink",
                "archive_entry_hardlink",
            ):
                self._bind(name, ctypes.c_char_p, ctypes.c_void_p)
            for name in (
                "archive_entry_filetype",
                "archive_entry_is_encrypted",
                "archive_entry_size_is_set",
            ):
                self._bind(name, ctypes.c_int, ctypes.c_void_p)
            self._bind("archive_entry_size", ctypes.c_int64, ctypes.c_void_p)
            self._bind("archive_version_string", ctypes.c_char_p)
        except (OSError, AttributeError) as error:
            raise ValueError(
                "RAR 解压库不可用，请安装支持 RAR4/RAR5 的 libarchive13"
            ) from error
        self.path = path
        self.handle = None

    def _bind(self, name, result, *arguments):
        """明确 C ABI 的指针和整数宽度，避免 ARM64 上的指针截断。"""
        function = getattr(self.library, name)
        function.restype = result
        function.argtypes = arguments

    def _check(self, code):
        """错误或部分成功都拒绝发布，避免把不完整 RAR 当作完整更新。"""
        if code != 0:
            message = (
                self.library.archive_error_string(self.handle) or b"invalid archive"
            )
            raise ValueError(
                "RAR 解压失败：" + message.decode("utf-8", errors="replace")
            )

    def __enter__(self):
        """只注册 RAR 解码器并打开单个归档，不执行系统解压命令。"""
        self.handle = self.library.archive_read_new()
        if not self.handle:
            raise ValueError("RAR 解压器内存不足")
        try:
            self._check(self.library.archive_read_support_filter_none(self.handle))
            self._check(self.library.archive_read_support_format_rar(self.handle))
            self._check(self.library.archive_read_support_format_rar5(self.handle))
            if self.path is None:
                return self
            if os.name == "nt":
                code = self.library.archive_read_open_filename_w(
                    self.handle, str(self.path), 65536
                )
            else:
                code = self.library.archive_read_open_filename(
                    self.handle, os.fsencode(self.path), 65536
                )
            self._check(code)
            return self
        except Exception:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *_):
        """成功或异常退出均释放 C 归档对象和文件句柄。"""
        if self.handle:
            self.library.archive_read_free(self.handle)
            self.handle = None

    def entries(self):
        """逐个读取元数据，拒绝加密、链接和特殊文件。"""
        entry = ctypes.c_void_p()
        while True:
            code = self.library.archive_read_next_header(
                self.handle, ctypes.byref(entry)
            )
            if code == 1:  # ARCHIVE_EOF
                return
            self._check(code)
            if self.library.archive_entry_is_encrypted(entry):
                raise ValueError("资源包不支持加密 RAR，请取消密码后重新打包")
            if self.library.archive_entry_symlink(
                entry
            ) or self.library.archive_entry_hardlink(entry):
                raise ValueError("压缩包不允许链接或特殊文件")
            kind = self.library.archive_entry_filetype(entry)
            if kind not in (stat.S_IFREG, stat.S_IFDIR):
                raise ValueError("压缩包不允许链接或特殊文件")
            name = self.library.archive_entry_pathname_utf8(entry)
            if not name:
                raise ValueError("RAR 文件名不能转换为 UTF-8")
            if kind == stat.S_IFREG and not self.library.archive_entry_size_is_set(
                entry
            ):
                raise ValueError("RAR 文件长度不可用")
            yield name.decode(
                "utf-8"
            ), kind == stat.S_IFDIR, self.library.archive_entry_size(entry)

    def read(self, size):
        """每次最多读取 1 MiB，并由库核验文件数据和 CRC。"""
        size = min(size, 1024**2)
        if size <= 0:
            return b""
        buffer = ctypes.create_string_buffer(size)
        count = self.library.archive_read_data(self.handle, buffer, size)
        if count < 0:
            self._check(count)
        return buffer.raw[:count]


@contextlib.contextmanager
def rar_utf8_locale():
    """在独立解压进程使用 UTF-8，兼容系统服务的 C 环境及中文资源名。"""
    previous = locale.setlocale(locale.LC_CTYPE)
    for candidate in ("C.UTF-8", "C.utf8", ".UTF-8"):
        try:
            locale.setlocale(locale.LC_CTYPE, candidate)
            break
        except locale.Error:
            continue
    else:
        raise ValueError("RAR 文件名处理需要 UTF-8 locale，请启用 C.UTF-8")
    try:
        yield
    finally:
        locale.setlocale(locale.LC_CTYPE, previous)


@contextlib.contextmanager
def rar_memory_budget():
    """Linux 限制独立解压进程地址空间，阻止超大 RAR 字典挤占 Qt 内存。"""
    if os.name != "posix":
        yield
        return
    import resource

    previous = resource.getrlimit(resource.RLIMIT_AS)
    budget = 256 * 1024**2
    soft = budget if previous[0] == resource.RLIM_INFINITY else min(previous[0], budget)
    resource.setrlimit(resource.RLIMIT_AS, (soft, previous[1]))
    try:
        yield
    finally:
        resource.setrlimit(resource.RLIMIT_AS, previous)


class ResourceTarInfo(tarfile.TarInfo):
    """限制 TAR 扩展头大小，避免解压元数据耗尽设备内存。"""

    def _proc_pax(self, package):
        """PAX 元数据只允许小型文本头。"""
        if self.size > 65536:
            raise ValueError("TAR 扩展头过大")
        return super()._proc_pax(package)

    def _proc_gnulong(self, package):
        """GNU 长文件名头同样受限。"""
        if self.size > 65536:
            raise ValueError("TAR 文件名头过大")
        return super()._proc_gnulong(package)

    def _proc_sparse(self, package):
        """资源包不使用 GNU 稀疏文件，禁止无界扩展头解析。"""
        raise ValueError("资源包不支持稀疏文件")


def save_json(path, data):
    """原子持久化状态，读者不会看到截断的 JSON。"""
    temporary = path.with_name(path.name + ".new")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(data, stream, ensure_ascii=False)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(str(temporary), str(path))
    sync_dir(path.parent)


def sync_dir(path):
    """Linux 上同步目录项；Windows 测试环境无目录 fsync。"""
    if os.name != "nt":
        descriptor = os.open(str(path), os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)


def remove_child(path, root):
    """只删除指定根目录的直接子项，不沿符号链接递归。"""
    if path.parent.resolve() != root.resolve() or path.name in ("", ".", ".."):
        raise ValueError("清理路径越界")
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.exists():
        shutil.rmtree(str(path))


@contextlib.contextmanager
def exclusive_lock(root):
    """跨进程非阻塞互斥；退出后由操作系统释放锁。"""
    lock = root / ".ycest-update.lock"
    if lock.is_symlink():
        raise ValueError("资源锁不能是符号链接")
    with lock.open("a+b") as stream:
        if os.name == "nt":
            import msvcrt

            stream.seek(0)
            stream.write(b"0")
            stream.flush()
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl

            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def entry_path(name, is_directory):
    """验证归档成员只能位于 Ycest 的一级目录内，拒绝路径穿越。"""
    if "\\" in name or "\x00" in name or ":" in name:
        raise ValueError("压缩包包含非法路径")
    parts = name.rstrip("/").split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise ValueError("压缩包包含非法路径")
    if not parts or parts[0] != "Ycest":
        raise ValueError("压缩包根目录必须为 Ycest")
    if len(parts) == 1:
        if not is_directory:
            raise ValueError("Ycest 必须为目录")
        return None
    if parts[1] in (WORK, WORK + ".lock"):
        raise ValueError("资源目录不能使用系统保留名称")
    if len(parts) == 2 and not is_directory:
        raise ValueError("Ycest 下只能提供资源目录")
    return PurePosixPath(*parts[1:])


def unpack(archive, stage):
    """流式解压普通文件和目录；限制条目、展开大小及剩余空间。"""
    names, directories = set(), set()
    total, count = 0, 0

    def extract(name, directory, size, opener):
        """逐项校验和落盘，不使用归档库的自动提取路径。"""
        nonlocal total, count
        count += 1
        if count > MAX_ENTRIES:
            raise ValueError("压缩包条目过多")
        relative = entry_path(name, directory)
        if relative is None:
            return
        key = str(relative)
        if key in names:
            raise ValueError("压缩包包含重复路径")
        names.add(key)
        directories.add(relative.parts[0])
        target = stage.joinpath(*relative.parts)
        if directory:
            target.mkdir(parents=True, exist_ok=True)
            return
        total += size
        if size < 0 or total > MAX_EXPANDED:
            raise ValueError("压缩包展开后超过 2 GiB")
        if shutil.disk_usage(str(stage)).free < size + RESERVE:
            raise ValueError("资源存储空间不足，原资源未修改")
        target.parent.mkdir(parents=True, exist_ok=True)
        with opener() as source, target.open("xb") as destination:
            remaining = size
            while remaining:
                block = source.read(min(1024**2, remaining))
                if not block:
                    raise ValueError("压缩包内容不完整")
                destination.write(block)
                remaining -= len(block)
            if source.read(1):
                raise ValueError("压缩包长度不一致")
            destination.flush()
            os.fsync(destination.fileno())

    with archive.open("rb") as signature:
        rar = signature.read(8).startswith(RAR_SIGNATURES)
    if rar:
        # 上传缓存以 .part 结尾，按真实文件头识别 RAR，复用逐项路径/容量校验。
        with rar_utf8_locale(), rar_memory_budget(), RarArchive(archive) as package:
            for name, directory, size in package.entries():
                extract(name, directory, size, lambda: contextlib.nullcontext(package))
    elif zipfile.is_zipfile(str(archive)):
        # 中央目录先限长，避免大量条目在设备上占满内存。
        with archive.open("rb") as stream:
            stream.seek(max(0, archive.stat().st_size - 65557))
            tail = stream.read()
        marker = tail.rfind(b"PK\x05\x06")
        if marker < 0 or len(tail) - marker < 22:
            raise ValueError("ZIP 目录损坏")
        metadata = tail[marker:]
        entries = int.from_bytes(metadata[10:12], "little")
        central_size = int.from_bytes(metadata[12:16], "little")
        if entries > MAX_ENTRIES or central_size > 16 * 1024**2:
            raise ValueError("ZIP 目录过大或不支持 ZIP64")
        with zipfile.ZipFile(str(archive)) as package:
            for member in package.infolist():
                mode = member.external_attr >> 16
                if (
                    member.flag_bits & 1
                    or stat.S_ISLNK(mode)
                    or (stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR))
                ):
                    raise ValueError("压缩包不允许加密、链接或特殊文件")
                extract(
                    member.filename,
                    member.is_dir(),
                    member.file_size,
                    lambda member=member: package.open(member),
                )
    else:
        with tarfile.open(str(archive), mode="r|*", tarinfo=ResourceTarInfo) as package:
            for member in package:
                if not (member.isfile() or member.isdir()):
                    raise ValueError("压缩包不允许链接或特殊文件")
                extract(
                    member.name,
                    member.isdir(),
                    member.size,
                    lambda member=member: package.extractfile(member),
                )
    if not directories:
        raise ValueError("Ycest 下没有可更新的资源目录")
    # 在开始替换前同步全部新目录及其子目录。
    for current, children, _ in os.walk(str(stage), topdown=False):
        for child in children:
            sync_dir(Path(current) / child)
        sync_dir(Path(current))
    return sorted(directories)


def recover(root):
    """按照持久化日志恢复中断的目录替换；已提交的更新只清理备份。"""
    work = root / WORK
    if work.is_symlink():
        raise ValueError("资源暂存目录不能是符号链接")
    journal = work / "journal.json"
    data = None
    if journal.exists():
        data = json.loads(journal.read_text(encoding="utf-8"))
        if data["phase"] != "COMMITTED":
            for item in reversed(data["directories"]):
                name = item["name"]
                entry_path("Ycest/" + name, True)
                if "/" in name:
                    raise ValueError("恢复日志目录无效")
                target, backup = root / name, work / "backup" / name
                if backup.exists():
                    remove_child(target, root)
                    os.replace(str(backup), str(target))
                elif not item["existed"] and not (work / "stage" / name).exists():
                    remove_child(target, root)
            sync_dir(root)
    if work.exists():
        remove_child(work, root)
    return data


def apply(archive, root, result):
    """先解压，再逐目录备份及替换；任何未提交失败均回滚。"""
    if root.is_symlink() or root.absolute() != root.resolve():
        raise ValueError("资源目标目录不能包含符号链接")
    root.mkdir(parents=True, exist_ok=True)
    with exclusive_lock(root):
        recover(root)
        work = root / WORK
        stage, backup = work / "stage", work / "backup"
        stage.mkdir(parents=True)
        backup.mkdir()
        try:
            save_json(result, {"state": "VALIDATING"})
            directories = unpack(archive, stage)
            entries = []
            for name in directories:
                target = root / name
                if target.is_symlink() or (target.exists() and not target.is_dir()):
                    raise ValueError("目标资源必须为普通目录：" + name)
                entries.append({"name": name, "existed": target.exists()})
            journal = {
                "phase": "REPLACING",
                "directories": entries,
                "result": str(result),
            }
            save_json(work / "journal.json", journal)
            save_json(result, {"state": "COMMITTING"})
            for item in entries:
                name = item["name"]
                if item["existed"]:
                    os.replace(str(root / name), str(backup / name))
                    sync_dir(root)
                    sync_dir(backup)
                os.replace(str(stage / name), str(root / name))
                sync_dir(stage)
                sync_dir(root)
            journal["phase"] = "COMMITTED"
            save_json(work / "journal.json", journal)
        except Exception:
            recover(root)
            raise
        # 成功后清除旧目录，避免备份长期占用 eMMC；提交状态已写盘。
        save_json(result, {"state": "READY", "updatedDirectories": directories})
        try:
            recover(root)
        except OSError:
            # 更新已经提交；残留备份由下次恢复清理，不误报更新失败。
            pass


def schedule_restart(command, result):
    """提交独立 systemd 定时任务；资源已更新时单独报告重启调度失败。"""
    details = json.loads(result.read_text(encoding="utf-8"))
    try:
        subprocess.run(
            command,
            check=True,
            timeout=5,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        details["restartScheduled"] = True
    except (OSError, subprocess.SubprocessError) as error:
        details["restartScheduled"] = False
        details["restartError"] = "资源已更新，但 Qt 自动重启调度失败：" + str(error)
    save_json(result, details)


def main():
    """固定部署入口；路径来自 Qt 配置，不接受浏览器指定的目标路径。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--recover", action="store_true")
    parser.add_argument(
        "--check-rar", action="store_true", help="仅检查 RAR 解码库，不更新资源或重启"
    )
    parser.add_argument("--destination", type=Path)
    parser.add_argument("--result", type=Path)
    parser.add_argument(
        "--restart-command", help="Qt 配置提供的固定命令 JSON，禁止由网页指定"
    )
    args = parser.parse_args()
    if args.check_rar:
        try:
            with RarArchive() as decoder:
                print(decoder.library.archive_version_string().decode("utf-8"))
            return 0
        except ValueError as error:
            parser.exit(1, str(error) + "\n")
    if args.destination is None or args.result is None:
        parser.error(
            "更新资源时必须提供 --destination 和 --result；本脚本由 Qt 自动调用"
        )
    try:
        if (
            args.destination.is_symlink()
            or args.destination.absolute() != args.destination.resolve()
        ):
            raise ValueError("资源目标目录不能包含符号链接")
        if args.recover:
            with exclusive_lock(args.destination):
                previous = recover(args.destination)
                if (
                    previous
                    and previous["phase"] == "COMMITTED"
                    and previous.get("result") == str(args.result)
                ):
                    save_json(
                        args.result,
                        {
                            "state": "READY",
                            "updatedDirectories": [
                                item["name"] for item in previous["directories"]
                            ],
                        },
                    )
                else:
                    current = (
                        json.loads(args.result.read_text(encoding="utf-8"))
                        if args.result.exists()
                        else {}
                    )
                    if current.get("state") != "READY":
                        raise ValueError("资源更新中断，未提交的目录已恢复，请重新上传")
        elif args.archive is not None:
            command = json.loads(args.restart_command or "[]")
            if (
                not command
                or not all(isinstance(item, str) for item in command)
                or not shutil.which(command[0])
            ):
                raise ValueError("Qt 自动重启程序不可用，未修改资源")
            apply(args.archive, args.destination, args.result)
            # READY 结果先落盘，任务属于 systemd，Qt 退出不会终止延迟重启。
            schedule_restart(command, args.result)
        else:
            raise ValueError("缺少资源包")
        return 0
    except Exception as error:
        save_json(args.result, {"state": "FAILED", "error": str(error)})
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
