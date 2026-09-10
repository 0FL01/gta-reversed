#!/usr/bin/env python3
"""Build/run production help timing with -Werror/UBSan; no assets or RW startup."""
import shlex
import subprocess

from RealtimeScriptHostProbe import SOURCE, WORKSPACE, OUTPUT


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    build = WORKSPACE / "build"
    commands = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", "mad-sa-linux"], text=True).splitlines()
    template = shlex.split(next(c for c in commands if "-c " in c and "/Realtime.cpp" in c))
    flags, i = [], 0
    while i < len(template):
        if template[i] in ("-MT", "-MF", "-o", "-c"):
            i += 2
        elif template[i] == "-MD":
            i += 1
        else:
            flags.append(template[i])
            i += 1
    # librw headers have unused parameters; all other warnings remain errors.
    flags += ["-UNDEBUG", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
              "-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-ffunction-sections", "-fdata-sections"]
    objects = []
    name = "NativeScriptEntitiesProbe"
    with (OUTPUT / (name + "-build.log")).open("w") as log:
        for unit in ("NativeScriptEntities", name):
            obj = OUTPUT / (unit + "-ubsan.o")
            command = flags + ["-c", str(SOURCE / (unit + ".cpp")), "-o", str(obj)]
            log.write(shlex.join(command) + "\n"); log.flush()
            subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(str(obj))
        subprocess.run([flags[0], *objects, "-fsanitize=undefined", "-Wl,--gc-sections", "-o", str(OUTPUT / name)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    result = subprocess.run([str(OUTPUT / name)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (OUTPUT / (name + ".log")).write_text(result.stdout)
    print(result.stdout, end="")
    result.check_returncode()


if __name__ == "__main__":
    main()
