"""
@file browser_check.py
@brief 使用真实 Chromium 验证生产网页；隔离测试响应不进入交付前端。
@author Du lin
@date 2026-10-09
"""
import argparse
import functools
import http.server
import json
import threading
from pathlib import Path
from playwright.sync_api import sync_playwright


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    """只提供已构建静态文件，不输出每次浏览器资源请求。"""
    def log_message(self, *_args):
        """测试仅打印最终断言结果。"""


def check(dist: Path, executable: str, output: Path) -> None:
    """在移动端宽度检查所有保留路由、禁用业务和真实 File 上传方式。"""
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(QuietHandler, directory=str(dist.resolve())))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    calls, errors, tasks, assets, manifest = [], [], {}, [], []
    origin = f'http://127.0.0.1:{server.server_port}'
    task_id = '11111111-1111-4111-8111-111111111111'
    batch_id = '22222222-2222-4222-8222-222222222222'
    state = {'batch': 'OPEN'}

    def respond(route):
        """白名单测试桩：记录每个请求，意外业务立即使测试失败。"""
        request = route.request
        path = request.url.split('/api/v1', 1)[1]
        calls.append((request.method, path, request.post_data_buffer, request.headers))
        if path == '/device/info': data = {'deviceId': 'TEST-BOARD', 'deviceName': '测试设备', 'wifiName': 'DEVICE-TEST'}
        elif path in ('/auth/login', '/auth/session'): data = {'username': 'tester', 'csrf': 'test-csrf'}
        elif path == '/files': data = {'files': []}
        elif path == '/persons': data = {'persons': [{'id': 1, 'name': '本地测试甲', 'account': 'web_a', 'employeeNo': 'W001', 'phone': '13812345678'}]}
        elif path == '/persons/1': data = {'person': {'id': 1, 'name': '本地测试甲', 'account': 'web_a', 'employeeNo': 'W001', 'phone': '13812345678'}}
        elif path == '/uploads':
            item = request.post_data_json
            tasks[task_id] = {**item, 'id': task_id, 'state': 'CREATED'}
            data = tasks[task_id]
        elif path == f'/uploads/{task_id}/content':
            assert request.post_data_buffer, 'File body missing'
            assert request.headers.get('content-type') == 'application/octet-stream'
            assert request.headers.get('x-csrf-token') == 'test-csrf'
            tasks[task_id]['state'] = 'RECEIVED'; data = tasks[task_id]
        elif path == f'/uploads/{task_id}/commit':
            tasks[task_id]['state'] = 'READY'; data = tasks[task_id]
            if data['kind'] != 'video': assets.append({'name': data['name'], 'kind': data['kind']})
        elif path == '/imports':
            manifest[:] = request.post_data_json['manifest']
            data = {'id': batch_id, 'state': 'OPEN', 'result': {'manifest': manifest}, 'assets': assets}
        elif path == f'/imports/{batch_id}': data = {'id': batch_id, 'state': state['batch'], 'result': {'manifest': manifest}, 'assets': assets}
        elif path == f'/imports/{batch_id}/start':
            state['batch'] = 'DONE'; data = {'id': batch_id, 'state': 'DONE', 'result': {'inserted': 1, 'savedFaces': 1, 'failed': 0}}
        else:
            errors.append(f'Unexpected business request: {request.method} {path}')
            route.fulfill(status=404, json={'code': 404, 'message': 'not supported'}); return
        route.fulfill(json={'code': 200, 'data': data})

    try:
        with sync_playwright() as runtime:
            browser = runtime.chromium.launch(executable_path=executable, headless=True)
            page = browser.new_page(viewport={'width': 390, 'height': 844}, locale='zh-CN')
            page.on('pageerror', lambda error: errors.append(str(error)))
            page.route('**/api/v1/**', respond)
            page.goto(origin + '/#/login')
            page.locator('input[autocomplete="username"]').fill('tester')
            page.locator('input[type="password"]').fill('test-secret')
            page.locator('button.submit').click()
            page.wait_for_url('**/#/home/preview')
            assert page.locator('.van-tabbar-item').count() == 5, 'Original navigation missing'
            for route in ('/home/preview', '/home/air', '/home/elevator', '/device/settings', '/person/1', '/home/video'):
                page.goto(origin + '/#' + route)
                page.get_by_text('本版本未接入', exact=False).first.wait_for()
                page.wait_for_timeout(100)
                assert page.locator('[inert],button:disabled,input:disabled').count() > 0, 'Unsupported controls active'
            output.mkdir(parents=True, exist_ok=True)
            page.screenshot(path=str(output / 'video-page.png'), full_page=True)
            page.locator('input[type="file"]').set_input_files({'name': 'browser-video.mp4', 'mimeType': 'video/mp4', 'buffer': b'raw-test-video-bytes'})
            page.get_by_role('button', name='上传到设备').click()
            page.wait_for_function("!document.querySelector('button.van-button--loading')")
            page.wait_for_timeout(150)
            assert any(body == b'raw-test-video-bytes' for _, path, body, _ in calls if path.endswith('/content')), 'Video bytes altered or omitted'
            page.goto(origin + '/#/home/person')
            page.locator('input[accept=".xls,.xlsx"]').set_input_files({'name': '任意文件.xlsx', 'mimeType': 'application/octet-stream', 'buffer': b'excel-fixture'})
            page.locator('input[multiple]').set_input_files({'name': 'web_a_1.jpg', 'mimeType': 'image/jpeg', 'buffer': b'original-face-fixture'})
            page.get_by_role('button', name='上传所选文件').click()
            page.get_by_role('button', name='开始导入', exact=True).wait_for()
            assert len(manifest) == 2 and manifest[1]['name'] == 'web_a_1.jpg', 'Original image manifest lost'
            page.reload()
            page.get_by_role('button', name='开始导入', exact=True).wait_for()
            page.screenshot(path=str(output / 'person-import.png'), full_page=True)
            page.get_by_role('button', name='开始导入', exact=True).click()
            page.get_by_role('button', name='确认', exact=True).click()
            page.get_by_text('完成', exact=True).wait_for()
            assert any(body == b'original-face-fixture' for _, path, body, _ in calls if path.endswith('/content')), 'Face bytes altered'
            assert not errors, '\n'.join(errors)
            browser.close()
            (output / 'browser-results.json').write_text(json.dumps({'passed': True, 'requests': [(method, path) for method, path, _, _ in calls], 'note': 'Browser test fixtures only; not a board/TLS test'}, ensure_ascii=False, indent=2), encoding='utf-8')
            print('Browser checks passed: all routes, disabled controls, raw uploads, complete manifest, refresh recovery')
    finally:
        server.shutdown(); server.server_close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dist', type=Path, required=True)
    parser.add_argument('--browser', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    check(args.dist, args.browser, args.output)
