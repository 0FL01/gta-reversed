#!/usr/bin/env python3
"""Build/run the CPU-only native contact-blip readiness contract probe."""

import subprocess

from RealtimeScriptHostProbe import OUTPUT, build_probe, options


if __name__ == '__main__':
    args = options(__doc__)
    name = 'NativeContactBlipProbe'
    if args.build:
        build_probe(name)
    else:
        result = subprocess.run(
            [str(OUTPUT / name), str(args.game_dir.resolve())],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=180,
        )
        (OUTPUT / (name + '.log')).write_text(result.stdout)
        print(result.stdout, end='')
        result.check_returncode()
        assert ('NativeContactBlipProbe PASS actual=0570@205876 sprite=33 image=radar_race rgba=1024 '
                'cpu-consumer-fixture-only GL-ready-claim=false') in result.stdout
