# nebula-openssh

OpenSSH 10.6p2 for the **Nebula pad** (Ingenic X2000 / XBurst2, MIPS32r2, little-endian,
soft-float, Linux 4.4.94, glibc 2.29 system). Provides a working `sshd` server plus the
`ssh`, `ssh-keygen`, `scp` and `sftp-server` clients, packaged for opkg.

## What's in it

| Path | Purpose |
| --- | --- |
| `release/openssh-nebula_10.6p2-2_mipsel-3.4.ipk` | The installable opkg package (also attached to the GitHub release) |
| `init/S41sshd` | Boot script for the firmware's `/etc/init.d` runner |
| `package/` | opkg control files and the config file the package installs |
| `build/build.sh` | Reproduces the build from upstream source tarballs |
| `make-ipk.py` | Assembles the `.ipk` from `package/` |

## Why it's built this way

The binaries are statically linked against musl (via Zig's cross compiler), so they don't
depend on the pad's glibc. OpenSSL is built without threads or assembly, and OpenSSH without
its syscall sandbox. Two device-specific findings drove these choices:

- The kernel rejects the seccomp sandbox (`prctl(PR_SET_SECCOMP): Invalid argument`).
- The hard-float musl build crashed `sshd-auth` after login, so the package uses soft-float.

The package is configured with `--without-pam`, so there is no PAM and no keyboard-interactive
backend. `KbdInteractiveAuthentication no` is set in the default config; leaving it on causes
a crash after the password prompt.

## Install

### 1. Download the package

Download `openssh-nebula_10.6p2-2_mipsel-3.4.ipk` from the
[latest release](https://github.com/mattwaltbriggs/nebula-openssh/releases/latest) and copy it
to the pad over sftp, e.g. to `/root/`.

### 2. Install with opkg (on the pad)

```sh
opkg install /root/openssh-nebula_10.6p2-2_mipsel-3.4.ipk
```

The package's postinst creates the `sshd` user (uid/gid 74), `/var/empty`, and an ed25519
host key in `/etc/ssh/` if one isn't there already.

If you installed an earlier build (`10.6p2-1`), this is a newer version, so opkg upgrades it.
Your `/etc/ssh/sshd_config` is a conffile, so opkg asks before overwriting it. Keep your
version, and make sure it contains `KbdInteractiveAuthentication no`.

### 3. Check the config and start the server

```sh
/usr/sbin/sshd -t -f /etc/ssh/sshd_config && echo CONFIG_OK
/usr/sbin/sshd -f /etc/ssh/sshd_config
grep ':08AE ' /proc/net/tcp     # port 2222 in hex; a line with state 0A means listening
```

Use the full path `/usr/sbin/sshd`. On the Nebula, `/opt/sbin/sshd` (Entware's OpenSSH) comes
first on `PATH` and would run instead.

### 4. Start on boot

Copy the boot script into the firmware's init directory:

```sh
cp /root/S41sshd /etc/init.d/S41sshd    # from init/S41sshd in this repo
chmod 755 /etc/init.d/S41sshd
```

The firmware's `/etc/init.d/rcS` runs every `S??*` script with `start` at boot, and the
script refuses to start a second copy if one is already running.

Manage it with:

```sh
/etc/init.d/S41sshd start|stop|restart|status
```

Don't put this script in `/opt/etc/init.d`. Entware's `rc.unslung` isn't running on this
device, so those scripts don't run at boot anyway, and Entware's `S40sshd` starts its own sshd.

### 5. Connect

From another machine:

```sh
ssh -p 2222 root@<pad-ip>
```

## Configuration

The default `/etc/ssh/sshd_config`:

```
Port 2222
ListenAddress 0.0.0.0
HostKey /etc/ssh/ssh_host_ed25519_key
PermitRootLogin yes
PubkeyAuthentication yes
PasswordAuthentication yes
KbdInteractiveAuthentication no
Subsystem sftp /usr/libexec/sftp-server
PidFile /var/run/sshd.pid
```

- **Password login is on.** Add your public key to `/root/.ssh/authorized_keys`, then set
  `PasswordAuthentication no` and restart.
- **Port 2222** avoids clashing with Entware's sshd on port 22.
- **Debug output:** don't run `sshd -ddd` in the foreground for normal use. It sends debug lines
  to the connected client.

## Known limitations

- No PAM, no syslog-based auth auditing, no seccomp sandbox.
- Not tested with SFTP beyond a basic check.
- The package isn't signed.

## Building from source

Requires [Zig](https://ziglang.org/) 0.17.x (`brew install zig` on macOS), python3, and curl.

```sh
./build/build.sh          # fetches zlib, OpenSSL, OpenSSH and builds static MIPS binaries
python3 make-ipk.py       # writes openssh-nebula_10.6p2-2_mipsel-3.4.ipk
```

The build works in `~/nebula-build` by default (set `WORK=` to change that). Start from a fresh
clone, since the script expects `package/data/` to be empty of binaries.

## Licenses

OpenSSH is under its own BSD-style licenses, OpenSSL under Apache-2.0, and zlib under the zlib
license. See the upstream projects for the full text.
