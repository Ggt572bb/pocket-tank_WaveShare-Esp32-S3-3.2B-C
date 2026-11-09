#!/bin/bash
# Entry point: tools/flash.sh --lcd35b [--build-only | --first-install BACKUP]
# First install archives a verified full factory backup instead of asking
# the factory application for Pocket Tank's director/battery log.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
BUILD=${TANK_BUILD:-$ROOT/firmware/build-lcd35b}
PORT=${TANK_PORT:-}
: "${IDF_PATH:?Export ESP-IDF before running this script}"
BACKUP=
BUILD_ONLY=0
while (($#)); do
  case "$1" in
    --build-only) BUILD_ONLY=1; shift ;;
    --first-install) BACKUP=${2:?Supply the full 16 MB factory backup}; shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done
[[ -f firmware/keys/ota_signing_key.pem ]] || { echo "Missing OTA signing key" >&2; exit 1; }
(cd firmware && idf.py -B "$BUILD" -DSDKCONFIG="$BUILD/sdkconfig" \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.lcd35b' build)
grep -q '^CONFIG_POCKET_TANK_LCD35B=y$' "$BUILD/sdkconfig"
((BUILD_ONLY)) && exit 0
: "${PORT:?Set TANK_PORT to the target board USB serial path}"
[[ -e "$PORT" ]] || { echo "Board port not found: $PORT" >&2; exit 1; }
if [[ -n "$BACKUP" ]]; then
  # The first install changes the factory partition table: keep a recoverable
  # full image and its checksum. Never replace this record on an update.
  python tools/backup_lcd35b.py --port "$PORT" --verify "$BACKUP"
  python - "$BACKUP" <<'PYMETA'
from pathlib import Path
import datetime, json, sys
backup = Path(sys.argv[1]).resolve()
meta = json.loads(Path(str(backup) + ".json").read_text())
out = Path("docs/batlog") / ("lcd35b-local-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f") + ".json")
out.parent.mkdir(exist_ok=True)
with out.open("x") as f:
    json.dump({"backup": str(backup), **meta}, f, indent=2)
    f.write("\n")
print(out)
PYMETA
else
  TANK_PORT="$PORT" python tools/preflight.py
fi
if [[ -n "$BACKUP" ]]; then
  python tools/model_trailer.py --check --build-dir "$BUILD"
  python -m esptool --chip esp32s3 -p "$PORT" --after no_reset erase_region 0x9000 0x7000
  python -m esptool --chip esp32s3 -p "$PORT" -b 460800 --after no_reset \
    write_flash 0x290000 model/out/model_q4.bin 0xA8F000 "$BUILD/model_trailer.bin"
fi
# Native USB cannot leave a manually entered bootloader with RTS alone.
(cd "$BUILD" && python -m esptool --chip esp32s3 -p "$PORT" -b 460800 --after watchdog_reset write_flash @flash_args)
