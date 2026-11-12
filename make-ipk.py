#!/usr/bin/env python3
"""Build opkg .ipk files from packages/<name>/{control,data}.

Usage: python3 make-ipk.py [package-name ...]   (default: all packages)
Output goes to release/<name>_<version>_<arch>.ipk.
"""
import io
import os
import subprocess
import sys
import tarfile
import tempfile

root = os.path.dirname(os.path.abspath(__file__))
packages_dir = os.path.join(root, "packages")

def read_control(path):
    fields = {}
    with open(path) as f:
        for line in f:
            if ":" in line:
                k, v = line.split(":", 1)
                fields[k.strip()] = v.strip()
    return fields


def tgz(src):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz", format=tarfile.USTAR_FORMAT) as t:
        for dirpath, dirnames, filenames in os.walk(src):
            for name in sorted(dirnames + filenames):
                full = os.path.join(dirpath, name)
                arc = "./" + os.path.relpath(full, src)
                info = t.gettarinfo(full, arcname=arc)
                info.uid = info.gid = 0
                info.uname = info.gname = "root"
                if info.isfile():
                    with open(full, "rb") as f:
                        t.addfile(info, f)
                else:
                    t.addfile(info)
    return buf.getvalue()


def build(name):
    pkg = os.path.join(packages_dir, name)
    ctl = read_control(os.path.join(pkg, "control", "control"))
    version, arch = ctl["Version"], ctl["Architecture"]
    out = os.path.join(root, "release", f"{name}_{version}_{arch}.ipk")
    os.makedirs(os.path.dirname(out), exist_ok=True)

    members = [
        ("debian-binary", b"2.0\n"),
        ("control.tar.gz", tgz(os.path.join(pkg, "control"))),
        ("data.tar.gz", tgz(os.path.join(pkg, "data"))),
    ]
    tmp = tempfile.mkdtemp()
    with tarfile.open(out, "w:gz", format=tarfile.USTAR_FORMAT) as t:
        for mname, data in members:
            path = os.path.join(tmp, mname)
            with open(path, "wb") as f:
                f.write(data)
            info = t.gettarinfo(path, arcname="./" + mname)
            info.uid = info.gid = 0
            info.uname = info.gname = "root"
            info.mode = 0o644
            with open(path, "rb") as f:
                t.addfile(info, f)
    print(out, os.path.getsize(out))
    return out


if __name__ == "__main__":
    names = sys.argv[1:] or sorted(os.listdir(packages_dir))
    for n in names:
        build(n)
