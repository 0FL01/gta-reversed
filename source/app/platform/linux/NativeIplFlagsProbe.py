"""Read-only source-word oracle and pinned before/after native pager differential.

Run in mad-sa-graphics-build: python3 .../NativeIplFlagsProbe.py
Only generated probe objects/executables/evidence go to artifacts/graphics.
"""
import collections
import math
import pathlib
import shlex
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[5]
GAME = pathlib.Path('/game')
OUT = ROOT / 'artifacts/graphics'
SOURCE = ROOT / 'gta-reversed/source/app/platform/linux'
BASE = 'dc0815e8'


def path(relative):
    result = GAME
    for part in relative.replace('\\', '/').split('/'):
        matches = [p for p in result.iterdir() if p.name.lower() == part.lower()]
        assert len(matches) == 1, relative
        result = matches[0]
    return result


def rows(relative):
    for raw in path(relative).read_text(encoding='latin1').splitlines():
        text = ''.join(' ' if ord(c) < 32 or c == ',' else c for c in raw).lstrip()
        if text and text[0] not in '#;':
            yield text.split()


def source_population():
    models, ipls = {}, []
    for dat in ('data/gta.dat', 'data/default.dat'):
        for t in rows(dat):
            if t[0] == 'IPL': ipls.append(t[1])
            if t[0] != 'IDE': continue
            section = None
            for r in rows(t[1]):
                if len(r) == 1: section = None if r[0] == 'end' else r[0]
                elif section in ('objs', 'tobj', 'anim'): models[int(r[0])] = r[1].lower()
    expected = []
    bits = lambda value: struct.unpack('<I', struct.pack('<f', float(value)))[0]
    for ipl in ipls:
        section, record = None, 0
        for t in rows(ipl):
            if len(t) == 1: section = None if t[0] == 'end' else t[0]
            elif section == 'inst':
                assert len(t) == 11
                word = int(t[2]) & 0xffffffff
                assert models[int(t[0])] == t[1].lower()
                expected.append([ipl, str(record), '0', t[0], t[1].lower(), str(word & 255), str(word), t[10],
                                 *[str(bits(v)) for v in t[3:10]]])
                record += 1
    text_count = len(expected)
    loaded = set()
    for img in ('models/gta3.img', 'models/gta_int.img', 'models/player.img'):
        with path(img).open('rb') as f:
            magic, count = struct.unpack('<4sI', f.read(8)); assert magic == b'VER2'
            entries = [struct.unpack('<IHH24s', f.read(32)) for _ in range(count)]
            for sector, size, streaming, raw in entries:
                name = raw.split(b'\0')[0].decode('ascii').lower()
                if not name.endswith('.ipl') or name in loaded: continue
                loaded.add(name)
                f.seek(sector * 2048); data = f.read((size & 0x7fff) * 2048)
                assert data[:4] == b'bnry' and len(data) >= 76
                n, start = struct.unpack_from('<I', data, 4)[0], struct.unpack_from('<I', data, 28)[0]
                assert not n or (start >= 76 and start + n * 40 <= len(data))
                for record in range(n):
                    transform = struct.unpack_from('<7I', data, start + record * 40)
                    ident, word, lod = struct.unpack_from('<iIi', data, start + record * 40 + 28)
                    expected.append([str(path(img)) + ':' + name, str(record), '1', str(ident), models[ident],
                                     str(word & 255), str(word), str(lod), *map(str, transform)])
    return expected, text_count


