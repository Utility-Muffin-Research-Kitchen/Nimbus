#!/bin/sh
set -eu
PAK_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

# Source the Leaf platform env (paths, runtime dirs) when present.
PLATFORM="${PLATFORM:-mlp1}"
for root in "${SDCARD_PATH:-/mnt/sdcard}" /mnt/sdcard /media/sdcard1; do
  env_sh="$root/.system/leaf/platforms/$PLATFORM/launcher/env.sh"
  if [ -f "$env_sh" ]; then . "$env_sh"; break; fi
done

SD="${SDCARD_PATH:-/mnt/sdcard}"
# Config + cache + logs live in the durable user-data tree, never the pak dir.
DATA="${USERDATA_PATH:-$SD/.userdata/$PLATFORM}/nimbus"
mkdir -p "$DATA/cache" 2>/dev/null || { DATA=/tmp/nimbus; mkdir -p "$DATA/cache"; }

export NIMBUS_CONFIG_DIR="$DATA"
export NIMBUS_CACHE_DIR="$DATA/cache"
export NIMBUS_PAK_DIR="$PAK_DIR"

# HTTPS needs a CA bundle (the stock device libcurl has no default CA path).
CA="$SD/.system/leaf/platforms/$PLATFORM/launcher/res/certs/cacert.pem"
[ -f "$CA" ] && export CURL_CA_BUNDLE="$CA"

LOG_DIR="${LOGS_PATH:-$DATA/logs}"
mkdir -p "$LOG_DIR" 2>/dev/null || LOG_DIR=/tmp

cd "$PAK_DIR"
exec "$PAK_DIR/bin/nimbus" 2>"$LOG_DIR/nimbus.log"
