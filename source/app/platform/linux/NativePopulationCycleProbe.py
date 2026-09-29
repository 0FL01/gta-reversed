"""Owned popcycle row oracle for the shared car/ped observation seam.

Clock/week/population settings are explicit observations; no runtime scheduling,
loaded assets, actor references or census completeness are certified here.
"""
import argparse
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
from NativeIplFlagsProbe import build_flags

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sanitized', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[5]
source = root / 'gta-reversed/source'
out = root / 'artifacts/graphics'
assert out.is_dir()
stem = 'NativePopulationCycleProbe' + ('-sanitized' if args.sanitized else '')
binary = root / 'build/godot-native/sa_loaded_cars_probe'
if args.sanitized:
    flags = build_flags()
    objects = []
    with (out / (stem + '.build.log')).open('w') as log:
        for name in ('NativeCarGeneratorPopulationProbe', 'NativeCarGeneratorPopulation',
                     'NativeCarGenerators', 'NativeZonePopulation', 'ZoneInfo',
                     'NativePedStreaming', 'NativePedModelMetadata', 'NativeWorldEntityInfo',
                     'NativeSourceRng', 'os_file_posix'):
            cpp = (root / 'godot/native/os_file_posix.cpp' if name == 'os_file_posix' else
                   source / 'app/platform/linux' / (name + '.cpp'))
            obj = out / (stem + '-' + name + '.o')
            command = ['g++', *flags, '-UNDEBUG', '-O1', '-fno-fast-math', '-ffp-contract=off',
                       '-fno-omit-frame-pointer', '-fsanitize=address,undefined',
                       '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra', '-Wpedantic',
                       '-Werror', '-c', str(cpp), '-o', str(obj)]
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
            objects.append(obj)
        binary = out / stem
        subprocess.run(['g++', '-fsanitize=address,undefined', '-Wl,--gc-sections',
                        *map(str, objects), '-pthread', '-o', str(binary)],
                       check=True, stdout=log, stderr=subprocess.STDOUT)
result = subprocess.run([str(binary), '/game', '--cycle-rows'], text=True, capture_output=True, check=True)
(out / (stem + '.log')).write_text(result.stdout + result.stderr)
expected_rows = []
for raw in Path('/game/data/popcycle.dat').read_text(encoding='latin1').splitlines():
    raw = raw.split('#', 1)[0].split('//', 1)[0].strip()
    if not raw:
        continue
    values = list(map(int, raw.split()))
    assert len(values) == 24 and all(0 <= v <= 255 for v in values)
    total = sum(values[6:])
    assert total > 0
    if total < 100:
        values[6:] = [v * 100 // total for v in values[6:]]
    expected_rows.append(values)
assert len(expected_rows) == 480
seen = set()
for line in result.stdout.splitlines():
    fields = line.split()
    if not fields or fields[0] != 'CYCLE':
        continue
    assert len(fields) == 43, fields
    poptype, weekend, hour, index = map(int, fields[1:5])
    label = fields[5]
    races, dealer, no_cops, *values = map(int, fields[6:])
    assert 0 <= poptype < 20 and weekend in (0, 1) and 0 <= hour < 24 and hour % 2 == 0
    assert index == (poptype * 2 + weekend) * 12 + hour // 2 and index not in seen
    assert label == 'ELS1a'
    assert (races, dealer, no_cops) == ((poptype + weekend + hour) % 16, poptype * 3, 0)
    assert values[:10] == [(poptype + gang) % 101 for gang in range(10)]
    assert values[10:] == expected_rows[index], ('source row/order/rescale', fields)
    seen.add(index)
assert seen == set(range(480))
assert result.stdout.splitlines()[-1].startswith('native-loaded-cars-ok ')
print('population-cycle-source-oracle-ok rows=480 clock-cases=960 regions=3 '
      'reader=shared source-zone=ELS1a settings=explicit slots=unowned census=incomplete')
print(result.stdout.splitlines()[-1])
