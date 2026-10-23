## @file portal_network_check.py
# @brief 在隔离目录中验证门户网络部署脚本，不操作开发机实际网络。

"""Run portal shell scripts with isolated fake NM/firewall tools; never changes networking."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time


## @brief 模拟 NetworkManager、iptables 和系统工具，所有状态写入隔离测试目录。
def driver():
    """模拟 NetworkManager、iptables 和系统工具，所有状态写入隔离测试目录。"""
    root = Path(os.environ["PORTAL_TEST_ROOT"])
    state_file = root / "state.json"
    state = json.loads(state_file.read_text(encoding="utf-8"))
    tool, *args = sys.argv[2:]
    state["calls"].append([tool, *args])
    result, code = "", 0
    if tool == "timeout":
        state_file.write_text(json.dumps(state), encoding="utf-8")
        sys.argv[2:] = args[1:]
        driver()
    elif tool == "systemctl":
        if args == ["is-active", "--quiet", "device-webd.service"]:
            pass
        elif args == ["reload", "device-webd.service"]:
            state_file.write_text(json.dumps(state), encoding="utf-8")
            result = subprocess.run(
                [os.environ["PORTAL_TEST_SHELL"], os.environ["PORTAL_TEST_NETWORK"], "refresh"]
            )
            raise SystemExit(result.returncode)
        else:
            raise AssertionError("Unexpected systemd operation: " + str(args))
    elif tool == "install":
        assert args[:2] == ["-d", "-m"]
        for path in args[3:]:
            Path(path).mkdir(parents=True, exist_ok=True)
    elif tool == "chmod":
        pass
    elif tool == "sleep":
        time.sleep(0.01)
    elif tool == "id":
        result = "0"
    elif tool == "flock":
        pass
    elif tool == "ip":
        if state.get("conflict"):
            result = "2: eth0 inet 192.168.10.20/24 scope global eth0"
        elif state["active"]:
            result = f"3: {state['iface']} inet 192.168.10.1/24 scope global {state['iface']}"
    elif tool == "iptables":
        if state.get("firewall_fail"):
            code = 1
        elif "-vnL" in args:
            result = "Chain FORWARD\n1 DROP portal WiFi traffic"
        else:
            operation = next(a for a in args if a in ("-I", "-C", "-D"))
            rule = args[args.index(operation) + 1 :]
            if operation == "-I":
                assert rule[1] == "1"
                rule = [rule[0], *rule[2:]]
                state["rules"].insert(0, rule)
            elif operation == "-C":
                code = 0 if rule in state["rules"] else 1
            elif rule in state["rules"]:
                state["rules"].remove(rule)
            else:
                code = 1
    elif tool == "nmcli":
        assert args[:4] == ["--wait", "15", "--escape", "no"]
        args = args[4:]
        if args[:1] == ["--show-secrets"]:
            args = args[1:]
        if args == ["-t", "-f", "RUNNING", "general"]:
            result = "running"
        elif args == ["radio", "wifi"]:
            result = "enabled"
        elif args == ["-t", "-f", "DEVICE,TYPE", "device", "status"]:
            result = f"{state['iface']}:wifi\neth0:ethernet" if state["wifi"] else "eth0:ethernet"
        elif args[:1] == ["-g"] and "device" in args:
            field = args[1]
            result = {
                "WIFI-PROPERTIES.AP": "yes" if state["ap"] else "no",
                "GENERAL.NM-MANAGED": "yes",
                "GENERAL.CONNECTION": (
                    "uplink-main"
                    if state.get("occupied")
                    else "yc-device-portal" if state["active"] else "--"
                ),
                "IP4.ADDRESS": "192.168.10.1/24" if state["active"] else "",
            }[field]
        elif args[:1] == ["-g"] and "connection" in args:
            if not state["profile"]:
                code = 10
            else:
                result = {
                    "connection.id": "yc-device-portal",
                    "connection.type": "802-11-wireless",
                    "802-11-wireless.mode": state.get("mode", "ap"),
                    "802-11-wireless.ssid": state["ssid"],
                    "802-11-wireless-security.psk": state["password"],
                    "802-11-wireless-security.psk-flags": "0 (none)",
                }[args[1]]
                # 模拟板端实际现象：C locale 会损坏中文，UTF-8 才能完整读回。
                if args[1] == "802-11-wireless.ssid" and os.environ.get("LC_ALL") != "C.UTF-8":
                    result = result.encode("ascii", errors="replace").decode("ascii")
        elif args[:2] == ["connection", "add"]:
            state["profile"] = True
            state["ssid"] = args[args.index("ssid") + 1]
        elif args[:2] == ["connection", "modify"]:
            if "802-11-wireless.ssid" in args:
                state["ssid"] = args[args.index("802-11-wireless.ssid") + 1]
            if "wifi-sec.psk" in args:
                state["password"] = args[args.index("wifi-sec.psk") + 1]
            if "ipv6.method" in args:
                assert args[args.index("ipv6.method") + 1] == "disabled"
            if "ipv4.addresses" in args:
                assert args[args.index("ipv4.addresses") + 1] == "192.168.10.1/24"
            if "connection.autoconnect" in args:
                assert args[args.index("connection.autoconnect") + 1] == "no"
        elif args[:2] == ["connection", "up"]:
            assert state["profile"] and args[-1] == state["iface"]
            state["active"] = True
        elif args[:2] == ["connection", "down"]:
            state["active"] = False
        elif args[:2] == ["connection", "delete"]:
            state["profile"] = False
        else:
            raise AssertionError(args)
    else:
        raise AssertionError(tool)
    state_file.write_text(json.dumps(state, ensure_ascii=False), encoding="utf-8")
    if result:
        print(result)
    raise SystemExit(code)


## @brief 在隔离环境中验证热点检测、网络规则和服务启动脚本。
# @param shell 用于执行部署脚本的 shell 路径。
# @param output 隔离测试日志及截图输出目录。
def check(shell: str, output: Path):
    """在隔离环境中验证热点检测、网络规则和服务启动脚本。"""
    repository = Path(__file__).resolve().parents[2]
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    bins = output / "bin"
    bins.mkdir(exist_ok=True)
    for tool in (
        "nmcli",
        "iptables",
        "ip",
        "id",
        "flock",
        "install",
        "chmod",
        "timeout",
        "sleep",
        "systemctl",
    ):
        stub = bins / tool
        stub.write_text(
            f'#!/bin/sh\nexec "$PORTAL_TEST_PYTHON" "$PORTAL_TEST_DRIVER" --driver {tool} "$@"\n',
            encoding="utf-8",
        )
        stub.chmod(0o755)
    config = output / "portal.env"
    custom, runtime, dns, ready = [
        output / name for name in ("custom-ssid", "runtime", "dns.conf", "ready")
    ]
    config.write_text(
        "\n".join(
            [
                "AP_INTERFACE=auto",
                "AP_CONNECTION=yc-device-portal",
                "AP_ADDRESS=192.168.10.1/24",
                "AP_PASSWORD=fixture-password",
                f"PORTAL_RUNTIME='{runtime.as_posix()}'",
                f"PORTAL_CUSTOM_SSID='{custom.as_posix()}'",
                f"PORTAL_DNS_CONFIG='{dns.as_posix()}'",
                f"QT_READY_FILE='{ready.as_posix()}'",
                f"QT_SOCKET='{(output / 'missing-socket').as_posix()}'",
                "QT_WAIT_SECONDS=1",
                "QT_STABLE_SECONDS=3",
                "WEB_BINARY=/never-run-web",
                "WEB_CONFIG=/unused",
                "",
            ]
        ),
        encoding="utf-8",
    )
    ready.write_text("1\nYCEEQZ12345ABCDE\nYC_ABCDE_0000\n", encoding="utf-8")
    # Only this test's named fixture is reset; later stop/prepare checks persistence.
    custom.unlink(missing_ok=True)
    env = {
        **os.environ,
        "PORTAL_ENV": config.as_posix(),
        "PORTAL_TEST_ROOT": str(output),
        "PORTAL_TEST_PYTHON": Path(sys.executable).as_posix(),
        "PORTAL_TEST_DRIVER": Path(__file__).resolve().as_posix(),
        "PYTHONIOENCODING": "utf-8",
    }
    # MSYS shells use a POSIX PATH, while the Python harness uses Windows PATH.
    posix_bins = (
        "/" + bins.drive[0].lower() + bins.as_posix()[2:] if os.name == "nt" else bins.as_posix()
    )
    env["PORTAL_TEST_BIN"] = posix_bins
    env["PATH"] = posix_bins + ":/usr/bin:/bin:" + os.environ.get("PATH", "")
    network = repository / "web/deploy/portal-network.sh"
    env["PORTAL_TEST_SHELL"] = shell
    env["PORTAL_TEST_NETWORK"] = network.as_posix()

    ## @brief 执行被测门户脚本，并核对实际退出码和捕获输出。
    # @param expected 预期退出码，默认 0。
    # @param password_input 经标准输入提供的测试密码，None 表示不提供。
    # @param args 传给固定测试程序的独立参数。
    # @return 测试辅助程序去掉首尾空白后的标准输出文本。
    def run(*args, expected=0, password_input=None):
        """执行被测门户脚本，并核对实际退出码和捕获输出。"""
        command = 'PATH="$PORTAL_TEST_BIN:$PATH"; export PATH; exec sh "$@"'
        result = subprocess.run(
            [shell, "-c", command, "portal-test", network.as_posix(), *args],
            env=env,
            capture_output=True,
            input=password_input,
            timeout=30,
        )
        assert result.returncode == expected, (
            args,
            result.returncode,
            result.stderr.decode("utf-8", errors="replace"),
        )
        return result.stdout.decode("utf-8").strip()

    state_file = output / "state.json"

    ## @brief 从隔离目录读取当前模拟网络状态。
    # @return 当前隔离模拟工具的状态对象。
    def state():
        """从隔离目录读取当前模拟网络状态。"""
        return json.loads(state_file.read_text(encoding="utf-8"))

    ## @brief 重置模拟网卡与连接状态，准备下一项独立测试。
    # @param changes 准备下个场景时合并到模拟状态的字段。
    def reset(**changes):
        """重置模拟网卡与连接状态，准备下一项独立测试。"""
        data = {
            "wifi": True,
            "ap": True,
            "profile": False,
            "active": False,
            "iface": "wlx001122334455",
            "ssid": "",
            "password": "fixture-password",
            "calls": [],
            "rules": [["FORWARD", "-j", "ACCEPT"]],
            **changes,
        }
        state_file.write_text(json.dumps(data), encoding="utf-8")

    reset(wifi=False)
    run("detect", expected=1)
    assert not any(c[0] == "iptables" for c in state()["calls"])
    reset(ap=False)
    run("prepare", expected=10)
    reset(occupied=True)
    run("detect", expected=1)
    reset(profile=True, active=True, mode="infrastructure")
    run("detect", expected=1)
    reset(conflict=True)
    run("prepare", expected=1)
    assert not state()["profile"]
    reset(firewall_fail=True)
    run("prepare", expected=1)
    assert not state()["active"], "Hotspot activated despite failed isolation"
    run("stop")
    reset()
    assert run("detect") == "wlx001122334455"
    run("prepare")
    assert state()["active"] and state()["ssid"] == "YC_ABCDE_0000"
    assert dns.read_text(encoding="utf-8").endswith("address=/#/192.168.10.1\n")
    assert len(state()["rules"]) == 2 and state()["rules"][0][2] == "wlx001122334455"
    run("prepare")
    assert len(state()["rules"]) == 2, "Firewall rules accumulated"
    run("save", "Custom_SSID_01")
    assert custom.read_text().strip() == "Custom_SSID_01" and state()["ssid"] == "Custom_SSID_01"
    run("save", "中文热点_01", "--password-stdin", password_input=b"New-Hotspot-123\n")
    assert run("ssid") == "中文热点_01", "SSID did not use UTF-8 output"
    assert state()["password"] == "New-Hotspot-123"
    run("save", "中文热点_01", "--password-stdin", password_input=b"a" * 64 + b"\n")
    assert state()["password"] == "a" * 64, "Hexadecimal PSK not supported"
    run("save", "中文热点_01", "--password-stdin", password_input=b"New-Hotspot-123\n")
    for invalid in (b"short\n", ("中" * 8 + "\n").encode("utf-8"), b"z" * 64 + b"\n"):
        run("save", "must-not-save", "--password-stdin", password_input=invalid, expected=1)
        assert state()["ssid"] == "中文热点_01" and state()["password"] == "New-Hotspot-123"
    run("save", "Custom_SSID_01")
    assert state()["password"] == "New-Hotspot-123", "SSID-only update changed password"
    previous = state()["calls"][:]
    run("request-refresh")
    fresh = state()["calls"][len(previous) :]
    assert any(c[0] == "nmcli" and c[5:7] == ["connection", "down"] for c in fresh)
    assert state()["active"] and not (runtime / "refreshing").exists()
    run("save", "bad\ncommand", expected=1)
    run("stop")
    assert not state()["active"] and state()["rules"] == [["FORWARD", "-j", "ACCEPT"]]
    assert not dns.exists() and custom.exists(), "Custom SSID should persist across stop"
    run("prepare")
    assert state()["ssid"] == "Custom_SSID_01", "Custom SSID lost on boot"
    assert state()["password"] == "New-Hotspot-123", "Custom password lost on boot"
    # 自定义名称文件写入失败时，NM 中的中文名称和原密码同时回滚。
    run("save", "原中文热点")
    with config.open("a", encoding="utf-8") as file:
        file.write(f"PORTAL_CUSTOM_SSID='{(output / 'missing-parent' / 'custom').as_posix()}'\n")
    run("save", "must-rollback", "--password-stdin", password_input=b"Rollback-123\n", expected=1)
    assert state()["ssid"] == "原中文热点" and state()["password"] == "New-Hotspot-123"
    with config.open("a", encoding="utf-8") as file:
        file.write(f"PORTAL_CUSTOM_SSID='{custom.as_posix()}'\n")
    run("stop")
    # Qt timeout is a normal skip, not a restart loop or a call to another service.
    wrapper = repository / "web/deploy/device-web-start.sh"
    result = subprocess.run([shell, wrapper.as_posix()], env=env, capture_output=True, timeout=5)
    assert result.returncode == 0 and b"portal skipped" in result.stdout
    # Exercise actual supervisor preparation function with install failures under set -e.
    text = (repository / "scripts/start_face_media.sh").read_text(encoding="utf-8")
    function = text[
        text.index("prepare_qt_web_environment()") : text.index(
            "\nload_driver()", text.index("prepare_qt_web_environment()")
        )
    ]
    probe = (
        'set -e\nlog() { echo "$*"; }\ninstall() { return 1; }\n'
        + function
        + "\nprepare_qt_web_environment\necho QT_CONTINUES\n"
    )
    result = subprocess.run([shell, "-c", probe], capture_output=True, timeout=5)
    assert result.returncode == 0 and b"QT_CONTINUES" in result.stdout
    print(
        "Portal shell checks passed: no WiFi/AP/uplink, subnet conflict, NM persistence, firewall, DNS, refresh, cleanup, Qt isolation"
    )


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--driver":
        driver()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shell", required=True)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    check(options.shell, options.output)
