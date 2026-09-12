#!/usr/bin/python3
"""Isolated real-asset + generated-fixture VM probe, in mad-sa-graphics-build.

Uses native ninja flags/libraries; compiles the actual session and OS wrapper.
No CMake/product target changes. Only artifacts/graphics receives build outputs.
The executable's services are explicitly TEST mocks, not native world evidence.
Checks the 53-command first-WAIT prefix, 117-command mission-0 numeric/policy prefix,
and independent generated scheduler/operand/rollback fixtures under UBSan.
"""
import pathlib
import shlex
import subprocess
import sys


def inspect_mission_prefix(game_dir):
    """Development-only schema inspection; emits numbers, never asset strings."""
    import collections
    import hashlib
    import json
    import struct
    import urllib.request

    revision = '53ed1c2561bf6ca70dc16afca5d8f3a406066158'
    with urllib.request.urlopen(
            f'https://raw.githubusercontent.com/sannybuilder/library/{revision}/sa/sa.json', timeout=60) as response:
        schema_bytes = response.read()
    assert hashlib.sha256(schema_bytes).hexdigest() == '797f32be6d3ebae87fd65b57ccc0c0b1cbc2e129c089668761e366740b5bd671'
    schema = json.loads(schema_bytes)
    commands = {int(c['id'], 16): c for e in schema['extensions'] if e['name'] == 'default'
                for c in e['commands'] if 'id' in c}
    data = (pathlib.Path(game_dir) / 'data/script/main.scm').read_bytes()
    pos = 0
    for _ in range(2):
        pos = struct.unpack_from('<I', data, pos + 3)[0]
    start = struct.unpack_from('<I', data, pos + 24)[0]
    pos = start
    counts = collections.Counter()
    print('schema', revision, schema['meta']['version'])
    for op in range(0x0008, 0x0018):
        print('numeric-schema', json.dumps(commands[op], sort_keys=True))
    while True:
        ip = pos
        op = struct.unpack_from('<H', data, pos)[0]
        c = commands[op]
        specs = c.get('input', []) + c.get('output', [])
        assert c['num_params'] == len(specs)
        pos += 2
        args = []
        for spec in specs:
            tag = data[pos]
            pos += 1
            if tag in (7, 8):
                args.append((tag, struct.unpack_from('<HHBB', data, pos)))
                pos += 6
            else:
                fmt = {1: 'i', 2: 'H', 3: 'H', 4: 'b', 5: 'h', 6: 'f', 9: '8s'}[tag]
                value = struct.unpack_from('<' + fmt, data, pos)[0]
                args.append((tag, len(value) if tag == 9 else value))
                pos += struct.calcsize(fmt)
        print(200000 + ip - start, f'{op:04X}', c['name'], args, 'next', 200000 + pos - start)
        if op == 0x09B4:
            break
        assert not c.get('attrs', {}).get('is_branch'), 'stop at unanalysed control flow'
        counts[op] += 1
    print('pre-service instructions', sum(counts.values()), 'opcode counts', {f'{op:04X}': n for op, n in counts.items()})


if len(sys.argv) > 1 and sys.argv[1] == '--inspect-mission-prefix':
    inspect_mission_prefix(sys.argv[2] if len(sys.argv) > 2 else '/game')
    sys.exit(0)

workspace = pathlib.Path('/workspace')
build = workspace / 'build'
source = workspace / 'gta-reversed/source'
output = workspace / 'artifacts/graphics'
output.mkdir(parents=True, exist_ok=True)
commands = subprocess.check_output(
    ['ninja', '-C', str(build), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
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
objects = []
log_path = output / 'NativeScriptSessionProbe-build.log'
with log_path.open('w') as log:
    for relative in ('app/platform/linux/NativeScriptSchema.cpp',
                     'app/platform/linux/NativeScriptCorpus.cpp',
                     'app/platform/linux/NativeScriptSession.cpp',
                     'app/platform/linux/NativeScriptSessionProbe.cpp',
                     'oswrapper/oswrapper_linux.cpp'):
        obj = output / ('NativeScript-' + pathlib.Path(relative).stem + '.o')
        command = flags + ['-g', '-Wall', '-Wextra', '-Werror',
                           '-fsanitize=undefined', '-fno-sanitize-recover=all',
                           '-c', str(source / relative), '-o', str(obj)]
        result = subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            log.flush()
            print(log_path.read_text())
            result.check_returncode()
        objects.append(str(obj))
    original = next(c for c in commands if ' -o mad-sa-linux ' in c)
    link = shlex.split(original.split('&&')[1])
    # Keep native library/link flags, not any other product translation units.
    link = [arg for arg in link if not arg.endswith('.o')]
    link[link.index('-o') + 1] = str(output / 'NativeScriptSessionProbe')
    link[1:1] = objects + ['-fsanitize=undefined', '-fno-sanitize-recover=all']
    result = subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        log.flush()
        print(log_path.read_text())
        result.check_returncode()
run_log = output / 'NativeScriptSessionProbe.log'
with run_log.open('w') as log:
    result = subprocess.run([str(output / 'NativeScriptSessionProbe'), '/game'],
                            cwd=workspace, stdout=log, stderr=subprocess.STDOUT)
print(run_log.read_text(), end='')
result.check_returncode()
