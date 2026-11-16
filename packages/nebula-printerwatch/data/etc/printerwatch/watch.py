#!/usr/bin/env python3
"""Watch the Moonraker-backed printer connection.

Polls Moonraker for the printer / Klippy connection state. When the printer
transitions from an unhealthy state (Klippy in shutdown / error, or printer
flags reporting an error / closed state) to healthy (Klippy ready, printer
operational) -- e.g. after the Ender 3 V3 SE motherboard was switched off and
back on -- it fires the configured prime action: a G-code command delivered
through Moonraker's API.

While unhealthy, it prompts Moonraker to reset the Klipper firmware
(POST /printer/firmware_restart), which clears the Klipper shutdown state and
resets the MCU, so a powered-on motherboard can re-establish communication.

This lets the Nebula Pad prime/prepare for printing automatically when the
motherboard powers back on, instead of requiring a manual reset.
"""

import os
import time
import json
import subprocess
import urllib.request

MOONRAKER = os.environ.get("MOONRAKER_URL", "http://127.0.0.1:7125")
PRIME_CONF = "/etc/printerwatch/prime.conf"
STATE_PATH = "/api/printer"
INFO_PATH = "/server/info"
RESET_PATH = "/printer/firmware_restart"
GC_PATH = "/printer/gcode/script"
CAMERA_RESET_CMD = "/etc/init.d/S50usb_camera restart"
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
        with urllib.request.urlopen(MOONRAKER + path, timeout=5) as r:
            return True, json.loads(r.read().decode())
    except Exception:
        return False, None


def moonraker_post(path, payload):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        MOONRAKER + path,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=10) as r:
        return True, r.read().decode()


def unwrap(obj):
    """Moonraker wraps JSON-RPC results in a top-level "result" key; be
    tolerant of both wrapped and unwrapped payloads."""
    if isinstance(obj, dict) and isinstance(obj.get("result"), dict):
        return obj["result"]
    return obj


def printer_healthy(data, info):
    """True when Moonraker reports a Klippy that is connected, ready, and not
    in an error/closed state."""
    if not info:
        return False
    info = unwrap(info)
    if info.get("klippy_connected") is False:
        return False
    if (info.get("klippy_state") or "") != "ready":
        # ready == healthy; disconnected/startup/error/shutdown are not
        return False
    if not data:
        return False
    data = unwrap(data)
    st = data.get("state") or {}
    flags = st.get("flags") or {}
    if flags.get("error"):
        return False
    if flags.get("closedOrError"):
        return False
    return True


def fire_reset():
    """Ask Moonraker to reset the Klipper firmware (clears shutdown, resets
    MCU), via POST /printer/firmware_restart."""
    try:
        moonraker_post(RESET_PATH, {})
        return True
    except Exception as e:
        print("printerwatch: firmware restart failed:", e, flush=True)
        return False


def fire_camera_reset():
    """Restart the MJPG camera streamer so the camera streams come back up."""
    try:
        subprocess.run(["sh", "-c", CAMERA_RESET_CMD], check=False, timeout=30)
        return True
    except Exception as e:
        print("printerwatch: camera streamer restart failed:", e, flush=True)
        return False


def fire_prime(prime):
    """Send the configured G-code through Moonraker to prompt/prime the MCU."""
    try:
        moonraker_post(GC_PATH, {"script": prime["gcode"]})
        return True
    except Exception as e:
        print("printerwatch: prime failed:", e, flush=True)
        return False


def main():
    prime = read_prime()
    ok, data = moonraker_get(STATE_PATH)
    ok2, info = moonraker_get(INFO_PATH)
    prev_healthy = ok and ok2 and printer_healthy(data, info)
    reset_sent = False

    while True:
        ok, data = moonraker_get(STATE_PATH)
        ok2, info = moonraker_get(INFO_PATH)
        healthy = ok and ok2 and printer_healthy(data, info)

        if healthy and not prev_healthy:
            # Transitioned to healthy: fire the prime action once.
            fire_prime(prime)
        if not healthy and not reset_sent:
            # Unhealthy: prompt Moonraker to reset the Klipper firmware so the
            # connection to the (powered-on) MCU can re-establish, and restart
            # the MJPG camera streamer so the camera streams come back up.
            fire_reset()
            fire_camera_reset()
            reset_sent = True
        if healthy:
            reset_sent = False

        prev_healthy = healthy
        time.sleep(POLL_INTERVAL)


if __name__ == "__main__":
    main()
