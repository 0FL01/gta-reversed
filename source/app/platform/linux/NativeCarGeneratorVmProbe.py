#!/usr/bin/python3
"""Generated SCM VM fixture gate; ASan/UBSan, pinned schema and local source.

Run inside mad-sa-graphics-build. Output stays in artifacts/graphics. This gate
does not claim actual mission command counts, generator spawning or world boot.
"""
import hashlib
import json
import pathlib
import shlex
import subprocess
import urllib.request


def verify_source(source):
    revision = '53ed1c2561bf6ca70dc16afca5d8f3a406066158'
    with urllib.request.urlopen(
            f'https://raw.githubusercontent.com/sannybuilder/library/{revision}/sa/sa.json', timeout=60) as response:
        data = response.read()
    assert hashlib.sha256(data).hexdigest() == '797f32be6d3ebae87fd65b57ccc0c0b1cbc2e129c089668761e366740b5bd671'
    commands = {c['id']: c for e in json.loads(data)['extensions'] if e['name'] == 'default'
                for c in e['commands'] if 'id' in c}
    create, switch = commands['014B'], commands['014C']
    assert create['num_params'] == 13
    assert [p['type'] for p in create['input']] == ['float'] * 4 + ['model_vehicle', 'int', 'int', 'bool', 'int', 'int', 'int', 'int']
    assert create['output'] == [{'name': 'handle', 'type': 'CarGenerator', 'source': 'var_any'}]
    assert 'modelId -1 selects a random vehicle from the local popcycle' in create['short_desc']
    assert switch['num_params'] == 2 and [p['type'] for p in switch['input']] == ['CarGenerator', 'int']
    vehicle = (source / 'game_sa/Scripts/Commands/VehicleCommands.cpp').read_text()
    body = vehicle.split('void SwitchCarGenerator(int32 generatorId, int32 count) {', 1)[1].split('\n}', 1)[0]
    assert 'if (count)' in body and 'generator->SwitchOn();' in body and 'if (count <= 100)' in body
    assert 'generator->m_nGenerateCount = count;' in body and 'generator->SwitchOff();' in body
    generator_h = (source / 'game_sa/CarGenerator.h').read_text()
    assert 'uint16       m_nGenerateCount;' in generator_h
    generator_cpp = (source / 'game_sa/CarGenerator.cpp').read_text()
    assert 'm_nGenerateCount = (uint16)-1;' in generator_cpp
    print(f'source PASS schema={revision} 014B=4float+8integer+var_any 014C=int32-to-source-uint16 randomModel=-1', flush=True)


workspace = pathlib.Path('/workspace')
build = workspace / 'build'
source = workspace / 'gta-reversed/source'
output = workspace / 'artifacts/graphics'
output.mkdir(parents=True, exist_ok=True)
verify_source(source)
commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
template = shlex.split(next(c for c in commands if '-c ' in c and '/Handling.cpp' in c))
flags = []
i = 0
while i < len(template):
    arg = template[i]
    if arg in ('-MT', '-MF', '-o', '-c'):
        i += 2
    elif arg in ('-MD', '-DNDEBUG'):
        i += 1
    else:
        flags.append(arg)
        i += 1
sanitize = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-D_GLIBCXX_ASSERTIONS']
objects = []
log_path = output / 'NativeCarGeneratorVmProbe-build.log'
with log_path.open('w') as log:
    for relative in ('app/platform/linux/NativeScriptSchema.cpp',
                     'app/platform/linux/NativeScriptCorpus.cpp',
                     'app/platform/linux/NativeScriptSession.cpp',
                     'app/platform/linux/NativeCarGeneratorVmProbe.cpp', 'oswrapper/oswrapper_linux.cpp'):
        obj = output / ('CarGeneratorVm-' + pathlib.Path(relative).stem + '.o')
        result = subprocess.run(flags + ['-O1', '-g', '-Wall', '-Wextra', '-Werror'] + sanitize +
                                ['-c', str(source / relative), '-o', str(obj)], cwd=build, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            log.flush()
            print(log_path.read_text())
            result.check_returncode()
        objects.append(str(obj))
    original = next(c for c in commands if ' -o mad-sa-linux ' in c)
    link = [arg for arg in shlex.split(original.split('&&')[1]) if not arg.endswith('.o')]
    link[link.index('-o') + 1] = str(output / 'NativeCarGeneratorVmProbe')
    link[1:1] = objects + sanitize
    result = subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        log.flush()
        print(log_path.read_text())
        result.check_returncode()
run_log = output / 'NativeCarGeneratorVmProbe.log'
with run_log.open('w') as log:
    result = subprocess.run([str(output / 'NativeCarGeneratorVmProbe')], cwd=workspace, stdout=log, stderr=subprocess.STDOUT)
print(run_log.read_text(), end='')
result.check_returncode()
