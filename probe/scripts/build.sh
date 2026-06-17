#!/usr/bin/env bash
# One-command Docker build of the XIAO HOGP probe -> firmware/zephyr.uf2.
# No native Zephyr SDK required — only Docker. First run pulls the image and
# clones Zephyr (slow, cached in the `hogp-ws` volume); later runs are fast.
set -euo pipefail
cd "$(dirname "$0")/.."

docker compose run --rm build

echo
echo "→ firmware/zephyr.uf2"
ls -l firmware/zephyr.uf2 2>/dev/null || { echo "build did not produce a .uf2"; exit 1; }
