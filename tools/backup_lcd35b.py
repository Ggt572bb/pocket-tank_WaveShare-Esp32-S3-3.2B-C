#!/usr/bin/env python3
"""Back up a 16 MB LCD35B board and bind its recovery image to its USB serial."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import sys

SIZE = 0x1000000


def port_serial(port):
    from serial.tools import list_ports
    matches = [p for p in list_ports.comports() if Path(p.device).resolve() == Path(port).resolve()]
    if len(matches) != 1 or not matches[0].serial_number:
        raise ValueError("Select a USB serial port with a unique serial number")
    return matches[0].serial_number.upper()


def verify_backup(path, serial):
    path = Path(path)
    meta = json.loads(Path(str(path) + ".json").read_text())
    if meta.get("usb_serial", "").upper() != serial.upper():
        raise ValueError("Backup belongs to another USB device")
    if path.stat().st_size != SIZE or meta.get("size") != SIZE:
        raise ValueError("Factory backup must contain exactly 16 MB")
    if hashlib.sha256(path.read_bytes()).hexdigest() != meta.get("sha256"):
        raise ValueError("Backup checksum does not match its metadata")
    return meta


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--output", type=Path, help="new factory image path; also creates <path>.json")
    mode.add_argument("--verify", type=Path, help="verify an existing image against the connected board")
    args = parser.parse_args()
    serial = port_serial(args.port)
    if args.verify:
        verify_backup(args.verify, serial)
        print("Backup matches the connected USB device and its checksum")
        return
    path = args.output
    manifest = Path(str(path) + ".json")
    if path.exists() or manifest.exists():
        raise ValueError("Refusing to overwrite a recovery image or its metadata")
    path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", args.port,
                    "--baud", "460800", "--after", "no_reset", "read_flash", "0", hex(SIZE), str(path)], check=True)
    if path.stat().st_size != SIZE:
        raise ValueError("Incomplete factory backup")
    meta = {"usb_serial": serial, "size": SIZE, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "created": datetime.datetime.now(datetime.timezone.utc).isoformat()}
    with manifest.open("x") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")
    verify_backup(path, serial)
    print(f"Recovery image: {path}; device/checksum metadata: {manifest}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, json.JSONDecodeError) as error:
        sys.exit(str(error))
