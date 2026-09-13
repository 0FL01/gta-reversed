#!/usr/bin/env python3
"""Ninja-derived isolated real-asset placement probe; outputs only artifacts."""
import argparse
import pathlib
import shlex
import subprocess

SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
OUTPUT = WORKSPACE / 'artifacts/graphics'


def build():
    directory = WORKSPACE / 'build'
    commands = subprocess.check_output(['ninja', '-C', str(directory), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
    template = shlex.split(next(c for c in commands if '-c ' in c and '/Realtime.cpp' in c))
    flags, i = [], 0
    while i < len(template):
        if template[i] in ('-MT', '-MF', '-o', '-c'):
            i += 2
        elif template[i] == '-MD':
            i += 1
        else:
            flags.append(template[i]); i += 1
    line = next(c for c in commands if ' -o mad-sa-linux ' in c)
    link = shlex.split(next(p for p in line.split('&&') if ' -o mad-sa-linux ' in p))
    units = ['StreamPager', 'NativeCollisionAssets', 'NativeGarages', 'NativeScriptSchema',
              'NativeScriptCorpus', 'NativeScriptSession', 'NativeScriptServiceTransaction', 'NativeStuntJumps', 'NativeScriptEntities',
             'RealtimeGameplay', 'RealtimeScriptHost', 'NativePlacementProbe']
    excluded = units + ['MainLinux', 'Realtime']
    link = [a for a in link if not any(a.endswith('/'+n+'.cpp.o') for n in excluded)]
    OUTPUT.mkdir(parents=True, exist_ok=True)
    link[link.index('-o')+1] = str(OUTPUT / 'NativePlacementProbe')
    with (OUTPUT / 'NativePlacementProbe-build.log').open('w') as log:
        for unit in units:
            obj = OUTPUT / ('placement-'+unit+'.o')
            command = flags + ['-UNDEBUG', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections',
                               '-c', str(SOURCE / (unit+'.cpp')), '-o', str(obj)]
            log.write(shlex.join(command)+'\n'); log.flush()
            subprocess.run(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
            link.insert(1, str(obj))
        link.insert(1, '-Wl,--gc-sections')
        # Linker wraps prove real pager/RW entry calls stay off main while the
        # worker owns parsers. Derive mangled names from these exact build objects.
        specs = [
            ('StreamPager_Update', str(OUTPUT / 'placement-StreamPager.o'), 'bool',
             'float x, float y, float z, WorldShotScene& s, E2EPagerFrame& f, char* e, std::size_t n, const std::shared_ptr<const NativePlacementOverrides>& o, std::vector<NativePlacementIdentity>* p',
             'x,y,z,s,f,e,n,o,p'),
            ('TexSample_LinkedParse', next(a for a in link if a.endswith('/TexSample.cpp.o')), 'LinkedClump',
             'const uint8_t* b, std::size_t n, rw::TexDictionary* p, rw::TexDictionary* const* f, std::size_t c, rw::TexDictionary* v',
             'b,n,p,f,c,v'),
        ]
        wrappers = ['#include "app/platform/linux/StreamPager.h"', '#include "app/platform/linux/TexSample.h"',
                    'void NativePlacementProbe_ParserThread();']
        for name, obj, result, parameters, arguments in specs:
            symbols = subprocess.check_output(['nm', '--defined-only', obj], cwd=directory, text=True).splitlines()
            symbol, = [line.split()[-1] for line in symbols if len(line.split()) == 3 and line.split()[1] == 'T'
                       and (line.split()[-1].startswith('_Z' + str(len(name)) + name) or line.split()[-1] == name)
                       and not line.split()[-1].endswith('.cold')]
            wrappers += [f'{result} Real{name}({parameters}) asm("__real_{symbol}");',
                         f'{result} Wrap{name}({parameters}) asm("__wrap_{symbol}");',
                         f'{result} Wrap{name}({parameters}) {{ NativePlacementProbe_ParserThread(); return Real{name}({arguments}); }}']
            link.insert(1, '-Wl,--wrap='+symbol)
        wrapper_source = OUTPUT / 'placement-thread-wrappers.cpp'
        wrapper_source.write_text('\n'.join(wrappers)+'\n')
        wrapper_obj = OUTPUT / 'placement-thread-wrappers.o'
        subprocess.run(flags + ['-c', str(wrapper_source), '-o', str(wrapper_obj)], cwd=directory,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
        link.insert(1, str(wrapper_obj))
        log.write(shlex.join(link)+'\n'); log.flush()
        subprocess.run(link, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
        # Pin the pre-placement algorithm so this remains reproducible after commit.
        # Only adapt its signature to the new header; do not alter its body.
        baseline = subprocess.check_output(['git', '-c', 'safe.directory='+str(SOURCE.parents[3]), '-C', str(SOURCE.parents[3]), 'show',
            '3c250733db4832c6639c7d5f9f8ee86879663f15:source/app/platform/linux/StreamPager.cpp'], text=True)
        old = 'char* err, std::size_t errSize) {\n    const float kRadius = s_options.radius;'
        new = ('char* err, std::size_t errSize, const std::shared_ptr<const NativePlacementOverrides>&, '
               'std::vector<NativePlacementIdentity>*) {\n    const float kRadius = s_options.radius;')
        if baseline.count(old) != 1:
            raise RuntimeError('pinned pager baseline signature changed; review offline comparison')
        # P2-A04 moved this unchanged composition body from Assets.cpp to the
        # current StreamPager.cpp. The older pager baseline predates that move,
        # while this probe deliberately links the current Assets.cpp, so append
        # the same body to keep the offline algorithm comparison link-complete.
        context_loader = '''
std::shared_ptr<const NativeCollisionContext> NativeCollisionContext::LoadBeforeWorker(
    const char* gameDir, float radius, std::string& error) {
    if (!std::isfinite(radius) || radius <= 0) { error="invalid source COL residency radius"; return {}; }
    auto context=std::make_shared<NativeCollisionContext>();
    context->Radius=radius;
    if (!StreamPager_CollisionPopulation(context->Population,error) ||
        !context->Assets.Load(gameDir,context->Population,error)) return {};
    return context;
}
'''
        baseline_source = OUTPUT / 'placement-StreamPager-baseline.cpp'
        baseline_source.write_text(baseline.replace(old, new) + context_loader)
        baseline_obj = OUTPUT / 'placement-StreamPager-baseline.o'
        subprocess.run(flags + ['-ffunction-sections', '-fdata-sections', '-c', str(baseline_source), '-o', str(baseline_obj)],
                       cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
        baseline_link = [str(baseline_obj) if a == str(OUTPUT / 'placement-StreamPager.o') else a for a in link]
        baseline_link[baseline_link.index('-o')+1] = str(OUTPUT / 'NativePlacementProbe-baseline')
        subprocess.run(baseline_link, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
    print(OUTPUT / 'NativePlacementProbe')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument('--build', action='store_true')
    modes.add_argument('--run', action='store_true')
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--game-dir', type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.build:
        build()
    else:
        command = [str(OUTPUT / 'NativePlacementProbe'), str(args.game_dir.resolve())]
        if args.offline:
            command.append('--offline')
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
        (OUTPUT / ('NativePlacementProbe-offline.log' if args.offline else 'NativePlacementProbe.log')).write_text(result.stdout)
        print(result.stdout, end='')
        result.check_returncode()
        if args.offline:
            command[0] = str(OUTPUT / 'NativePlacementProbe-baseline')
            baseline = subprocess.check_output(command, text=True, stderr=subprocess.STDOUT, timeout=240)
            (OUTPUT / 'NativePlacementProbe-offline-baseline.log').write_text(baseline)
            if baseline != result.stdout:
                raise RuntimeError('offline default hashes differ from pinned pager')
            print('PASS offline default hashes match pre-placement pager 3c250733')
