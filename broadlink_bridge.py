#!/usr/bin/env python3
"""One requested BroadLink operation per process; JSON in, JSON lines out."""
import base64
import ipaddress
import json
import re
import sys
import time


def packet_from_text(text):
    if not isinstance(text, str) or len(text) > 90000:
        raise ValueError("Invalid saved infrared command. Learn the button again.")
    packet = base64.b64decode(text, validate=True)
    if len(packet) < 6 or packet[0] != 0x26:
        raise ValueError("This is not a BroadLink infrared command.")
    length = int.from_bytes(packet[2:4], "little")
    if length < 2 or length > len(packet) - 4:
        raise ValueError("The saved infrared command is incomplete.")
    return packet


def describe(device):
    return {"id": device.mac.hex(), "host": device.host[0],
            "name": device.name or device.model or "BroadLink Mini",
            "model": device.model or device.TYPE,
            "locked": bool(device.is_locked)}


def execute(request, broadlink, progress=lambda message: None):
    if not isinstance(request, dict):
        raise ValueError("Invalid command request.")
    action = request.get("action")
    if action == "check":
        return {"message": "BroadLink support is ready."}
    if action == "discover":
        devices = broadlink.discover(timeout=5)
        hubs = {d.mac.hex(): describe(d) for d in devices
                if isinstance(d, broadlink.remote.rmmini)}
        return {"hubs": list(hubs.values())}
    if action not in ("identify", "learn", "send"):
        raise ValueError("Unknown infrared action.")
    address = ipaddress.IPv4Address(request.get("host", ""))
    if address.is_unspecified or address.is_multicast or address.is_loopback or str(address) == "255.255.255.255":
        raise ValueError("Enter the hub's own IPv4 address.")
    expected = request.get("id", "")
    if action != "identify" and not re.fullmatch(r"[0-9a-f]{12}", expected):
        raise ValueError("Discover or add this hub before sending commands.")
    # Reject invalid codes before contacting any device.
    packet = packet_from_text(request.get("code")) if action == "send" else None
    device = broadlink.hello(str(address), timeout=5)
    if not isinstance(device, broadlink.remote.rmmini):
        raise ValueError("This device is not a supported BroadLink infrared hub.")
    if action == "identify":
        return {"hubs": [describe(device)]}
    # DHCP addresses can change. Never send to a different hub at an old address.
    if device.mac.hex() != expected:
        raise ValueError("A different hub is using that address. Discover hubs again.")
    device.timeout = 5
    if device.is_locked:
        raise ValueError("Turn off 'Lock device' in the BroadLink phone app, then try again.")
    if not device.auth():
        raise ValueError("The hub refused authentication. Check 'Lock device' in the BroadLink app.")
    if action == "send":
        device.send_data(packet)
        return {"message": "Command sent. Check the TV or projector for its response."}
    device.enter_learning()
    progress("Point the original remote at the Mini and press the selected button once (30 seconds).")
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        try:
            packet = device.check_data()
        except broadlink.exceptions.StorageError:
            packet = None  # No command captured yet.
        if packet:
            code = base64.b64encode(packet).decode("ascii")
            packet_from_text(code)
            return {"code": code}
        time.sleep(0.25)
    raise ValueError("No infrared signal learned. Point the remote at the Mini and try again.")


def emit(payload):
    print(json.dumps(payload), flush=True)


def main():
    try:
        raw = sys.stdin.read(100001)
        if len(raw) > 100000:
            raise ValueError("Command request is too large.")
        request = json.loads(raw)
        try:
            import broadlink
            import broadlink.remote
        except ImportError as error:
            raise RuntimeError("Install the Scratch package python-broadlink and its dependencies, then click Check support.") from error
        result = execute(request, broadlink, lambda text: emit({"event": "progress", "message": text}))
        emit({"event": "result", "ok": True, **result})
        return 0
    except Exception as error:
        emit({"event": "result", "ok": False, "message": str(error)})
        return 1


if __name__ == "__main__":
    sys.exit(main())
