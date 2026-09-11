"""Read-only independent raw IPL + source-binding oracle. Run in mad-sa-graphics-build.

python3 -B /workspace/gta-reversed/source/app/platform/linux/NativeLodCatalogProbe.py
Generated objects and derived evidence only: artifacts/graphics/NativeLodCatalog*.
Runs the synthetic on-disk malformed-reader gate (valid baseline + four single-
descriptor mutations) in a tempdir under artifacts/graphics, then the 50935
real-data oracle. No /game writes, no tmp, no copied game bytes in the fixture.
"""
import collections
import json
import math
import pathlib
import shlex
import shutil
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[5]
SOURCE = ROOT / 'gta-reversed/source/app/platform/linux'
OUT = ROOT / 'artifacts/graphics'
GAME = pathlib.Path('/game')


def path(relative):
    result = GAME
    for part in relative.replace('\\', '/').split('/'):
        matches = [p for p in result.iterdir() if p.name.lower() == part.lower()]
        assert len(matches) == 1, relative
        result = matches[0]
    return result


def rows(relative):
    for number, raw in enumerate(path(relative).read_text(encoding='latin1').splitlines(), 1):
        text = ''.join(' ' if ord(c) < 32 or c == ',' else c for c in raw).lstrip()
        if text and text[0] != '#':
            yield number, text.split()


def raw_catalog():
    models, texts = {}, []
    archives = [('MODELS\\GTA3.IMG', 'CStreaming::InitImageList@0x4083C0', 0),
                ('MODELS\\GTA_INT.IMG', 'CStreaming::InitImageList@0x4083C0', 0)]
    for dat in ('data/default.dat', 'data/gta.dat'):
        for number, t in rows(dat):
            if t[0] == 'EXIT': break
            if t[0] == 'IMG' and t[1] != 'MODELS\\GTA_INT.IMG': archives.append((t[1], dat, number))
            if t[0] == 'IPL': texts.append((t[1], dat, number))
            if t[0] != 'IDE': continue
            section = None
            for _, r in rows(t[1]):
                if len(r) == 1: section = None if r[0] == 'end' else r[0]
                elif section in ('objs', 'tobj', 'anim'):
                    draw = float(r[4] if section == 'anim' else r[3])
                    if section != 'anim' and draw < 4: draw = float(r[4])
                    models[int(r[0])] = (r[1].lower(), {'objs': 1, 'tobj': 2, 'anim': 3}[section], draw)
    sources, placements = [], []

    def add_source(key, name, binary, dat, line, archive='', order=0, record=0, sector=0, sectors=0):
        index = len(sources)
        sources.append([key, name, int(binary), dat, line, archive, order, record, sector, sectors, 0])
        return index

    def add(s, ident, name, flags, lod, transform):
        assert models[ident][0] == name
        placements.append(dict(source=s, local=sources[s][-1], ident=ident, name=name, flags=flags,
                               lod=lod, transform=transform, kind=models[ident][1], draw=models[ident][2]))
        sources[s][-1] += 1

    for key, dat, line in texts:
        s = add_source(key, pathlib.PureWindowsPath(key).stem.lower(), False, dat, line)
        section = None
        for _, t in rows(key):
            if len(t) == 1: section = None if t[0] == 'end' else t[0]
            elif section == 'inst':
                assert len(t) == 11
                add(s, int(t[0]), t[1].lower(), int(t[2]) & 0xffffffff, int(t[10]), struct.pack('<7f', *map(float, t[3:10])))
    text_count = len(placements)
    loaded = set()
    for order, (archive, dat, line) in enumerate(archives):
        with path(archive).open('rb') as f:
            magic, count = struct.unpack('<4sI', f.read(8)); assert magic == b'VER2'
            directory = [struct.unpack('<IHH24s', f.read(32)) for _ in range(count)]
            for record, (sector, size, packed, raw) in enumerate(directory):
                name = raw.split(b'\0')[0].decode('ascii').lower()
                if not name.endswith('.ipl') or name[:-4] in loaded: continue
                loaded.add(name[:-4]); sectors = packed or size
                s = add_source(str(path(archive)) + ':' + name, name[:-4], True, dat, line, archive, order, record, sector, sectors)
                f.seek(sector * 2048); data = f.read(sectors * 2048)
                assert data[:4] == b'bnry'
                n, at = struct.unpack_from('<I', data, 4)[0], struct.unpack_from('<I', data, 28)[0]
                assert not n or (at >= 76 and at + n * 40 <= len(data))
                for r in range(n):
                    transform = data[at+r*40:at+r*40+28]
                    ident, flags, lod = struct.unpack_from('<iIi', data, at+r*40+28)
                    add(s, ident, models[ident][0], flags, lod, transform)
    return sources, placements, text_count


