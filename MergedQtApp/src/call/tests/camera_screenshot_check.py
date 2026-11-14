"""Build real camera screenshot controller, IPC and capture-loop host regressions.

Author: Du lin
Created: 2026-11-05
Note: 最近修改：2026-11-10；修改人：Du lin；编译声音设置通知的 moc 并执行即时保存回归。
Only the V4L2/RGA backend is replaced. A generated CameraService.cpp copy changes
its single platform guard to exercise the Linux capture loop on the Windows host.
No production files, real camera, broker, audio hardware or runtime INI are used.
Outputs go to .web-build/camera-screenshot-20261105; failed commands raise errors.
"""
import os
from pathlib import Path
import subprocess


def main():
    """Generate the isolated build, run Qt tests and preserve all build/test logs."""
    root = Path(__file__).resolve().parents[3]
    output = root / '.web-build/camera-screenshot-20261105'
    output.mkdir(parents=True, exist_ok=True)
    source = (root / 'modules/facegate/FaceGateQt/core/CameraService.cpp').read_text(encoding='utf-8')
    if source.count('#ifndef Q_OS_LINUX') != 1:
        raise ValueError('CameraService platform guard changed; review the fixture')
    (output / 'camera_service_host.cpp').write_text(source.replace('#ifndef Q_OS_LINUX', '#if 0'), encoding='utf-8')
    sources = ['src/call/tests/camera_screenshot_tests.cpp',
               'components/settings/mqtt/camera_screenshot_upload.cpp',
               'components/settings/mqtt/mqttipcclient.cpp',
               'components/settings/mqtt/mqttpacketparser.cpp']
    headers = ['common/device_feature_switches.h',
               'components/settings/mqtt/camera_screenshot_upload.h',
               'components/settings/mqtt/mqttipcclient.h',
               'modules/facegate/FaceGateQt/core/CameraService.h']
    includes = ['', 'modules/facegate/FaceGateQt/core', 'modules/facegate/FaceGateQt/config', 'components/rga']
    project = 'QT += core gui widgets network testlib\nCONFIG += console testcase c++17\nTARGET = camera_screenshot_tests\n'
    project += 'INCLUDEPATH += ' + ' '.join('"' + (root / p).as_posix() + '"' for p in includes) + '\n'
    project += 'SOURCES += camera_service_host.cpp ' + ' '.join('"' + (root / p).as_posix() + '"' for p in sources) + '\n'
    project += 'HEADERS += ' + ' '.join('"' + (root / p).as_posix() + '"' for p in headers) + '\n'
    (output / 'tests.pro').write_text(project, encoding='utf-8')
    qt = root / '.web-build-tools/sdk/5.15.2/mingw81_64/bin'
    gcc = root / '.web-build-tools/sdk/Tools/mingw810_64/bin'
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_QPA_FONTDIR='C:/Windows/Fonts')
    env['PATH'] = str(qt) + os.pathsep + str(gcc) + os.pathsep + env.get('PATH', '')
    for command, name in [([str(qt / 'qmake.exe'), 'tests.pro'], 'qmake.log'),
                          ([str(gcc / 'mingw32-make.exe'), '-j4'], 'build.log'),
                          ([str(output / 'release/camera_screenshot_tests.exe'), '-o', str(output / 'results.txt') + ',txt'], 'run.log')]:
        result = subprocess.run(command, cwd=output, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
        (output / name).write_bytes(result.stdout)
        if result.returncode:
            print(result.stdout.decode('utf-8', errors='replace')[-12000:])
            if name == 'run.log' and (output / 'results.txt').exists():
                print((output / 'results.txt').read_text(encoding='utf-8'))
            raise SystemExit(result.returncode)
    print((output / 'results.txt').read_text(encoding='utf-8'))


if __name__ == '__main__':
    main()
