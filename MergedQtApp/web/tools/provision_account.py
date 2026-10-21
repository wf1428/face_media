"""
@file provision_account.py
@brief 出厂生成一次性 Web 账号摘要文件，不修改 SQLite、不传明文密码到命令行。
@author Du lin
@date 2026-10-09
"""

import argparse
import getpass
import hashlib
import json
import os
from pathlib import Path


def main() -> None:
    """通过隐藏输入生成 PBKDF2-SHA256 摘要；禁止覆盖已有工厂文件。"""
    parser = argparse.ArgumentParser(
        description="Generate factory Web account hash for Qt first boot"
    )
    parser.add_argument("--username", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    username = args.username.strip()
    if not username or len(username) > 80:
        parser.error("username must contain 1..80 characters")
    password = getpass.getpass("Web account password: ")
    if password != getpass.getpass("Confirm password: "):
        parser.error("passwords do not match")
    if not 10 <= len(password) <= 256:
        parser.error("password must contain 10..256 characters")

    # 出厂文件只保存随机盐和派生摘要，不保存输入的明文密码。
    salt = os.urandom(32)
    iterations = 210000
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, iterations)
    payload = {
        "_comment": (
            "Factory PBKDF2-SHA256 credential; no plaintext password. "
            "Used only when web_accounts is empty."
        ),
        "username": username,
        "salt_hex": salt.hex(),
        "hash_hex": digest.hex(),
        "iterations": iterations,
    }

    # 使用独占创建，避免误覆盖已经发放给设备的账号文件。
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(args.output, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as file:
        json.dump(payload, file, ensure_ascii=False, indent=2)
        file.write("\n")
    print("Factory account hash file created; install it for the Qt service owner.")


if __name__ == "__main__":
    main()
