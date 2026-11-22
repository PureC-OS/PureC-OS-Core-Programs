#!/usr/bin/env python3
import argparse
import os
import subprocess
import sys

DESC = {
    "init": "PureC OS init, getty, service control and power commands",
    "sh": "PureC OS command shell",
    "coreutils": "PureC OS file, text and shell utilities with manual pages",
    "net-tools": "PureC OS network daemons and network utilities",
    "disk-utils": "PureC OS disk, file system and initramfs utilities",
    "accounts": "PureC OS user identity commands and sudo",
    "system": "PureC OS system information and process utilities",
    "installer": "PureC OS system installer",
    "core-tests": "PureC OS kernel and libc test programs",
    "lua": "Lua interpreter",
    "micropython": "MicroPython interpreter",
    "doom": "Chocolate Doom with the Freedoom Phase 1 IWAD",
}

CORE = ["init", "sh", "coreutils", "net-tools", "disk-utils", "accounts", "system", "installer", "core-tests"]
DEPS = {"base": ["init", "sh", "coreutils", "net-tools", "disk-utils", "accounts", "system", "pkg"]}
EXTRA = {
    "lua": [("usr/bin/lua", "@lua")],
    "micropython": [("usr/bin/micropython", "@micropython")],
    "doom": [("usr/bin/doom", "@chocolate-doom"), ("usr/share/doom/freedoom1.wad", "assets/doom/freedoom1.wad")],
}


def load(path):
    packages = {}
    with open(path, encoding="utf-8") as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            name, dest, source, required = line.split()
            packages.setdefault(name, []).append((dest.lstrip("/"), source, required == "1"))
    return packages


def resolve(source, root, programs):
    if source.startswith("@"):
        return os.path.join(programs, source[1:])
    return os.path.join(root, source)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--programs", required=True)
    parser.add_argument("--conf", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--pkgrel", default="1")
    parser.add_argument("--out", required=True)
    parser.add_argument("--sign-key")
    args = parser.parse_args()

    mkpkg = os.path.join(args.root, "mk", "pkg", "mkpkg.py")
    pkgsign = os.path.join(args.root, "mk", "pkg", "pkgsign.py")
    packages = load(args.conf)
    for name, entries in EXTRA.items():
        packages[name] = [(d, s, False) for d, s in entries]
    os.makedirs(args.out, exist_ok=True)
    for old in os.listdir(args.out):
        if old.endswith(".pkg.tar.gz") or old.endswith(".pkg.tar.gz.sig"):
            os.remove(os.path.join(args.out, old))
    version = "%s-%s" % (args.version, args.pkgrel)
    built = []
    for name in CORE + ["lua", "micropython", "doom", "base"]:
        cmd = [sys.executable, mkpkg, "--name", name, "--version", version, "-o", args.out]
        count = 0
        if name == "base":
            cmd += ["--desc", "PureC OS base system (meta package)"]
            for dep in DEPS["base"]:
                cmd += ["--depend", dep]
        else:
            cmd += ["--desc", DESC[name]]
            for dest, source, required in packages.get(name, []):
                path = resolve(source, args.root, args.programs)
                if not os.path.isfile(path):
                    if required:
                        sys.exit("mkpackages: missing required %s for %s" % (path, name))
                    if name in EXTRA:
                        break
                    print("mkpackages: skip missing %s" % path)
                    continue
                cmd += ["--file", "%s=%s" % (dest, path)]
                count += 1
            if name in EXTRA and count == 0:
                print("mkpackages: %s not built, skipped" % name)
                continue
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
        filename = "%s-%s-x86_64.pkg.tar.gz" % (name, version)
        if args.sign_key and os.path.isfile(args.sign_key):
            full = os.path.join(args.out, filename)
            subprocess.run([sys.executable, pkgsign, "sign", args.sign_key, full, full + ".sig"], check=True)
        built.append((name, count))
        print("%-12s %3d files  %s" % (name, count, filename))


if __name__ == "__main__":
    main()
