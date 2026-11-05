"""Compile current DSP and the saved pre-observation implementation, compare PCM.

Author: Du lin
Date: 2026-11-04
Uses the actual SpeexDSP host DLL, synthetic sounds, 16 switch combinations and
500 frames per combination. No hardware, production INI or network is accessed.
Requires this task's saved before/src/call/AudioProcessing.cpp snapshot.
"""
import os
from pathlib import Path
import subprocess


def main():
    """Build isolated executables and require identical PCM and existing counters."""
    root=Path.cwd()
    task=root/'.web-build/voice-cause-20261104'
    qt=root/'.web-build-tools/sdk/5.15.2/mingw81_64/bin'
    gcc=root/'.web-build-tools/sdk/Tools/mingw810_64/bin'
    env=dict(os.environ)
    env['PATH']=str(qt)+os.pathsep+str(gcc)+os.pathsep+env.get('PATH','')
    env['VOICE_TEST_SPEEX_LIBRARY']=str(root/'.web-build/speex-v2-20261031/speexdsp.dll')
    results=[]
    pcm_results=[]
    for variant in ('before','after'):
        output=task/('parity-'+variant)
        output.mkdir(parents=True,exist_ok=True)
        source=task/'before/src/call/AudioProcessing.cpp' if variant=='before' else root/'src/call/AudioProcessing.cpp'
        project='QT += core\nQT -= gui\nCONFIG += console c++17\nTARGET = dsp_parity\n'
        project+='INCLUDEPATH += "'+root.as_posix()+'" "'+(root/'src/call').as_posix()+'"\n'
        project+='SOURCES += "'+(root/'src/call/tests/dsp_observation_parity.cpp').as_posix()+'" "'+source.as_posix()+'" "'+(root/'src/call/SpeexDsp.cpp').as_posix()+'"\n'
        (output/'parity.pro').write_text(project,encoding='utf-8')
        for command,name in [([str(qt/'qmake.exe'),'parity.pro'],'qmake.log'),([str(gcc/'mingw32-make.exe'),'-j4'],'build.log')]:
            result=subprocess.run(command,cwd=output,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90)
            (output/name).write_bytes(result.stdout)
            if result.returncode:
                print(result.stdout.decode(errors='replace')[-5000:]);result.check_returncode()
        result=subprocess.run([str(output/'release/dsp_parity.exe'),str(output/'output.pcm')],cwd=output,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=90,check=True)
        (output/'pcm-hashes.csv').write_bytes(result.stdout)
        (output/'library.log').write_bytes(result.stderr)
        results.append(result.stdout.replace(b'\r\n',b'\n'))
        pcm_results.append((output/'output.pcm').read_bytes())
    assert len(results[0].splitlines())==16, 'Incomplete switch coverage'
    assert len(pcm_results[0])==16*500*320*2, 'Incomplete PCM output'
    assert pcm_results[0]==pcm_results[1], 'Observation changed actual PCM bytes'
    assert results[0]==results[1], 'Observation changed PCM or existing counters; inspect parity directories'
    print('PASS: before/after 5,120,000 PCM bytes and existing counters identical for 16 configurations x 500 frames')


if __name__=='__main__':
    main()
