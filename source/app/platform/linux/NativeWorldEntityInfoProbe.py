"""Standalone metadata gate inside mad-sa-graphics-build (/workspace, /game:ro).

Builds the module/probe and current pager/COL dependencies. Independent Python
source/asset census checks every emitted model and all
class counts. No game asset is copied and no product build file is changed.
"""
import collections
import pathlib
import shlex
import struct
import subprocess
import sys
import zlib
sys.dont_write_bytecode = True
from NativeIplFlagsProbe import source_population

ROOT = pathlib.Path('/workspace')
GAME = pathlib.Path('/game')
OUT = ROOT / 'artifacts/graphics'
SOURCE = ROOT / 'gta-reversed/source/app/platform/linux'


def path(relative):
    result = GAME
    for part in relative.replace('\\', '/').split('/'):
        matches = [p for p in result.iterdir() if p.name.lower() == part.lower()]
        assert len(matches) == 1, relative
        result = matches[0]
    return result


def rows(relative):
    # Independent token grammar on the valid actual asset corpus.
    for line, raw in enumerate(path(relative).read_text(encoding='latin1').splitlines(), 1):
        text = ''.join(' ' if ord(c) < 32 or c == ',' else c for c in raw).lstrip()
        if text and text[0] not in '#;':
            yield line, text.split()


def reference():
    declarations = []
    ipls = []
    for dat in ('data/default.dat', 'data/gta.dat'):
        for _, tokens in rows(dat):
            if tokens[0] == 'IDE': declarations.append(tokens[1])
            if tokens[0] == 'IPL': ipls.append(tokens[1])
    models = {}
    for ide in declarations:
        section = None
        for line, t in rows(ide):
            if len(t) == 1:
                section = None if t[0] == 'end' else t[0]
                continue
            if section in ('objs', 'tobj', 'anim', 'cars', 'peds', 'weap', 'hier'):
                entry = dict(name=t[1], section=section, ide=ide, line=line,
                             anim=section == 'anim' and t[3] != 'null', objects=[], default=-1,
                             txd=t[2], animName=t[3] if section == 'anim' else '',
                             timeOn=None, timeOff=None)
                if section == 'tobj':
                    entry['timeOn'], entry['timeOff'] = int(t[-2]), int(t[-1])
                    assert -(2 ** 31) <= entry['timeOn'] < 2 ** 31 and -(2 ** 31) <= entry['timeOff'] < 2 ** 31
                if section == 'anim':
                    assert len(t) >= 6
                models[int(t[0])] = entry
    keys = collections.defaultdict(list)
    key = lambda name: zlib.crc32(name.upper().encode('ascii')) ^ 0xffffffff
    for ident, model in models.items(): keys[key(model['name'])].append(ident)
    for line, t in rows('data/object.dat'):
        if t[0].startswith('*'): break
        assert len(t) >= 13
        floats = [struct.unpack('f', struct.pack('f', float(v)))[0] for v in t[1:8]]
        ints = list(map(int, t[8:13]))
        matches = keys[key(t[0])]
        assert len(matches) <= 1, (line, t[0], matches)
        if not matches: continue
        m = models[matches[0]]
        m['objects'].append(line)
        m['default'] = -1
        damage, special, camera = [v & 255 for v in ints[:3]]
        if floats[0] == 99999 and floats[6] == 1 and damage == 0 and special in (0, 4):
            m['default'] = (2 if special == 4 else 0) + int(camera != 0)
    static = {ident: m for ident, m in models.items() if m['section'] in ('objs', 'tobj', 'anim')}
    for m in static.values(): m['class'] = 3 if m['objects'] else 2 if m['anim'] else 1
    population = collections.Counter()
    for ipl in ipls:
        section = None
        for _, t in rows(ipl):
            if len(t) == 1:
                section = None if t[0] == 'end' else t[0]
            elif section == 'inst':
                assert len(t) == 11
                population[static[int(t[0])]['class']] += 1
    for img in ('models/gta3.img', 'models/gta_int.img', 'models/player.img'):
        with path(img).open('rb') as f:
            magic, count = struct.unpack('<4sI', f.read(8))
            assert magic == b'VER2'
            entries = [struct.unpack('<IHH24s', f.read(32)) for _ in range(count)]
            for sector, size, streaming, name in entries:
                if not name.split(b'\0')[0].lower().endswith(b'.ipl'): continue
                f.seek(sector * 2048)
                data = f.read((streaming or size) * 2048)
                assert data[:4] == b'bnry' and len(data) >= 76
                n = struct.unpack_from('<I', data, 4)[0]
                start = struct.unpack_from('<I', data, 28)[0]
                assert not n or (start >= 76 and start + n * 40 <= len(data))
                for i in range(n):
                    ident = struct.unpack_from('<i', data, start + i * 40 + 28)[0]
                    population[static[ident]['class']] += 1
    return static, population


