#!/usr/bin/env bash
# Runs INSIDE the Zephyr build container (invoked by docker-compose.yml).
# Kept as a script file rather than an inline compose command so the shell `$`
# variables aren't swallowed by docker-compose variable interpolation.
set -euo pipefail

cd /workspace

[ -d .west ] || west init -l probe
west update --narrow -o=--depth=1
west zephyr-export

# Zephyr 4.1.0 needs Zephyr SDK 0.17.x, but the zephyr-build image ships a newer
# incompatible SDK (1.0.x). Install 0.17.0 into the cached volume once
# (arch-matched) and point Zephyr at it — matching the pinned CI SDK.
ARCH="$(uname -m)"
SDK="zephyr-sdk-0.17.0"
if [ ! -d "/workspace/${SDK}" ]; then
	TARBALL="${SDK}_linux-${ARCH}_minimal.tar.xz"
	wget -nv "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.0/${TARBALL}"
	tar xJf "${TARBALL}"
	rm -f "${TARBALL}"
	"/workspace/${SDK}/setup.sh" -t arm-zephyr-eabi -h -c
fi
export ZEPHYR_SDK_INSTALL_DIR="/workspace/${SDK}"

west build -b xiao_ble probe -d build
mkdir -p probe/firmware
cp build/zephyr/zephyr.uf2 probe/firmware/zephyr.uf2
echo "==> probe/firmware/zephyr.uf2 ready"
