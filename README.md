# nebula-openssh

Packages for the **Nebula pad** (Ingenic X2000 / XBurst2, MIPS32r2, little-endian,
soft-float, Linux 4.4.94, glibc 2.29 system):

| Package | Version | What it does |
| --- | --- | --- |
| `openssh-nebula` | 10.6p2-4 | OpenSSH 10.6p2 `sshd` server, plus `ssh`, `ssh-keygen`, `scp`, `sftp-server` |
| `nebula-mdns` | 1.1-1 | Announces `nebula.local` and its services over mDNS (Bonjour) at startup |

Install `openssh-nebula` first. `nebula-mdns` is optional, but it's what lets you use
`ssh -p 2222 root@nebula.local` and find the pad in Bonjour browsers.

## What's in the repo

| Path | Purpose |
| --- | --- |
| `release/*.ipk` | Installable opkg packages (also attached to the GitHub release) |
| `packages/openssh-nebula/` | Control files, `S41sshd` boot script, `sshd_config` |
| `packages/nebula-mdns/` | Control files and the `S42mdns` boot script |
| `mdns/mdns-announce.c` | Source for the mDNS responder used by `nebula-mdns` |
| `build/build.sh` | Rebuilds everything from upstream source tarballs |
| `make-ipk.py` | Builds the `.ipk` files from `packages/` |

## Why it's built this way

The binaries are statically linked against musl (via Zig's cross compiler), so they don't
depend on the pad's glibc. OpenSSL is built without threads or assembly, and OpenSSH without
its syscall sandbox. Two device-specific findings drove these choices:

- The kernel rejects the seccomp sandbox (`prctl(PR_SET_SECCOMP): Invalid argument`).
- The hard-float musl build crashed `sshd-auth` after login, so the packages use soft-float.

The SSH package is configured with `--without-pam`, so there is no PAM and no keyboard-interactive
backend. `KbdInteractiveAuthentication no` is set in the default config; leaving it on causes
a crash after the password prompt.

## Install

### 1. Download the packages

Download the `.ipk` files from the
[latest release](https://github.com/mattwaltbriggs/nebula-openssh/releases/latest) and copy
them to the pad over sftp, e.g. to `/root/`.

### 2. Install openssh-nebula

If you copied `S41sshd` into `/etc/init.d` by hand earlier, remove it first, since the package
installs its own copy and opkg won't overwrite a file it doesn't own:

```sh
rm -f /etc/init.d/S41sshd
opkg install /root/openssh-nebula_10.6p2-4_mipsel-3.4.ipk
```

The postinst creates the `sshd` user (uid/gid 74), `/var/empty`, and an ed25519 host key in
`/etc/ssh/` if one isn't there already.

When upgrading, opkg asks before overwriting `/etc/ssh/sshd_config` and the init script,
since they are conffiles. Keep your version of `sshd_config`, and make sure it contains
`KbdInteractiveAuthentication no`.

### 3. Install nebula-mdns (optional)

```sh
rm -f /etc/init.d/S42mdns      # only if you copied one there by hand
opkg install /root/nebula-mdns_1.2-1_mipsel-3.4.ipk
```

### 4. Start the services

```sh
/etc/init.d/S41sshd start
/etc/init.d/S42mdns start
```

Check the SSH listener with `grep ':08AE ' /proc/net/tcp` (port 2222 in hex; state `0A`
means listening).

Use the full path `/usr/sbin/sshd` if you run it by hand. On the Nebula, `/opt/sbin/sshd`
(Entware's OpenSSH) comes first on `PATH` and would run instead. The init script already uses
the full path.

### 5. Start at boot

The firmware's `/etc/init.d/rcS` runs every `S??*` script in `/etc/init.d` with `start` at
boot. Each script refuses to start a second copy if one is already running. `S42mdns` runs
after `S41sshd`, so the SSH service is up when it's announced.

Manage a service with:

```sh
/etc/init.d/S41sshd start|stop|restart|status
/etc/init.d/S42mdns start|stop|restart|status
```

Don't put these scripts in `/opt/etc/init.d`. Entware's `rc.unslung` isn't running on this
device, so those scripts don't run at boot anyway, and Entware's `S40sshd` starts its own sshd.

### 6. Connect

```sh
ssh -p 2222 root@nebula.local
```

or `ssh -p 2222 root@<pad-ip>` if mDNS isn't working on your network.

## nebula.local (mDNS / Bonjour)

`nebula-mdns` announces the host `nebula.local` and a list of services. The list is in
`/etc/nebula-mdns/services.conf` (a conffile, so your edits survive upgrades):

```
label|type|port|path
Nebula|_ssh._tcp|@SSH_PORT@|
Nebula|_sftp-ssh._tcp|@SSH_PORT@|
Nebula|_sftp._tcp|@SSH_PORT@|
Nebula Creality|_http._tcp|80|/
Nebula Fluidd|_http._tcp|4408|/
Nebula Mainsail|_http._tcp|4409|/
Nebula Camera|_http._tcp|8080|/?action=stream
Nebula Camera 2|_http._tcp|8081|/?action=stream
Nebula Moonraker|_http._tcp|7125|/server/info
```

`@SSH_PORT@` is replaced at startup with the `Port` setting from `/etc/ssh/sshd_config`, so
the SSH and SFTP entries follow your SSH config. The path is published as a TXT `path=`
record, which Bonjour browsers use to build the URL.

To add a service, add a line and restart: `/etc/init.d/S42mdns restart`.

On the Mac, the services show up in these browsers:

| Browse for | Shows |
| --- | --- |
| `_ssh._tcp` | SSH access, `ssh -p 2222 root@nebula.local` |
| `_sftp-ssh._tcp`, `_sftp._tcp` | SFTP over SSH |
| `_http._tcp` | The Creality, Fluidd, and Mainsail web UIs, both cameras, and Moonraker |

Test from the Mac with:

```sh
dns-sd -G v4 nebula.local                  # address
dns-sd -B _http._tcp local                 # browse for web UIs, cameras, Moonraker
dns-sd -L "Nebula Fluidd" _http._tcp local # host, port and path for one service
```

To run it by hand: `/usr/sbin/mdns-announce -c <config> [-n <host>.local] [-a <ipv4>] [-i <ipv4>]`.

## Known limitations

- No PAM, no syslog-based auth auditing, no seccomp sandbox.
- SFTP has had only a basic check. Both SFTP service types point at the SSH port, so they
  work only if the SFTP client connects over SSH on that port.
- The packages aren't signed.
- `mdns-announce` uses the first non-loopback IPv4 interface it finds. If the pad has more than
  one active interface, pass `-i <ipv4>` in `/etc/init.d/S42mdns` to choose.
- `mdns-announce` was tested against macOS `dns-sd` from a development Mac. It hasn't been
  checked on the pad's network yet.
- The Moonraker entry points at its API, not a web page, so a browser opening it shows JSON.
- Port 9999 is still unidentified and isn't announced.

## Building from source

Requires [Zig](https://ziglang.org/) 0.17.x (`brew install zig` on macOS), python3, and curl.

```sh
./build/build.sh    # fetches zlib, OpenSSL, OpenSSH; builds static MIPS binaries and both .ipk files
```

The build works in `~/nebula-build` by default (set `WORK=` to change that).

## Licenses

OpenSSH is under its own BSD-style licenses, OpenSSL under Apache-2.0, and zlib under the zlib
license. See the upstream projects for the full text.