def main():
    assert OUT.is_dir()
    commands = subprocess.check_output(['ninja', '-C', str(ROOT / 'build'), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
    args = shlex.split(next(c for c in commands if '-c ' in c and '/Realtime.cpp' in c))
    flags = []
    i = 0
    while i < len(args):
        if args[i] in ('-MT', '-MF', '-o', '-c'): i += 2
        elif args[i] == '-MD': i += 1
        else: flags.append(args[i]); i += 1
    objects = []
    with (OUT / 'NativeWorldEntityInfoProbe-build.log').open('w') as log:
        for unit in ('NativeWorldEntityInfo', 'NativeWorldEntityInfoProbe', 'StreamPager', 'NativeCollisionAssets', 'TexSample'):
            obj = OUT / ('entityinfo-' + unit + '.o')
            command = flags + ['-UNDEBUG', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections', '-c', str(SOURCE / (unit + '.cpp')), '-o', str(obj)]
            if unit.startswith('NativeWorldEntityInfo'): command.insert(1, '-Werror')
            subprocess.run(command, cwd=ROOT / 'build', stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(str(obj))
        executable = OUT / 'NativeWorldEntityInfoProbe'
        subprocess.run([flags[0], '-Wl,--gc-sections', *objects, 'vendor/librw/src/librw.a', '-lpthread', '-lm', '-o', str(executable)],
                       cwd=ROOT / 'build', stdout=log, stderr=subprocess.STDOUT, check=True)
    result = subprocess.run([str(executable), str(GAME)], capture_output=True, text=True)
    (OUT / 'NativeWorldEntityInfoProbe.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        print(result.stdout)
        print(result.stderr)
        result.check_returncode()
    models, population = reference()
    source, _ = source_population()
    text_words = {(p[0], int(p[1]), int(p[3])): int(p[6]) for p in source if p[2] == '0'}
    seen = set()
    window = collections.Counter()
    added = collections.Counter()
    added_rows = set()
    legacy = None
    for line in result.stdout.splitlines():
        t = line.split('\t')
        if t[0] == 'MODEL':
            _, ident, name, cls, ide, row, nobjects, last, default, txd, animName, timeOn, timeOff = t
            ident = int(ident); m = models[ident]; seen.add(ident)
            assert (name, int(cls), ide, int(row), int(nobjects), int(last), int(default)) == (
                m['name'], m['class'], m['ide'], m['line'], len(m['objects']), m['objects'][-1] if m['objects'] else 0, m['default']), (ident, t, m)
            assert txd == m['txd'] and animName == m['animName'], (ident, t, m)
            assert timeOn == ('none' if m['timeOn'] is None else str(m['timeOn'])) and timeOff == (
                'none' if m['timeOff'] is None else str(m['timeOff'])), (ident, t, m)
            assert bool(txd) and (m['section'] != 'anim' or bool(animName)), (ident, t, m)
            assert (m['section'] == 'tobj') == (timeOn != 'none' and timeOff != 'none'), (ident, t, m)
            assert (m['section'] == 'anim') == bool(animName), (ident, t, m)
        elif t[0] == 'WINDOW': window[models[int(t[1])]['class']] += 1
        elif t[0] == 'ADDED':
            key = (t[1], int(t[2]), int(t[3]))
            assert key not in added_rows
            added_rows.add(key)
            assert text_words[key] == int(t[4]) and int(t[4]) & ~255 and int(t[4]) & 255 == 0, t
            added[models[key[2]]['class']] += 1
        elif t[0] == 'LEGACY':
            legacy = list(map(int, t[1:]))
            assert legacy[0] == 0 and legacy[1] + legacy[2] == 3081 and legacy[3] == 2699 and sum(legacy) == 5780
            print(line)
        elif t[0] == 'POPULATION':
            assert list(map(int, t[1:])) == [population[i] for i in range(4)], (t, population)
            print(line)
        elif t[0] == 'STARTUP':
            assert list(map(int, t[1:])) == [window[i] for i in range(4)], (t, window)
            print(line)
        else: print(line)
    assert seen == models.keys(), (len(seen), len(models))
    assert legacy is not None
    assert [window[i] for i in range(4)] == [legacy[i] + added[i] for i in range(4)]
    print('PASS source-word-proven startup additions:', dict(sorted(added.items())), 'rows', len(added_rows))
    counts = collections.Counter(m['class'] for m in models.values())
    print('PASS independent actual Object.dat/IDE/IPL oracle:', len(seen), 'models; class counts', dict(sorted(counts.items())))


if __name__ == '__main__':
    main()