def oracle(sources, placements):
    arrays = collections.defaultdict(list)
    for n, p in enumerate(placements): arrays[p['source']].append(n)
    parents = []
    for p in placements:
        s = p['source']; source = sources[s]
        # Source-binding oracle is independently executed from the raw archive
        # directory and DAT declarations, never from graph/pager names or indices.
        candidates = [s] if not source[2] else [i for i, t in enumerate(sources)
            if not t[2] and source[1][:len(t[1])+7].lower() == t[1].lower() + '_stream']
        p['parent_source'] = candidates[0] if len(candidates) == 1 else -1
        lod = p['lod']
        if lod == -1: status, parent = 0, -1
        elif lod < -1: status, parent = 4, -1
        elif not candidates: status, parent = 2, -1
        elif len(candidates) != 1: status, parent = 3, -1
        elif lod >= len(arrays[candidates[0]]): status, parent = 4, -1
        else: status, parent = 1, arrays[candidates[0]][lod]
        p['status'] = status; parents.append(parent)
    # Actual source data must be acyclic; C++ adversarial fixtures exercise errors.
    for start in range(len(placements)):
        seen = set(); n = start
        while n >= 0:
            assert n not in seen, ('raw source cycle', start)
            seen.add(n); n = parents[n]
    counts = collections.Counter(p for p in parents if p >= 0)
    return parents, counts