def build_flags():
    commands = subprocess.check_output(['ninja', '-C', str(ROOT / 'build'), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
    args = shlex.split(next(c for c in commands if '-c ' in c and '/Realtime.cpp' in c))
    flags, i = [], 0
    while i < len(args):
        if args[i] in ('-MT', '-MF', '-o', '-c'): i += 2
        elif args[i] == '-MD': i += 1
        else: flags.append(args[i]); i += 1
    return flags


def candidates(population, window):
    anim = set()
    for dat in ('data/gta.dat', 'data/default.dat'):
        for t in rows(dat):
            if t[0] != 'IDE': continue
            section = None
            for r in rows(t[1]):
                if len(r) == 1: section = None if r[0] == 'end' else r[0]
                elif section == 'anim': anim.add(r[1].lower())
    f32 = lambda v: struct.unpack('<f', struct.pack('<f', v))[0]
    decode = lambda v: struct.unpack('<f', struct.pack('<I', int(v)))[0]
    x, y = [(2488.562255859375,-1666.864501953125),(2495,-1687),(325,2537),(-2026,156),(1540,-1736)][window]
    kept = []
    for order, p in enumerate(population):
        if int(p[5]) != 0 or p[4].startswith('lod') or p[4] in anim: continue
        dx, dy = f32(decode(p[8])-x), f32(decode(p[9])-y)
        distance = f32(math.sqrt(f32(f32(dx*dx) + f32(dy*dy))))
        if distance <= 900: kept.append((distance, order, tuple(p[:5])))
    kept.sort()
    assert len(kept) >= 8, 'actual comparison windows must not need sparse fallback'
    return [p[2] for p in kept[:4096]]


def run(command, label):
    result = subprocess.run(command, cwd=ROOT / 'build', capture_output=True, text=True, timeout=300)
    (OUT / (label + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode: print(result.stdout[-4000:], result.stderr)
    result.check_returncode()
    return [line.split('\t') for line in result.stdout.splitlines()]


def main():
    assert OUT.is_dir()
    flags = build_flags() + ['-UNDEBUG', '-ffunction-sections', '-fdata-sections']
    with (OUT / 'NativeIplFlagsProbe-build.log').open('w') as log:
        objects = []
        for unit in ('NativeIplFlagsProbe', 'StreamPager', 'NativeCollisionAssets', 'TexSample', 'RealtimeGameplay', 'Collide'):
            obj = OUT / ('iplflags-' + unit + '.o'); objects.append(str(obj))
            subprocess.run(flags + ['-Wall', '-Wextra', '-c', str(SOURCE / (unit + '.cpp')), '-o', str(obj)],
                           cwd=ROOT / 'build', stdout=log, stderr=subprocess.STDOUT, check=True)
        baseline = subprocess.check_output(['git', '-c', 'safe.directory=/workspace/gta-reversed', '-C', str(ROOT / 'gta-reversed'),
                                           'show', BASE + ':source/app/platform/linux/StreamPager.cpp'], text=True)
        before_obj = str(OUT / 'iplflags-StreamPager-before.o')
        subprocess.run(flags + ['-x', 'c++', '-c', '-', '-o', before_obj], input=baseline, text=True,
                       cwd=ROOT / 'build', stdout=log, stderr=subprocess.STDOUT, check=True)
        for variant in ('before', 'after'):
            linked = [before_obj if variant == 'before' and a.endswith('iplflags-StreamPager.o') else a for a in objects]
            subprocess.run([flags[0], '-Wl,--gc-sections', '-Wl,--wrap=fopen', *linked, 'vendor/librw/src/librw.a',
                            '-lpthread', '-lm', '-o', str(OUT / ('NativeIplFlagsProbe-' + variant))],
                           cwd=ROOT / 'build', stdout=log, stderr=subprocess.STDOUT, check=True)
    expected, text_count = source_population()
    high = [p for p in expected[:text_count] if int(p[6]) & ~255]
    assert len(high) == 996, len(high)
    summaries = ['SOURCE upper-word census (area,LOD-prefix): ' + str(dict(sorted(collections.Counter(
        (int(p[5]), p[4].startswith('lod')) for p in high).items())))]
    for mode in ('offline', 'runtime'):
        results = {variant: run([str(OUT / ('NativeIplFlagsProbe-' + variant)), str(GAME), mode],
                                'NativeIplFlagsProbe-' + variant + '-' + mode) for variant in ('before', 'after')}
        old, new = results['before'], results['after']
        actual = [r[1:] for r in new if r[0] == 'IPL']
        prior = [r[1:] for r in old if r[0] == 'IPL']
        reference = expected if mode == 'runtime' else []
        assert len(actual) == len(prior) == len(reference)
        for a, b, e in zip(actual, prior, reference):
            legacy = e.copy()
            if e[2] == '0':
                word = int(e[6]); legacy[5] = str(word if word < 2**31 else word - 2**32); legacy[6] = '0'
            assert b == legacy, (b, legacy)
            assert a == (e if mode == 'runtime' else legacy), (a, e)
        if mode == 'offline':
            assert old == new, 'legacy population/resident geometry/counters changed'
            summaries.append('PASS offline export rejection, load counters and five resident windows identical to ' + BASE)
            continue
        words = {tuple(e[:5]): int(e[6]) for e in expected}
        for kind in ('COL', 'RENDER'):
            for window in range(5):
                b = {tuple(r[2:7]): r[7:] for r in old if r[:2] == [kind, str(window)]}
                a = {tuple(r[2:7]): r[7:] for r in new if r[:2] == [kind, str(window)]}
                assert all(a[k] == b[k] for k in a.keys() & b.keys()), (kind, window, 'common geometry changed')
                added, removed = a.keys() - b.keys(), b.keys() - a.keys()
                if kind == 'COL':
                    assert not removed
                    assert all(k[2] == '0' and words[k] & ~255 and words[k] & 255 == 0 for k in added)
                    # Preserve complete COL traversal order, not just membership.
                    assert [r[2:7] for r in new if r[:2] == [kind,str(window)] and tuple(r[2:7]) in b] == [
                        r[2:7] for r in old if r[:2] == [kind,str(window)]]
                else:
                    for population, output, rendered in ((prior,old,b),(actual,new,a)):
                        selected = candidates(population,window)
                        assert set(rendered) <= set(selected), ('render outside authored grid/cap',window)
                        assert [tuple(r[2:7]) for r in output if r[:2] == [kind,str(window)]] == [k for k in selected if k in rendered], (
                            'candidate source/distance order changed',window)
                    # All removed residents were displaced by the unchanged cap,
                    # not silently dropped by some new model/geometry filter.
                    selected = candidates(actual,window)
                    assert not removed.intersection(selected), ('unexpected existing candidate loss',window)
                    skipped = set(selected) - a.keys()
                    if skipped: summaries.append(f'RENDER window={window} selected-but-unrenderable={sorted(skipped)}')
                summaries.append(f'{kind} window={window} instances={len(b)}->{len(a)} added={len(added)} removed={len(removed)} '
                                 f'triangles={sum(int(v[0]) for v in b.values())}->{sum(int(v[0]) for v in a.values())}')
        summaries += ['BEFORE ' + '\t'.join(r) for r in old if r[0] in ('LOAD','WINDOW')]
        summaries += ['AFTER ' + '\t'.join(r) for r in new if r[0] in ('LOAD','WINDOW')]
        summaries += ['AFTER ' + '\t'.join(r) for r in new if r[0].startswith('PASS')]
    synthetic = run([str(OUT / 'NativeIplFlagsProbe-after'), str(GAME), 'synthetic'], 'NativeIplFlagsProbe-synthetic')
    summaries += ['\t'.join(r) for r in synthetic if r[0].startswith('PASS')]
    summaries.append(f'PASS independent raw IPL oracle: {len(expected)} ordered records, {text_count} text, 996 upper words; binary/identity/transform unchanged')
    report = '\n'.join(summaries) + '\n'
    (OUT / 'NativeIplFlagsProbe-summary.log').write_text(report)
    print(report, end='')


def checks():
    # Execute the affected existing offline contract verbatim (only output paths
    # change). These modes use surfaceless EGL; no desktop or environment changes.
    for line in (ROOT / 'tools/etalon-sweep.sh').read_text().splitlines():
        if not line.startswith('chk '): continue
        _, label, expected, *command = shlex.split(line)
        if label not in ('smoke', 'e2e', 'drive', 'walk'): continue
        command = [t.replace('$B', str(ROOT / 'build/mad-sa-linux')).replace('$O/', str(OUT / 'iplflags-etalon-')) for t in command]
        result = run(command, 'NativeIplFlagsProbe-etalon-' + label)
        assert expected in '\n'.join('\t'.join(r) for r in result), (label, expected)
        print('PASS unchanged etalon', label, expected, flush=True)
    # The runtime curb lane uses current product objects, including the genuine
    # native OS wrapper and controller, with only this probe replacing main.
    commands = subprocess.check_output(['ninja', '-C', str(ROOT / 'build'), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
    line = next(c for c in commands if ' -o mad-sa-linux ' in c)
    link = shlex.split(next(p for p in line.split('&&') if ' -o mad-sa-linux ' in p))
    link = [a for a in link if not any(a.endswith('/' + unit + '.cpp.o') for unit in ('MainLinux', 'Realtime'))]
    obj = OUT / 'iplflags-curb.o'
    with (OUT / 'NativeIplFlagsProbe-curb-build.log').open('w') as log:
        subprocess.run(build_flags() + ['-UNDEBUG', '-DNATIVE_IPLFLAGS_PRODUCT_OS', '-ffunction-sections', '-fdata-sections',
            '-c', str(SOURCE / 'NativeIplFlagsProbe.cpp'), '-o', str(obj)], cwd=ROOT / 'build', stdout=log, stderr=subprocess.STDOUT, check=True)
        link[1:1] = ['-Wl,--gc-sections', '-Wl,--wrap=fopen', str(obj)]
        link[link.index('-o') + 1] = str(OUT / 'NativeIplFlagsProbe-curb')
        subprocess.run(link, cwd=ROOT / 'build', stdout=log, stderr=subprocess.STDOUT, check=True)
    result = run([str(OUT / 'NativeIplFlagsProbe-curb'), str(GAME), 'curb'], 'NativeIplFlagsProbe-curb')
    print('\n'.join('\t'.join(r) for r in result), flush=True)


def stream(game):
    for mode in ('run', 'close-cpu', 'close-gpu', 'close-retire'):
        result = run([str(OUT / 'realtime-streaming-probe'), str(pathlib.Path(game).resolve()), mode],
                     'NativeIplFlagsProbe-stream-' + mode)
        text = '\n'.join('\t'.join(r) for r in result)
        assert 'stream-probe PASS mode=' + mode in text
        print(text, flush=True)


if __name__ == '__main__':
    if len(sys.argv) == 1: main()
    elif sys.argv[1:] == ['--checks']: checks()
    elif len(sys.argv) == 3 and sys.argv[1] == '--stream': stream(sys.argv[2])
    else: raise SystemExit('usage: NativeIplFlagsProbe.py [--checks | --stream GAME_DIR]')
