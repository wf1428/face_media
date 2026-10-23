## @file browser_check.py
# @brief 使用真实 Chromium 验证生产网页；隔离测试响应不进入交付前端。

"""
@file browser_check.py
@brief 使用真实 Chromium 验证生产网页；隔离测试响应不进入交付前端。
@author Du lin
@date 2026-10-09
Note: 最近修改：2026-10-22；修改人：Du lin；验证混合媒体队列累计选择、移除重选、逐项上传及断线后原任务取消恢复。
"""

import argparse
from urllib.parse import urlsplit
import uuid
import json
import os
import socket
import subprocess
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from playwright.sync_api import sync_playwright, expect


## @brief 实际 device-webd 提供静态文件/CSP；仅 Qt 业务使用浏览器测试桩。
# @param dist 实际 Vite 构建输出目录。
# @param executable 本机 Chromium 可执行文件路径。
# @param output 隔离测试日志及截图输出目录。
# @param server_executable 待验证的真实 device-webd 可执行文件。
def check(dist: Path, executable: str, output: Path, server_executable: Path) -> None:
    """实际 device-webd 提供静态文件/CSP；仅 Qt 业务使用浏览器测试桩。"""
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with socket.socket() as port_probe:
        port_probe.bind(("127.0.0.1", 0))
        port = port_probe.getsockname()[1]
    origin = f"http://127.0.0.1:{port}"
    config = output / "web-test.json"
    config.write_text(
        json.dumps(
            {
                "bind": "127.0.0.1",
                "port": port,
                "portal_port": 0,
                "origin": origin,
                "socket": "/no-such-device-webd-browser-test/socket",
                "www": str(dist.resolve()),
                "development_http": True,
            }
        ),
        encoding="utf-8",
    )
    stdout = (output / "server-stdout.log").open("w", encoding="utf-8")
    stderr = (output / "server-stderr.log").open("w", encoding="utf-8")
    server = subprocess.Popen(
        [str(server_executable.resolve()), str(config)],
        stdout=stdout,
        stderr=stderr,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )
    calls, errors, tasks, assets, manifest = [], [], {}, [], []
    violations, external_requests = [], []
    task_id = "11111111-1111-4111-8111-111111111111"
    batch_id = "22222222-2222-4222-8222-222222222222"
    state = {"batch": "OPEN"}
    device = {
        "deviceId": "YCEEQZ12345ABCDE",
        "deviceName": "YC_ABCDE_0000",
        "wifiName": "YC_ABCDE_0000",
    }

    people = {
        1: {
            "id": 1,
            "name": "本地测试甲",
            "account": "web_a",
            "employeeNo": "W001",
            "phone": "13812345678",
            "version": 1,
        }
    }
    credentials = {
        1: {
            "personId": 1,
            "version": 1,
            "icCards": [],
            "qrCodes": [],
            "faces": [],
            "floors": [],
            "rules": {},
        }
    }
    schedules = []
    play = {
        "contentType": "VIDEO_IMAGE",
        "smallImageMode": 0,
        "videoType": "NONE",
        "name": "",
        "videoVolume": 35,
        "videoPath": "",
        "videoUrl": "",
        "playbackState": "STOPPED",
    }
    jobs = {}
    network = {
        "iface": "eth0",
        "dhcp": True,
        "ip": "192.168.3.189",
        "netmask": "255.255.255.0",
        "gateway": "192.168.3.1",
        "dns": "1.1.1.1",
        "useConfig": True,
        "autoConnect": True,
    }
    floor_states = {n: int(n == 1) for n in [*range(-8, 0), *range(1, 121)]}
    pixel = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jh1sAAAAASUVORK5CYII="

    ## @brief 提供标准 API 的浏览器隔离响应，并记录意外业务调用。
    # @param route Playwright 路由对象，测试响应仅用于当前隔离浏览器。
    def standard(route):
        """提供标准 API 的浏览器隔离响应，并记录意外业务调用。"""
        request = route.request
        path = urlsplit(request.url).path.removeprefix("/api")
        args = request.post_data_json if request.post_data_buffer else {}
        calls.append(
            (request.method, "/std" + path, request.post_data_buffer, request.headers)
        )
        if request.method != "GET":
            assert (
                request.headers.get("x-csrf-token") == "test-csrf"
            ), "Standard API missing CSRF"
            assert request.headers.get(
                "idempotency-key"
            ), "Mutation missing idempotency key"
        meta = {}
        if path == "/device/network":
            if request.method == "GET":
                data = dict(network)
            else:
                ident = str(uuid.uuid4())
                if state.get("network_fail"):
                    jobs[ident] = {
                        "operationId": ident,
                        "state": "FAILED",
                        "message": "测试网络未连接",
                    }
                else:
                    network.update(args)
                    if args["dhcp"]:
                        network.update(
                            {k: "" for k in ("ip", "netmask", "gateway", "dns")}
                        )
                    jobs[ident] = {
                        "operationId": ident,
                        "state": "APPLIED",
                        "result": {
                            **network,
                            "saved": True,
                            "applied": not args["dhcp"],
                            "waitingForDhcp": args["dhcp"],
                        },
                    }
                data = {"operationId": ident, "state": "PENDING"}
        elif path == "/device/overview":
            data = {
                "deviceIp": "192.168.10.1",
                "personCount": len(people),
                "faceCount": 2,
                "icCount": 3,
                "qrCount": 4,
                "storage": {
                    "used": 1.234567891234,
                    "total": 31.987654321,
                    "usedPercent": 3.859999,
                },
                "storageAvailable": True,
            }
        elif path == "/device/screenshot":
            data = {"base64Image": pixel, "timestamp": "2026-10-15T12:00:00"}
        elif path == "/device/capabilities":
            data = {
                "personnel": True,
                "playback": True,
                "screenshot": True,
                "elevatorRead": True,
                "elevatorWrite": True,
                "floorScheduleWrite": True,
                "screenSleep": False,
                "boardPower": False,
                "air": False,
                "mqttRequired": False,
            }
        elif path == "/person/list":
            data = list(people.values())
            meta = {"hasMore": False, "total": len(people)}
        elif path.startswith("/person/detail/"):
            data = people[int(path.rsplit("/", 1)[1])]
        elif path.startswith("/credential/getByPerson/"):
            data = credentials[int(path.rsplit("/", 1)[1])]
        elif path == "/person/add":
            ident = max(people) + 1
            data = {**args, "id": ident, "version": 1, "account": f"WEB_{ident}"}
            people[ident] = data
            credentials[ident] = {
                "personId": ident,
                "version": 1,
                "icCards": [],
                "qrCodes": [],
                "faces": [],
                "floors": [],
                "rules": {},
            }
        elif path == "/person/update":
            people[args["id"]].update(args)
            data = people[args["id"]]
        elif path == "/person/delete":
            data = {"deleted": True}
            people.pop(args["id"])
        elif path == "/person/faceRegisterResult":
            data = {"results": []}
        elif path == "/credential/addSubItem":
            c = credentials[args["personId"]]
            if args["subType"] == "IC_CARD":
                c["icCards"].append(args["subData"])
            else:
                c["qrCodes"].append(args["subData"])
            c["version"] += 1
            data = c
        elif path == "/credential/removeSubItem":
            c = credentials[args["personId"]]
            key, field = (
                ("icCards", "cardId")
                if args["subType"] == "IC_CARD"
                else ("qrCodes", "qrUnique")
            )
            c[key] = [item for item in c[key] if item[field] != args["itemId"]]
            c["version"] += 1
            data = c
        elif path == "/credential/update":
            c = credentials[args["personId"]]
            c["rules"] = args["extra"]["rules"]
            c["version"] += 1
            data = c
        elif path == "/personFloor/save":
            c = credentials[args["personId"]]
            c["floors"] = args["floor"]
            c["version"] += 1
            data = c
        elif path == "/device/getPlayInfo":
            data = play
        elif path == "/device/videoControl":
            play.update(args)
            ident = str(uuid.uuid4())
            jobs[ident] = {
                "operationId": ident,
                "state": "APPLIED",
                "result": dict(play),
            }
            data = {"operationId": ident, "state": "PENDING"}
        elif path.startswith("/operations/"):
            data = jobs[path.rsplit("/", 1)[1]]
        elif path == "/elevator/floorLimit/get":
            data = [
                {"floor": n, "status": status} for n, status in floor_states.items()
            ]
        elif path == "/elevator/floorLimit/set":
            for floor in args["floors"]:
                floor_states[floor["floor"]] = floor["action"]
            ident = str(uuid.uuid4())
            jobs[ident] = {
                "operationId": ident,
                "state": "APPLIED",
                "result": {
                    "saved": True,
                    "transportSent": True,
                    "hardwareConfirmed": None,
                },
            }
            data = {"operationId": ident, "state": "PENDING"}
        elif path == "/elevator/floorSchedule/get":
            data = {
                "deviceSchedules": list(schedules),
                "platformSchedules": None,
                "matched": None,
                "writable": True,
            }
        elif path in ("/elevator/floorSchedule/set", "/elevator/floorSchedule/delete"):
            if path.endswith("/set"):
                schedules[:] = args["schedules"]
            else:
                schedules[:] = [item for item in schedules if item["hash"] not in args["hashList"]]
            ident = str(uuid.uuid4())
            jobs[ident] = {"operationId": ident, "state": "APPLIED", "result": {"deviceSchedules": list(schedules), "saved": True}}
            data = {"operationId": ident, "state": "PENDING"}
        elif path == "/elevator/elevatorMode/get":
            data = {
                k: False
                for k in (
                    "masterSwitch",
                    "delay",
                    "parking",
                    "driver",
                    "independent",
                    "ext1",
                    "ext2",
                    "ext3",
                    "ext4",
                )
            }
        elif path in ("/elevator/floorLimit/set", "/elevator/elevatorMode/set"):
            data = {"saved": True, "transportSent": True, "hardwareConfirmed": None}
        elif path == "/device/change-password":
            data = {"reloginRequired": True}
        else:
            errors.append(f"Unexpected standard API: {request.method} {path}")
            route.fulfill(status=404, json={"code": 404, "message": "unexpected"})
            return
        route.fulfill(json={"code": 200, "message": "ok", "data": data, "meta": meta})

    ## @brief 白名单测试桩：记录每个请求，意外业务立即使测试失败。
    # @param route Playwright 路由对象，测试响应仅用于当前隔离浏览器。
    def respond(route):
        """白名单测试桩：记录每个请求，意外业务立即使测试失败。"""
        nonlocal task_id
        request = route.request
        path = request.url.split("/api/v1", 1)[1]
        calls.append((request.method, path, request.post_data_buffer, request.headers))

        # 只模拟本次交付的接口；其他路由出现即作为测试失败记录。
        if path == "/device/info":
            data = device
        elif path == "/device/wifi":
            assert request.method == "POST"
            assert request.headers.get("x-csrf-token") == "test-csrf"
            ssid = request.post_data_json["ssid"]
            device.update(deviceName=ssid, wifiName=ssid)
            data = {**device, "restartPending": True}
        elif path in ("/auth/login", "/auth/session"):
            data = {"username": "tester", "csrf": "test-csrf"}
        elif path == "/files":
            data = {"files": []}
        elif path == "/persons":
            data = {
                "persons": [
                    {
                        "id": 1,
                        "name": "本地测试甲",
                        "account": "web_a",
                        "employeeNo": "W001",
                        "phone": "13812345678",
                    }
                ]
            }
        elif path == "/persons/1":
            data = {
                "person": {
                    "id": 1,
                    "name": "本地测试甲",
                    "account": "web_a",
                    "employeeNo": "W001",
                    "phone": "13812345678",
                }
            }
        elif path == "/uploads":
            item = request.post_data_json
            if item["kind"] in ("video", "ad_image"):
                assert not any(task["kind"] in ("video", "ad_image")
                               and task["state"] not in ("READY", "FAILED", "CANCELLED")
                               for task in tasks.values()), "Media queue started concurrent receiving tasks"
            task_id = str(uuid.uuid4())
            tasks[task_id] = {**item, "id": task_id, "state": "CREATED"}
            data = tasks[task_id]
        elif path == f"/uploads/{task_id}/content":
            assert request.post_data_buffer, "File body missing"
            assert request.headers.get("content-type") == "application/octet-stream"
            assert request.headers.get("x-csrf-token") == "test-csrf"
            if state.get("fail_media_name") == tasks[task_id]["name"]:
                state.pop("fail_media_name")
                tasks[task_id]["state"] = "RECEIVING"
                route.abort()
                return
            tasks[task_id]["state"] = "RECEIVED"
            data = tasks[task_id]
        elif path == f"/uploads/{task_id}/cancel":
            tasks[task_id]["state"] = "CANCELLED"
            data = {"cancelled": True}
        elif path == f"/uploads/{task_id}/commit":
            tasks[task_id]["state"] = (
                "COMMITTING"
                if tasks[task_id]["kind"] == "resource_package"
                else "READY"
            )
            data = tasks[task_id]
            if data["kind"] not in ("video", "ad_image", "resource_package"):
                assets.append({"name": data["name"], "kind": data["kind"]})
        elif path == f"/uploads/{task_id}":
            if tasks[task_id]["kind"] == "resource_package":
                tasks[task_id].update(
                    state="READY",
                    updatedDirectories=["logo"],
                    restartScheduled=not state.get("resource_restart_fail", False),
                    restartError="后端诊断不应替代页面固定文案",
                )
            data = tasks[task_id]
        elif path == "/imports":
            manifest[:] = request.post_data_json["manifest"]
            data = {
                "id": batch_id,
                "state": "OPEN",
                "result": {"manifest": manifest},
                "assets": assets,
            }
        elif path == f"/imports/{batch_id}":
            data = {
                "id": batch_id,
                "state": state["batch"],
                "result": {"manifest": manifest},
                "assets": assets,
            }
        elif path == f"/imports/{batch_id}/start":
            state["batch"] = "DONE"
            data = {
                "id": batch_id,
                "state": "DONE",
                "result": {"inserted": 1, "savedFaces": 1, "failed": 0},
            }
        else:
            errors.append(f"Unexpected business request: {request.method} {path}")
            route.fulfill(status=404, json={"code": 404, "message": "not supported"})
            return
        route.fulfill(json={"code": 200, "data": data})

    try:
        for _ in range(100):
            if server.poll() is not None:
                raise AssertionError(
                    "device-webd exited before startup; inspect console captures"
                )
            try:
                urllib.request.urlopen(origin, timeout=1).close()
                break
            except (OSError, urllib.error.URLError):
                time.sleep(0.1)
        else:
            raise AssertionError("device-webd did not become ready")

        ## @brief 向实际测试服务器发起请求，统一返回成功或错误状态码。
        # @param path 待生成的夹具路径或相对于隔离服务的请求路径。
        # @param kwargs 传给 urllib 请求的额外参数。
        # @return HTTP 响应状态码，错误响应也返回其实际状态。
        def fetch(path, **kwargs):
            """向实际测试服务器发起请求，统一返回成功或错误状态码。"""
            try:
                return urllib.request.urlopen(origin + path, timeout=5, **kwargs).status
            except urllib.error.HTTPError as error:
                return error.code

        ## @brief 取得指定 HTTP 请求的实际状态码，不把错误响应当成连接失败。
        # @param request 已构建的 HTTP 请求对象。
        # @return HTTP 响应状态码，错误响应也返回其实际状态。
        def fetch_status(request):
            """取得指定 HTTP 请求的实际状态码，不把错误响应当成连接失败。"""
            try:
                return urllib.request.urlopen(request, timeout=5).status
            except urllib.error.HTTPError as error:
                return error.code

        # Exercise real error responses/logging before API interception: no Qt,
        # forbidden Origin, missing file, and parallel requests without query leaks.
        assert fetch("/api/v1/health") == 503
        assert fetch("/api/device/info") == 503
        assert fetch("/api/person/list") == 401
        assert fetch("/api/device/network") == 401
        for path, payload, expected in [
            ("/api/device/verify-password", {}, 400),
            (
                "/api/device/verify-password",
                {"username": "tester", "password": "DO_NOT_LOG_BODY"},
                503,
            ),
            ("/api/device/videoControl", {"videoType": "NONE"}, 401),
            ("/api/device/network", {"dhcp": True}, 401),
        ]:
            req = urllib.request.Request(
                origin + path,
                data=json.dumps(payload).encode(),
                headers={"Origin": origin, "Content-Type": "application/json"},
            )
            assert fetch_status(req) == expected
        denied = urllib.request.Request(
            origin + "/api/v1/auth/login",
            data=b'{"password":"DO_NOT_LOG_BODY"}',
            headers={
                "Origin": "http://invalid.test",
                "Content-Type": "application/json",
                "Cookie": "DO_NOT_LOG_COOKIE",
                "X-CSRF-Token": "DO_NOT_LOG_TOKEN",
            },
        )
        try:
            urllib.request.urlopen(denied, timeout=5)
            raise AssertionError("Invalid Origin accepted")
        except urllib.error.HTTPError as error:
            assert error.code == 403
        valid_login = urllib.request.Request(
            origin + "/api/v1/auth/login",
            data=b'{"username":"tester","password":"DO_NOT_LOG_BODY"}',
            headers={
                "Origin": origin,
                "Content-Type": "application/json",
                "Cookie": "DO_NOT_LOG_COOKIE",
                "X-CSRF-Token": "DO_NOT_LOG_TOKEN",
            },
        )
        try:
            urllib.request.urlopen(valid_login, timeout=5)
            raise AssertionError("Login succeeded without Qt")
        except urllib.error.HTTPError as error:
            assert error.code == 503
        assert fetch("/not-found?password=DO_NOT_LOG_QUERY") == 404
        with ThreadPoolExecutor(max_workers=6) as pool:
            assert all(status == 200 for status in pool.map(fetch, ["/"] * 12))
        live_stdout = (output / "server-stdout.log").read_text(encoding="utf-8")
        live_stderr = (output / "server-stderr.log").read_text(encoding="utf-8")
        assert (
            "HTTP listening:" in live_stdout
            and "Web directory:" in live_stdout
            and "Qt socket:" in live_stdout
        )
        assert "HTTP client=127.0.0.1 GET / status=200 elapsed=" in live_stdout
        assert "Qt RPC operation=health" in live_stderr and "status=503" in live_stderr
        assert "Qt RPC operation=auth.login" in live_stderr
        if os.name != "nt":
            assert "stage=connect status=503 errno=2" in live_stderr
        assert (
            "[WARN]" in live_stderr
            and "status=403" in live_stderr
            and "status=404" in live_stderr
        )
        assert (
            "DO_NOT_LOG_" not in live_stdout + live_stderr
        ), "Sensitive HTTP data leaked to console"
        # Reserve a non-reusable port. Windows SO_REUSEADDR permits two httplib
        # servers on one port, so another ordinary instance is not a valid fixture.
        with socket.socket() as occupied:
            if os.name == "nt":
                occupied.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            occupied.bind(("127.0.0.1", 0))
            occupied.listen(1)
            blocked_port = occupied.getsockname()[1]
            blocked_config = output / "web-port-busy.json"
            blocked = json.loads(config.read_text(encoding="utf-8"))
            blocked.update(port=blocked_port, origin=f"http://127.0.0.1:{blocked_port}")
            blocked_config.write_text(json.dumps(blocked), encoding="utf-8")
            duplicate = subprocess.run(
                [str(server_executable.resolve()), str(blocked_config)],
                capture_output=True,
                timeout=10,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
            )
            assert (
                duplicate.returncode != 0
                and b"Main listener bind failed:" in duplicate.stderr
            )

        with sync_playwright() as runtime:
            browser = runtime.chromium.launch(executable_path=executable, headless=True)
            page = browser.new_page(
                viewport={"width": 390, "height": 844}, locale="zh-CN"
            )
            page.on("pageerror", lambda error: errors.append(str(error)))
            page.add_init_script(
                """window.testCspViolations=[];
                document.addEventListener('securitypolicyviolation',event=>{
                  window.testCspViolations.push({directive:event.effectiveDirective, blockedURI:event.blockedURI});
                });"""
            )
            # 只替换浏览器密码提供者；验证保存/回填调用，不冒充手机已允许保存。
            page.add_init_script(
                """window.testPasswordStores=[];
                window.PasswordCredential=class {
                  constructor(data){Object.assign(this,data);this.type='password'}
                };
                Object.defineProperty(navigator,'credentials',{configurable:true,value:{
                  get:async()=>({type:'password',id:'tester',password:'test-secret'}),
                  store:async c=>{window.testPasswordStores.push({id:c.id,password:c.password})}
                }});"""
            )

            ## @brief 只允许测试设备同源请求，记录并拒绝外部资源加载。
            # @param route Playwright 路由对象，测试响应仅用于当前隔离浏览器。
            def allow_local_only(route):
                """只允许测试设备同源请求，记录并拒绝外部资源加载。"""
                if route.request.url.startswith(origin + "/"):
                    route.continue_()
                else:
                    external_requests.append(route.request.url)
                    route.abort()

            page.route("**/*", allow_local_only)
            page.route("**/api/**", standard)
            page.route("**/api/v1/**", respond)
            # 实际服务器提供门户资源和固定地址；不再通过 302 把窗口送到登录页。
            guide = page.goto(origin + "/portal?url=https://untrusted.invalid")
            assert guide.status == 200 and "location" not in guide.headers
            page.locator("#open-browser").wait_for(state="visible")
            assert page.locator("#server-address").input_value() == origin + "/#/login"
            assert (
                page.locator("#open-browser").get_attribute("href")
                == origin + "/#/login"
            )
            assert page.locator('input[type="password"]').count() == 0
            page.screenshot(path=str(output / "portal-guide.png"), full_page=True)
            android = browser.new_page(
                user_agent="Mozilla/5.0 (Linux; Android 14; Xiaomi 14)"
            )
            android.goto(origin + "/portal")
            android.locator("#open-browser").wait_for(state="visible")
            intent = android.locator("#open-browser").get_attribute("href")
            assert intent.startswith(f"intent://127.0.0.1:{port}/#Intent;scheme=http;")
            assert (
                "android.intent.action.VIEW" in intent
                and "S.browser_fallback_url=" in intent
            )
            assert (
                android.url == origin + "/portal"
            ), "Portal launched app without a user click"
            android.close()
            response = page.goto(origin + "/#/login")
            assert "font-src 'self' data:" in response.headers.get(
                "content-security-policy", ""
            )
            page.get_by_text("YC_ABCDE_0000", exact=True).first.wait_for()
            assert page.locator("input").nth(0).input_value() == device["deviceId"]
            assert page.locator("input").nth(1).input_value() == device["wifiName"]
            assert page.get_by_text("由浏览器保存并填充", exact=False).count() == 0
            output.mkdir(parents=True, exist_ok=True)
            page.screenshot(path=str(output / "login-device.png"), full_page=True)
            page.get_by_role("checkbox", name="记住密码", exact=True).click()
            page.locator('input[name="username"]').fill("tester")
            page.locator('input[name="password"]').fill("test-secret")
            page.get_by_role("button", name="显示密码", exact=True).click()
            assert (
                page.locator('input[name="password"]').get_attribute("type") == "text"
            )
            page.get_by_role("button", name="隐藏密码", exact=True).click()
            assert (
                page.locator('input[name="password"]').get_attribute("type")
                == "password"
            )
            page.locator("button.submit").click()
            page.wait_for_url("**/#/home/preview")
            assert page.evaluate("window.testPasswordStores") == [
                {"id": "tester", "password": "test-secret"}
            ]
            assert "test-secret" not in page.evaluate("JSON.stringify(localStorage)")
            page.goto(origin + "/#/login")
            page.wait_for_function(
                "() => document.querySelector('input[name=password]').value === 'test-secret'"
            )
            assert page.locator('input[name="username"]').input_value() == "tester"
            assert (
                page.get_by_role("checkbox", name="记住密码", exact=True).get_attribute(
                    "aria-checked"
                )
                == "true"
            )
            page.goto(origin + "/#/home/preview")
            page.locator(".van-tabbar").wait_for()
            assert (
                page.locator(".van-tabbar-item").count() == 6
            ), "Device navigation missing"
            page.evaluate("document.fonts.ready")
            assert page.evaluate(
                "document.fonts.check('16px vant-icon')"
            ), "Icon font blocked or unavailable"
            assert page.evaluate(
                "[...document.fonts].some(f=>f.family.includes('vant-icon') && f.status==='loaded')"
            )
            assert page.locator(
                ".lang-switch svg.language-icon"
            ).is_visible(), "Local language icon missing"
            glyphs = page.locator(".van-tabbar .van-icon").evaluate_all(
                "nodes=>nodes.map(n=>getComputedStyle(n,'::before').content)"
            )
            assert len(glyphs) == 6 and all(
                g not in ("none", "normal", '""') for g in glyphs
            )
            page.screenshot(path=str(output / "preview-icons.png"), full_page=True)
            violations.extend(page.evaluate("window.testCspViolations"))
            page.locator(".van-tabbar").get_by_text("设置", exact=True).click()
            page.wait_for_url("**/#/home/device")
            assert (
                page.get_by_text("仅替换 Ycest 下提供的目录", exact=False).count() == 0
            )
            # 真正点击文件控件并上传原始压缩包，成功必须等待目录更新结果。
            with page.expect_file_chooser() as chooser:
                page.get_by_text("选择配置压缩包", exact=True).click()
            chooser.value.set_files(
                {
                    "name": "Ycest.rar",
                    "mimeType": "application/vnd.rar",
                    "buffer": b"resource-browser-fixture",
                }
            )
            page.get_by_role("button", name="上传到设备", exact=True).click()
            page.get_by_text("点击确认后，设备即将更新", exact=True).wait_for()
            page.get_by_role("button", name="确认", exact=True).click()
            page.get_by_role("status").filter(has_text="更新完成！").wait_for()
            assert page.get_by_text("设备即将重启，请稍候", exact=False).count() > 0
            assert tasks[task_id]["kind"] == "resource_package"
            assert (
                page.evaluate("sessionStorage.getItem('ycest-resource-task')") is None
            )
            page.locator(".van-toast").wait_for(state="hidden")
            page.wait_for_timeout(350)
            page.screenshot(path=str(output / "resource-package.png"), full_page=True)
            state["resource_restart_fail"] = True
            with page.expect_file_chooser() as chooser:
                page.get_by_text("选择配置压缩包", exact=True).click()
            chooser.value.set_files(
                {
                    "name": "Ycest.zip",
                    "mimeType": "application/zip",
                    "buffer": b"resource-restart-failure-fixture",
                }
            )
            page.get_by_role("button", name="上传到设备", exact=True).click()
            page.get_by_role("button", name="确认", exact=True).click()
            page.get_by_role("status").filter(
                has_text="资源已更新，但设备未刷新，请检查设备服务。"
            ).wait_for()
            assert (
                page.get_by_text("后端诊断不应替代页面固定文案", exact=False).count()
                == 0
            )
            state["resource_restart_fail"] = False
            page.get_by_text("IP设置", exact=True).click()
            page.wait_for_url("**/#/home/network")
            ip_field = page.locator('input[name="network-ip"]')
            page.wait_for_function(
                "() => document.querySelector('input[name=network-ip]')?.value === '192.168.3.189'"
            )
            assert ip_field.is_disabled()
            page.get_by_role("radio", name="静态 IP", exact=True).click()
            ip_field.fill("192.168.3.55")
            page.locator('input[name="network-netmask"]').fill("255.0.255.0")
            before_posts = sum(
                m == "POST" and p == "/std/device/network" for m, p, _, _ in calls
            )
            page.get_by_role("button", name="应用 IP 设置", exact=True).click()
            page.get_by_text("请检查 IPv4", exact=False).wait_for()
            assert before_posts == sum(
                m == "POST" and p == "/std/device/network" for m, p, _, _ in calls
            )
            page.locator('input[name="network-netmask"]').fill("255.255.255.0")
            page.locator('input[name="network-gateway"]').fill("192.168.3.1")
            page.locator('input[name="network-dns"]').fill("1.1.1.1 8.8.8.8")
            page.get_by_role("button", name="应用 IP 设置", exact=True).click()
            page.locator(".status").filter(has_text="网络设置已应用并保存").wait_for()
            assert network["ip"] == "192.168.3.55" and not network["dhcp"]
            page.reload()
            page.wait_for_function(
                "() => document.querySelector('input[name=network-ip]')?.value === '192.168.3.55'"
            )
            page.wait_for_function(
                "() => document.querySelector('.footer button')?.disabled === false"
            )
            page.wait_for_timeout(350)
            page.screenshot(path=str(output / "network-static.png"), full_page=True)
            network["ip"] = "192.168.3.66"
            page.get_by_role("button", name="刷新", exact=True).click()
            page.wait_for_function(
                "() => document.querySelector('input[name=network-ip]')?.value === '192.168.3.66'"
            )
            page.get_by_role("radio", name="动态 DHCP", exact=True).click()
            page.get_by_role("button", name="应用 IP 设置", exact=True).click()
            page.locator(".status").filter(has_text="正在等待分配地址").wait_for()
            assert network["dhcp"] and network["ip"] == "" and ip_field.is_disabled()
            page.locator(".van-toast").wait_for(state="hidden")
            page.screenshot(path=str(output / "network-dhcp.png"), full_page=True)
            state["network_fail"] = True
            page.get_by_role("button", name="应用 IP 设置", exact=True).click()
            page.locator(".status").filter(has_text="测试网络未连接").wait_for()
            assert network["ip"] == "", "Failed network task overwrote saved settings"
            state["network_fail"] = False
            page.locator(".van-nav-bar__left").click()
            page.wait_for_url("**/#/home/device")
            assert page.locator(".van-icon-description").evaluate(
                "n=>getComputedStyle(n,'::before').content"
            ) not in ("none", "normal", '""')
            ssid = page.get_by_placeholder("请输入热点名称")
            ssid.fill("手机连接新热点")
            hotspot_password = page.locator('input[name="hotspot-new-password"]')
            hotspot_password.fill("short")
            page.get_by_role("button", name="保存设置", exact=True).click()
            page.get_by_text("热点密码需为", exact=False).first.wait_for()
            assert not any(path == "/device/wifi" for _, path, _, _ in calls)
            hotspot_password.fill("New-Hotspot-123")
            page.get_by_role("button", name="保存设置", exact=True).click()
            page.get_by_role("button", name="取消", exact=True).click()
            assert not any(
                path == "/device/wifi" for _, path, _, _ in calls
            ), "Cancelled SSID was saved"
            ssid.fill("中" * 11)
            page.get_by_role("button", name="保存设置", exact=True).click()
            page.get_by_text("SSID 需为 1–32 字节", exact=False).first.wait_for()
            assert not any(
                path == "/device/wifi" for _, path, _, _ in calls
            ), "Overlong UTF-8 SSID submitted"
            ssid.fill("手机连接新热点")
            page.get_by_role("button", name="保存设置", exact=True).click()
            page.get_by_role("button", name="确认", exact=True).click()
            page.locator(".restart-notice").wait_for()
            assert page.get_by_role("button", name="保存设置", exact=True).is_disabled()
            assert "手机会断开连接" in page.locator(".restart-notice").inner_text()
            assert "3 秒" in page.locator(".restart-notice").inner_text()
            wifi_requests = [
                body for _, path, body, _ in calls if path == "/device/wifi"
            ]
            assert json.loads(wifi_requests[-1])["password"] == "New-Hotspot-123"
            assert hotspot_password.input_value() == ""
            page.locator(".van-overlay").wait_for(state="hidden")
            page.locator(".van-toast").wait_for(state="hidden")
            page.screenshot(path=str(output / "device-ssid.png"), full_page=True)
            page.reload()
            ssid.wait_for()
            page.wait_for_function(
                "() => !document.querySelector('input[placeholder=\"请输入热点名称\"]').disabled"
            )
            assert ssid.input_value() == "手机连接新热点", "Saved SSID not restored"
            assert page.locator("input").nth(1).input_value() == "手机连接新热点"
            # 名称不变时也能单独修改密码，不能被“名称未变化”短路。
            hotspot_password.fill("Password-Only-456")
            page.get_by_role("button", name="保存设置", exact=True).click()
            page.get_by_role("button", name="确认", exact=True).click()
            page.locator(".restart-notice").wait_for()
            wifi_requests = [
                body for _, path, body, _ in calls if path == "/device/wifi"
            ]
            assert json.loads(wifi_requests[-1]) == {
                "ssid": "手机连接新热点",
                "password": "Password-Only-456",
            }

            # Local data pages use standard APIs; missing capabilities remain disabled.
            page.goto(origin + "/#/home/preview")
            page.locator(".screen-card img").wait_for()
            assert page.locator(".ov-num").all_inner_texts() == ["1", "2", "3", "4"]
            assert page.get_by_text("多媒体", exact=True).is_visible()
            assert page.locator(".header .title").count() == 0
            assert page.locator(".header .van-icon-setting-o").count() == 0
            assert page.locator(".chip.active").inner_text() == "5s"
            assert page.locator(".ov-storage-text").inner_text() == "1.23 / 31.99 GiB"
            page.locator(".van-toast").wait_for(state="hidden")
            page.screenshot(path=str(output / "preview-local.png"), full_page=True)
            page.goto(origin + "/#/home/air")
            page.get_by_text("本版本未接入", exact=False).first.wait_for()
            assert page.locator("[inert]").count() > 0
            page.goto(origin + "/#/home/elevator")
            page.get_by_role("button", name="提交楼层设置", exact=True).wait_for()
            assert page.get_by_role(
                "button", name="提交楼层设置", exact=True
            ).is_enabled()
            assert page.get_by_role(
                "button", name="保存并下发", exact=True
            ).is_enabled()
            assert page.get_by_role(
                "button", name="提交定时开关机", exact=True
            ).is_disabled()
            floor_choices = page.locator(".floor-grid [role=checkbox]")
            assert (
                floor_choices.count() == 128
            ), "Floor permissions changed during layout update"
            boxes = floor_choices.evaluate_all(
                "nodes=>nodes.slice(0,4).map(n=>n.getBoundingClientRect().top)"
            )
            assert len(set(boxes)) == 1, "Floor grid did not fit four choices on phone"
            page.set_viewport_size({"width": 320, "height": 844})
            assert page.locator(".floor-grid").evaluate(
                "n=>n.scrollWidth<=n.clientWidth"
            ), "Floor grid overflowed a narrow phone"
            page.set_viewport_size({"width": 390, "height": 844})
            page.get_by_role("checkbox", name="B1", exact=True).click()
            page.get_by_role("button", name="提交楼层设置", exact=True).click()
            expect(page.get_by_role("button", name="提交楼层设置", exact=True)).to_be_enabled()
            assert (
                floor_states[-1] == 1
            ), "Circle selection did not submit open permission"
            page.locator(".van-toast").wait_for(state="hidden")
            page.screenshot(path=str(output / "elevator-local.png"), full_page=True)
            page.get_by_text("添加定时规则", exact=True).click()
            popup = page.locator(".schedule-pop")
            popup.wait_for(state="visible", timeout=5000)
            page.screenshot(path=str(output / "schedule-dialog.png"), full_page=True)
            time_field = popup.locator(".van-field").filter(has=page.get_by_text("时间", exact=True))
            expect(time_field.locator("input")).to_have_attribute("readonly", "")
            time_field.click()
            time_popup = page.locator(".time-pop")
            time_popup.wait_for(state="visible")
            columns = time_popup.locator(".van-picker-column")
            assert columns.count() == 2
            expect(columns.nth(0).locator(".van-picker-column__item--selected")).to_have_text("08")
            expect(columns.nth(1).locator(".van-picker-column__item--selected")).to_have_text("00")
            # 实际触摸滑动两列，确认选择写回并保存；关闭遮罩不提交草稿。
            touch = page.context.new_cdp_session(page)
            touch.send("Emulation.setTouchEmulationEnabled", {"enabled": True})

            ## @brief 通过实际触摸手势滚动时间列并等待控件稳定。
            # @param column 时间选择器的小时或分钟滚动列。
            # @param rows 模拟滑动跨越的条目数。
            def slide_time_column(column, rows):
                box = column.bounding_box()
                item_height = column.locator("li").first.bounding_box()["height"]
                x, y = box["x"] + box["width"] / 2, box["y"] + box["height"] / 2
                touch.send("Input.dispatchTouchEvent", {"type": "touchStart", "touchPoints": [{"x": x, "y": y}]})
                touch.send("Input.dispatchTouchEvent", {"type": "touchMove", "touchPoints": [{"x": x, "y": y - rows * item_height}]})
                page.wait_for_timeout(350)  # 松手前停稳，避免惯性改变预期时刻。
                touch.send("Input.dispatchTouchEvent", {"type": "touchEnd", "touchPoints": []})

            page.wait_for_timeout(350)  # 底部弹窗滑入后再开始手势。
            slide_time_column(columns.nth(0), 6)
            slide_time_column(columns.nth(1), 1)
            expect(columns.nth(0).locator(".van-picker-column__item--selected")).to_have_text("14")
            expect(columns.nth(1).locator(".van-picker-column__item--selected")).to_have_text("01")
            page.screenshot(path=str(output / "schedule-time-picker.png"))
            time_popup.get_by_role("button", name="确认", exact=True).click()
            time_popup.wait_for(state="hidden")
            expect(time_field.locator("input")).to_have_value("14:01")
            time_field.click()
            time_popup.wait_for(state="visible")
            page.wait_for_timeout(350)
            slide_time_column(columns.nth(0), -1)
            expect(columns.nth(0).locator(".van-picker-column__item--selected")).to_have_text("13")
            page.mouse.click(10, 10)  # 点击当前最上层遮罩，不依赖弹窗的 DOM 顺序。
            time_popup.wait_for(state="hidden")
            expect(popup).to_be_visible()
            expect(time_field.locator("input")).to_have_value("14:01")
            touch.send("Emulation.setTouchEmulationEnabled", {"enabled": False})
            touch.detach()
            popup.get_by_role("button", name="确认", exact=True).click()
            page.get_by_role("button", name="保存并下发", exact=True).click()
            expect(page.get_by_role("button", name="保存并下发", exact=True)).to_be_enabled()
            assert len(schedules) == 1 and schedules[0]["hash"].startswith("web-")
            assert schedules[0]["time"] == "14:01", "Wheel time was not persisted"
            page.locator(".van-toast").wait_for(state="hidden")
            page.reload()
            page.locator(".van-icon-cross").wait_for()
            page.locator(".van-icon-cross").click()
            page.locator(".van-dialog:visible").get_by_role("button", name="确认", exact=True).click()
            page.wait_for_function("() => document.querySelectorAll('.van-icon-cross').length === 0")
            assert schedules == []
            page.goto(origin + "/#/home/person")
            # X 可在未创建时退出，不发送创建请求；再次打开时草稿清空。
            page.get_by_role("button", name="新建账户", exact=True).click()
            dialog = page.locator(".van-dialog:visible")
            dialog.locator("input").nth(0).fill("不应创建")
            page.wait_for_timeout(350)  # 等待弹窗过渡完成后截图。
            page.screenshot(path=str(output / "person-create.png"), full_page=True)
            page.get_by_role("button", name="关闭新建账户", exact=True).click()
            dialog.wait_for(state="hidden")
            assert not any(path == "/std/person/add" for _, path, _, _ in calls)
            page.screenshot(path=str(output / "person-list-nfc.png"), full_page=True)
            page.get_by_role("button", name="查看 NFC 读卡二维码", exact=True).click()
            page.locator(".nfc-popup").wait_for(state="visible")
            page.wait_for_timeout(350)
            page.screenshot(path=str(output / "person-nfc-preview.png"), full_page=True)
            with page.expect_download() as saved_image:
                page.get_by_role("link", name="保存到本地", exact=True).click()
            assert saved_image.value.suggested_filename == "NFC-readCard.jpg"
            assert (
                Path(saved_image.value.path()).read_bytes()
                == (dist / "NFC-readCard.jpg").read_bytes()
            )
            page.locator(".nfc-popup .van-popup__close-icon").click()
            page.get_by_role("button", name="新建账户", exact=True).click()
            dialog = page.locator(".van-dialog:visible")
            dialog.locator("input").nth(0).fill("浏览器新增人员")
            dialog.get_by_role("button", name="确认", exact=True).click()
            page.get_by_text("浏览器新增人员", exact=True).click()
            page.wait_for_url("**/#/person/2")
            page.get_by_text("录 IC 卡", exact=True).click()
            dialog = page.locator(".van-dialog:visible")
            dialog.locator("input").nth(0).fill("测试门禁卡")
            dialog.locator("input").nth(1).fill("01020304")
            dialog.get_by_role("button", name="确认", exact=True).click()
            page.get_by_text("01020304", exact=True).wait_for()
            assert credentials[2]["icCards"][0]["cardId"] == "01020304"
            page.screenshot(path=str(output / "person-local.png"), full_page=True)
            page.goto(origin + "/#/home/video")
            sources = page.locator(".source-row").first.locator(".van-radio")
            expect(sources.nth(2)).to_have_attribute("aria-checked", "true")
            expect(page.locator(".content-type-row")).to_have_count(0)
            sources.nth(0).click()
            expect(page.locator(".content-type-row")).to_be_visible()
            assert (
                page.locator(".source-options").first.evaluate(
                    "n=>getComputedStyle(n).flexWrap"
                )
                == "wrap"
            )
            positions = page.locator(".source-options").first.locator(".van-radio").evaluate_all(
                "nodes=>nodes.map(n=>n.getBoundingClientRect().top)"
            )
            assert len(set(positions)) == 1, "Video sources did not fit a phone row"
            slider = page.locator(".volume-row .van-slider").bounding_box()
            settings_card = page.locator(".play-settings").bounding_box()
            assert (
                slider["y"] + slider["height"] + 12
                < settings_card["y"] + settings_card["height"]
            )
            assert page.locator(".footer").evaluate(
                "n=>n.nextElementSibling.classList.contains('van-cell-group__title')"
                " && n.nextElementSibling.textContent.includes('广告列表')"
            )
            page.screenshot(path=str(output / "video-controls.png"), full_page=True)
            assert page.locator(".content-type-row .van-radio").count() == 3
            content_positions = page.locator(".content-type-row .van-radio").evaluate_all("nodes=>nodes.map(n=>n.getBoundingClientRect().top)")
            assert len(set(content_positions)) == 1, "Content choices did not fit a phone row"
            page.locator(".content-type-row .van-radio").nth(1).click()
            sources.nth(1).click()
            expect(page.locator(".content-type-row")).to_have_count(0)
            page.screenshot(path=str(output / "video-network-controls.png"), full_page=True)
            sources.nth(2).click()
            expect(page.locator(".content-type-row")).to_have_count(0)
            sources.nth(0).click()
            expect(page.locator(".content-type-row .van-radio").nth(1)).to_have_attribute("aria-checked", "true")
            page.get_by_role("button", name="保存播放设置", exact=True).click()
            page.wait_for_function(
                "() => !document.querySelector('button.van-button--loading')"
            )
            assert any(path == "/std/device/videoControl" for _, path, _, _ in calls)
            assert play["contentType"] == "IMAGE" and play["smallImageMode"] == 0
            assert any(
                path.startswith("/std/operations/") for _, path, _, _ in calls
            ), "Operation result not polled"
            page.locator(".van-toast").wait_for(state="hidden")
            page.screenshot(path=str(output / "video-page.png"), full_page=True)

            # 视频和人脸图片都应作为原始字节发送，清单名称保持不变。
            with page.expect_file_chooser() as chooser:
                page.get_by_text("选择视频或图片", exact=True).click()
            chooser.value.set_files(
                {
                    "name": "browser-video.mp4",
                    "mimeType": "video/mp4",
                    "buffer": b"raw-test-video-bytes",
                }
            )
            page.get_by_role("button", name="上传到设备").click()
            page.wait_for_function(
                "() => !document.querySelector('button.van-button--loading')"
            )
            page.wait_for_timeout(150)
            assert any(
                body == b"raw-test-video-bytes"
                for _, path, body, _ in calls
                if path.endswith("/content")
            ), "Video bytes altered or omitted"
            with page.expect_file_chooser() as chooser:
                page.get_by_text("选择视频或图片", exact=True).click()
            chooser.value.set_files({"name": "任意广告.PNG", "mimeType": "image/png", "buffer": b"raw-image-bytes"})
            page.locator(".play-settings [role=switch]").click()
            page.get_by_role("button", name="上传到设备").click()
            page.wait_for_function("() => !document.querySelector('button.van-button--loading')")
            assert any(path == "/uploads" and json.loads(body).get("kind") == "ad_image" and json.loads(body).get("smallImageMode") == 1
                       for _, path, body, _ in calls if path == "/uploads" and body)
            assert any(body == b"raw-image-bytes" for _, path, body, _ in calls if path.endswith("/content"))
            expect(page.get_by_role('button', name='保存播放设置', exact=True)).to_be_enabled()

            # 五项后再次选择十项必须整次拒绝；通过 x 移除后可继续补选。
            media_input = page.locator('input[type="file"]')
            queue_rows = page.locator('.upload-queue-row')

            def media_fixture(name):
                """小字节夹具仍经过真实文件选择和上传请求，不分配大文件。"""
                return {"name": name, "mimeType": "image/png" if name.endswith('.png') else "video/mp4",
                        "buffer": ("queue-bytes:" + name).encode()}

            first_five = [media_fixture(f"queue-first-{n}.{'png' if n % 2 else 'mp4'}") for n in range(5)]
            media_input.set_input_files(first_five)
            expect(queue_rows).to_have_count(5)
            media_input.set_input_files([media_fixture(f"queue-over-{n}.mp4") for n in range(10)])
            expect(page.locator('.van-toast').filter(has_text='最多选择 10 个文件')).to_be_visible()
            expect(queue_rows).to_have_count(5)
            page.locator('.van-toast').wait_for(state='hidden')
            # 同名再次选择不会重复加入。
            media_input.set_input_files(first_five)
            expect(queue_rows).to_have_count(5)
            page.locator('.van-toast').wait_for(state='hidden')
            queue_rows.nth(0).locator('.queue-remove').click()
            expect(queue_rows).to_have_count(4)
            media_input.set_input_files([media_fixture(f"queue-add-{n}.{'png' if n % 2 else 'mp4'}") for n in range(6)])
            expect(queue_rows).to_have_count(10)
            page.screenshot(path=str(output / 'media-upload-queue.png'), full_page=True)
            assert page.locator('.upload-queue-row').evaluate_all(
                'nodes=>nodes.every(n=>n.scrollWidth<=n.clientWidth+1)'
            ), 'Queue overflows phone width'
            queue_call_start = len(calls)
            page.get_by_role('button', name='上传到设备', exact=True).click()
            expect(queue_rows).to_have_count(0)
            expect(page.get_by_role('button', name='保存播放设置', exact=True)).to_be_enabled()
            queued = [json.loads(body) for method, path, body, _ in calls[queue_call_start:]
                      if method == 'POST' and path == '/uploads']
            assert len(queued) == 10 and {item['kind'] for item in queued} == {'video', 'ad_image'}
            assert all(item['smallImageMode'] == 1 for item in queued if item['kind'] == 'ad_image')
            assert all(item['name'] != 'queue-first-0.mp4' for item in queued), 'Removed file uploaded'
            assert page.evaluate("sessionStorage.getItem('device-video-task')") is None
            page.locator('.van-toast').wait_for(state='hidden')

            # 第二项断线时停止队列；x 先取消原任务，再允许保留的第三项继续上传。
            state['fail_media_name'] = 'queue-fail.png'
            media_input.set_input_files([media_fixture('queue-ok.mp4'), media_fixture('queue-fail.png'), media_fixture('queue-after.mp4')])
            failure_start = len(calls)
            page.get_by_role('button', name='上传到设备', exact=True).click()
            expect(queue_rows).to_have_count(2)
            page.get_by_role('button', name='查询并取消未完成任务', exact=True).wait_for()
            expect(page.get_by_role('button', name='上传到设备', exact=True)).to_be_disabled()
            assert page.evaluate("sessionStorage.getItem('device-video-task')"), 'Active task not recoverable'
            created = [json.loads(body)['name'] for method, path, body, _ in calls[failure_start:]
                       if method == 'POST' and path == '/uploads']
            assert created == ['queue-ok.mp4', 'queue-fail.png'], 'Failure did not stop queue'
            page.locator('.van-toast').wait_for(state='hidden')
            queue_rows.nth(0).locator('.queue-remove').click()
            expect(queue_rows).to_have_count(1)
            expect(page.get_by_role('button', name='上传到设备', exact=True)).to_be_enabled()
            assert tasks[task_id]['state'] == 'CANCELLED', 'X left an active remote task'
            page.get_by_role('button', name='上传到设备', exact=True).click()
            expect(queue_rows).to_have_count(0)
            expect(page.get_by_role('button', name='保存播放设置', exact=True)).to_be_enabled()
            created = [json.loads(body)['name'] for method, path, body, _ in calls[failure_start:]
                       if method == 'POST' and path == '/uploads']
            assert created == ['queue-ok.mp4', 'queue-fail.png', 'queue-after.mp4'], 'Completed file was replayed'
            page.locator('.van-toast').wait_for(state='hidden')
            page.goto(origin + "/#/home/person")
            with page.expect_file_chooser() as chooser:
                page.get_by_text("选择 Excel 人员表", exact=True).click()
            chooser.value.set_files(
                {
                    "name": "任意文件.xlsx",
                    "mimeType": "application/octet-stream",
                    "buffer": b"excel-fixture",
                }
            )
            with page.expect_file_chooser() as chooser:
                page.get_by_text("选择人脸图片（可多选）", exact=True).click()
            chooser.value.set_files(
                {
                    "name": "web_a_1.jpg",
                    "mimeType": "image/jpeg",
                    "buffer": b"original-face-fixture",
                }
            )
            page.get_by_role("button", name="上传所选文件").click()
            page.get_by_role("button", name="开始导入", exact=True).wait_for()
            assert (
                len(manifest) == 2 and manifest[1]["name"] == "web_a_1.jpg"
            ), "Original image manifest lost"
            page.reload()
            page.get_by_role("button", name="开始导入", exact=True).wait_for()
            page.screenshot(path=str(output / "person-import.png"), full_page=True)
            page.get_by_role("button", name="开始导入", exact=True).click()
            page.get_by_role("button", name="确认", exact=True).click()
            page.get_by_text("完成", exact=True).wait_for()
            assert any(
                body == b"original-face-fixture"
                for _, path, body, _ in calls
                if path.endswith("/content")
            ), "Face bytes altered"
            page.goto(origin + "/#/home/device")
            page.get_by_placeholder("请输入原密码").fill("test-secret")
            page.get_by_placeholder("8–128", exact=True).fill("new-secret-123")
            page.get_by_placeholder("再次输入新密码").fill("new-secret-123")
            page.get_by_role("button", name="修改登录密码", exact=True).click()
            page.wait_for_url("**/#/login")
            assert any(path == "/std/device/change-password" for _, path, _, _ in calls)
            violations.extend(page.evaluate("window.testCspViolations"))
            assert not violations, f"CSP violations: {violations}"
            assert (
                not external_requests
            ), f"External resource requests: {external_requests}"
            assert not errors, "\n".join(errors)
            browser.close()
            (output / "browser-results.json").write_text(
                json.dumps(
                    {
                        "passed": True,
                        "requests": [(method, path) for method, path, _, _ in calls],
                        "note": "Actual device-webd static/CSP/logging; Qt browser fixtures only, not a board/TLS test",
                    },
                    ensure_ascii=False,
                    indent=2,
                ),
                encoding="utf-8",
            )
            print(
                "Browser checks passed: local reads/CRUD/credentials/media/password, capability controls, "
                "raw uploads, image destination, local-only content choices, touch time picker and cancellation, schedule add/save/delete, complete manifest, refresh recovery, offline icons, CSP, console logs"
            )
    finally:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=5)
        stdout.close()
        stderr.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--browser", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--server", type=Path, required=True, help="Built device-webd executable"
    )
    args = parser.parse_args()
    check(args.dist, args.browser, args.output, args.server)
