#!/usr/bin/env python3
"""Watch the Moonraker-backed printer connection.

Polls Moonraker for the printer state. When the printer transitions from
offline/unreachable to online (e.g. after the Ender 3 V3 SE motherboard was
switched off and back on), it fires the configured prime action: a G-code
command delivered through Moonraker's API.

While the printer is unhealthy (errored / closed / unreachable), it prompts
the system to communicate with the MCU by sending the configured G-code through
Moonraker's API, so a powered-on motherboard is primed automatically instead of
requiring a manual reset of the Nebula Pad.

This lets the Nebula Pad prime/prepare for printing automatically when the
motherboard powers back on, instead of requiring a manual reset.
"""

import os
import time
import json
import urllib.request

MOONRAKER = os.environ.get("MOONRAKER_URL", "http://127.0.0.1:7125")
PRIME_CONF = "/etc/printerwatch/prime.conf"
STATE_PATH = "/api/printer"
POLL_INTERVAL = 5


def read_prime():
    prime = {"gcode": "G28", "printer": "printer"}
    try:
        with open(PRIME_CONF, "r") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                k = k.strip()
                v = v.strip()
                if k == "gcode":
                    prime["gcode"] = v
                elif k == "printer":
                    prime["printer"] = v
    except Exception:
        pass
    return prime


def moonraker_get(path):
    try:
        with urllib.request.urlopen(MOONRACKER + path, timeout=5) as r:
            return True, json.loads(r.read().decode())
    except Exception:
        return False, None


def moonraker_post(path, payload):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        MOONRACKER + path,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=10) as r:
        return True, r.read().decode()


def printer_healthy(data):
    """True when Moonraker reports a printer that is not errored/closed."""
    if not data:
        return False
    st = data.get("state") or {}
    flags = st.get("flags") or {}
    if flags.get("error"):
        return False
    if flags.get("closedOrError"):
        return False
    return True


def fire_prime(prime):
    """Send the configured G-code through Moonraker to prompt/prime the MCU."""
    try:
        moonraker_post("/api/printer/gcode", {"printer": prime["printer"], "gcode": prime["gcode"]})
        return True
    except Exception as e:
        print("printerwatch: prime failed:", e, flush=True)
        return False


def main():
    prime = read_prime()
    ok, data = moonraker_get(STATE_PATH)
    prev_online = ok and printer_healthy(data)
    primed_while_unhealthy = False
    while True:
        ok, data = moonraker_get(STATE_PATH)
        online = ok and printer_healthy(data)

        if online and not prev_online:
            # Transitioned to online: fire the prime action once.
            fire_prime(prime)
        elif not online and not primed_while_unhealthy:
            # Printer is unhealthy: prompt the system to communicate with the MCU.
            fire_prime(prime)
            primed_while_unhealthy = True

        prev_online = online
        time.sleep(POLL_INTERVAL)


if __name__ == "__main__":
    main()
