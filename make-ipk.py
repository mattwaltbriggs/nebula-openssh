import os, subprocess, sys, tarfile, io, tempfile
root = os.path.dirname(os.path.abspath(__file__))
pkg = os.path.join(root, "package")
out = os.path.join(root, "openssh-nebula_10.6p2-2_mipsel-3.4.ipk")

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

members = [
    ("debian-binary", b"2.0\n"),
    ("control.tar.gz", tgz(os.path.join(pkg, "control"))),
    ("data.tar.gz", tgz(os.path.join(pkg, "data"))),
]
out_dir = tempfile.mkdtemp()
for name, data in members:
    with open(os.path.join(out_dir, name), "wb") as f:
        f.write(data)
if os.path.exists(out):
    os.remove(out)
with tarfile.open(out, "w:gz", format=tarfile.USTAR_FORMAT) as t:
    for name, _ in members:
        info = t.gettarinfo(os.path.join(out_dir, name), arcname="./" + name)
        info.uid = info.gid = 0
        info.uname = info.gname = "root"
        info.mode = 0o644
        with open(os.path.join(out_dir, name), "rb") as f:
            t.addfile(info, f)
print(out, os.path.getsize(out))