def build():
    commands = subprocess.check_output(['ninja', '-C', str(ROOT / 'build'), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
    args = shlex.split(next(c for c in commands if '-c ' in c and '/Realtime.cpp' in c))
    flags, i = [], 0
    while i < len(args):
        if args[i] in ('-MT', '-MF', '-o', '-c'): i += 2
        elif args[i] == '-MD': i += 1
        else: flags.append(args[i]); i += 1
    flags += ['-UNDEBUG', '-ffunction-sections', '-fdata-sections']
    objects = []
    with (OUT / 'NativeLodCatalog-build.log').open('w') as log:
        for unit in ('NativeLodCatalog', 'NativeLodCatalogProbe', 'NativeWorldEntityInfo', 'StreamPager', 'TexSample'):
            obj = OUT / ('NativeLodCatalog-' + unit + '.o'); objects.append(str(obj))
            subprocess.run(flags + ['-Wall', '-Wextra', '-c', str(SOURCE / (unit + '.cpp')), '-o', str(obj)],
                           cwd=ROOT / 'build', stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run([flags[0], '-Wl,--gc-sections', *objects, 'vendor/librw/src/librw.a', '-lpthread', '-lm',
                        '-o', str(OUT / 'NativeLodCatalogProbe')], cwd=ROOT / 'build', stdout=log, stderr=subprocess.STDOUT, check=True)


def transform_hash(data):
    value = 1469598103934665603
    for byte in data: value = ((value ^ byte) * 1099511628211) & (2**64 - 1)
    return value


def main():
    assert OUT.is_dir()
    build()
    fixture = pathlib.Path(tempfile.mkdtemp(prefix='NativeLodCatalog-fixture-', dir=OUT))
    try:
        result = subprocess.run([str(OUT / 'NativeLodCatalogProbe'), str(GAME), str(fixture)],
                                capture_output=True, text=True, timeout=300)
        (OUT / 'NativeLodCatalog-probe.log').write_text(result.stdout + result.stderr)
        result.check_returncode()
        output = [line.split('\t') for line in result.stdout.splitlines()]
        disk = [r for r in output if r[0] == 'DISKFIXTURE']
        assert disk == [['DISKFIXTURE', 'pass', 'baseline,other-section,advisory-size,advertised-range,actual-inst-range,actual-cargen-range,inst-budget,cumulative']], \
            ('synthetic disk fixture', disk)
    except BaseException:
        print('fixture dir retained: ' + str(fixture))
        raise
    shutil.rmtree(fixture, ignore_errors=True)
    sources, placements, text_count = raw_catalog()
    actual_sources = [r[2:] for r in output if r[0] == 'SOURCE']
    assert actual_sources == [[str(v) for v in s] for s in sources], 'source order/provenance mismatch'
    parents, counts = oracle(sources, placements)
    actual = [r[2:] for r in output if r[0] == 'NODE']
    statuses = collections.Counter(p['status'] for p in placements)
    assert len(sources) == 242 and sum(not s[2] for s in sources) == 52 and sum(bool(s[2]) for s in sources) == 190
    assert len(actual) == len(placements) == 50935 and text_count == 9268 and len(placements) - text_count == 41667
    assert sum(parent >= 0 for parent in parents) == 6103 and len(counts) == 6086 and statuses == {0: 44832, 1: 6103}
    # Integrated source-review census: claims to verify, not targets to fit.
    # Time-kind and distinct-ID counts are independently derivable from the raw
    # IDE/IPL decode above; class counts come from the authoritative C++ metadata
    # (IDE objs/tobj/anim + object.dat) via the probe CENSUS line.
    assert sum(p['kind'] == 2 for p in placements) == 161, 'time placements 161'
    assert len(set(p['ident'] for p in placements)) == 12839, 'placed static model IDs 12839 (not the 14259-entry IDE namespace)'
    census = [r[1:] for r in output if r[0] == 'CENSUS']
    assert len(census) == 1, 'missing integrated CENSUS line'
    assert census[0] == ['242', '52', '190', '50935', '9268', '41667',
                         '6103', '6086', '0', '161', '12839',
                         '34759', '68', '16108', '0'], ('CENSUS mismatch', census[0])
    for i, (p, a) in enumerate(zip(placements, actual)):
        # SetupRelatedIpls' ppCurrIplInstance tail contains only streamed rows
        # with a raw LOD index; 0x5B5285 is not reached for unlinked binary rows.
        trigger = lambda multiplier: (0 if sources[p['source']][2] and p['status'] == 0 else
                                      2 if counts[i] or p['draw'] * multiplier > 300 else 1)
        expected = [p['source'], p['local'], p['ident'], p['name'], p['flags'], p['lod'], transform_hash(p['transform']),
                    p['status'], p['parent_source'], parents[i], counts[i], p['kind'], trigger(1), trigger(2)]
        assert a == list(map(str, expected)), (i, a, expected)
    # Same exact child+parent identities assessed at two distance multipliers in
    # actual LS startup and distant-city populations. This is graph preparation,
    # not a renderer visibility oracle: parent rendering is an independent barrier.
    samples = []
    for label, x, y in [('LS-startup', 2488.562255859375, -1666.864501953125), ('SF-distant-city', -2026, 156)]:
        eligible = [i for i, p in enumerate(placements) if parents[i] >= 0 and not p['flags'] & 255 and
                    math.dist(struct.unpack('<7f', p['transform'])[:2], (x, y)) < 900]
        assert len(eligible) >= 2
        chosen = []
        for i in sorted(eligible, key=lambda i: math.dist(struct.unpack('<7f', placements[i]['transform'])[:2], (x, y))):
            if parents[i] in [parents[j] for j in chosen]: continue
            chosen.append(i)
            if len(chosen) == 2: break
        assert len(chosen) == 2
        for i in chosen:
            p, parent = placements[i], placements[parents[i]]
            samples.append(dict(window=label, candidates=len(eligible), child=[sources[p['source']][0], p['local'], p['ident'], p['name']],
                                parent=[sources[parent['source']][0], parent['local'], parent['ident'], parent['name']],
                                raw_lod=p['lod'], children=counts[parents[i]],
                                child_pose=list(struct.unpack('<7f', p['transform'])), parent_pose=list(struct.unpack('<7f', parent['transform'])),
                                lod_multipliers=[1, 2], parent_identity_stable=True))
    special = {}
    for label, predicate in [
            ('multiple_children', lambda i, p: counts[parents[i]] > 1),
            ('different_parent_pose', lambda i, p: p['transform'] != placements[parents[i]]['transform']),
            ('time_model_child', lambda i, p: p['kind'] == 2),
            ('text_child', lambda i, p: not sources[p['source']][2]),
            ('flagged_text', lambda i, p: not sources[p['source']][2] and p['flags'] & ~255)]:
        eligible = [i for i, p in enumerate(placements) if parents[i] >= 0 and predicate(i, p)]
        examples = []
        for i in eligible[:2]:
            p, parent = placements[i], placements[parents[i]]
            examples.append(dict(child=[sources[p['source']][0], p['local'], p['ident'], p['name']],
                                 parent=[sources[parent['source']][0], parent['local'], parent['ident'], parent['name']],
                                 child_pose=list(struct.unpack('<7f', p['transform'])),
                                 parent_pose=list(struct.unpack('<7f', parent['transform'])),
                                 flags=p['flags'], raw_lod=p['lod'], child_count=counts[parents[i]]))
        special[label] = dict(count=len(eligible), examples=examples)
    report = dict(population=len(placements), text=text_count, binary=len(placements)-text_count,
                  text_sources=sum(not s[2] for s in sources), binary_sources=sum(bool(s[2]) for s in sources),
                  edges=sum(n >= 0 for n in parents), lod_targets=len(counts),
                  statuses=dict(statuses),
                  child_count_histogram=dict(sorted(collections.Counter(counts.values()).items())),
                  time_placements=sum(p['kind'] == 2 for p in placements),
                  distinct_model_ids=len(set(p['ident'] for p in placements)),
                  census=census[0], samples=samples, source_cases=special,
                  graphics='UNRESOLVED: renderer does not consume graph',
                  runtime_semantics='Unknown: collision-load ownership, LinkLods mutation order/cache path, time counterpart residency')
    (OUT / 'NativeLodCatalog-oracle.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k not in ('samples', 'source_cases')}, indent=2))


if __name__ == '__main__':
    main()
