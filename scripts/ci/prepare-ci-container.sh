#!/usr/bin/env bash
# SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <app-path>" >&2
    exit 2
fi

app_path="$1"

# The host checkout and container-created west repositories have different
# owners, but this is an isolated CI container with a trusted workspace mount.
git config --global --add safe.directory '*'

if [[ -d .west ]]; then
    echo "Cleaning out old west manifest directories"
    while IFS= read -r dir; do
        if [[ "$dir" != "$app_path" ]]; then
            echo "Removing $dir"
            rm -rf "$dir"
        fi
    done < <(west list -f '{path}')
    rm -rf .west
fi

if [[ -d zephyr-sdk ]]; then
    echo "Cleaning out old zephyr-sdk directory"
    rm -rf zephyr-sdk
fi

find . -path '*/hal/stm32/zephyr/blobs' -prune -exec rm -rf {} +
find . -path '*/.git/shallow.lock' -exec rm -f {} +

export ZEPHYR_BASE="${GITHUB_WORKSPACE}/zephyr"
echo "ZEPHYR_BASE=$ZEPHYR_BASE" >> "$GITHUB_ENV"
echo "$HOME/.local/bin" >> "$GITHUB_PATH"
echo "$HOME/.cargo/bin" >> "$GITHUB_PATH"
export PATH="$HOME/.local/bin:$HOME/.cargo/bin:$PATH"

apt-get update
apt-get install -y ipmitool

rm -rf .west
west init -l "$app_path" || true
west config --global update.narrow true
west update --path-cache /tt-zephyr ||
    { rm -rf modules bootloader tools; west update --path-cache /tt-zephyr; }
west forall -c 'git reset --hard HEAD'

export ZEPHYR_SDK_INSTALL_DIR
ZEPHYR_SDK_INSTALL_DIR="$(find /opt/toolchains -name '*zephyr-sdk*' -print -quit)"
if [[ -z "$ZEPHYR_SDK_INSTALL_DIR" ]]; then
    echo "Unable to find the Zephyr SDK under /opt/toolchains." >&2
    exit 1
fi
echo "ZEPHYR_SDK_INSTALL_DIR=$ZEPHYR_SDK_INSTALL_DIR" >> "$GITHUB_ENV"

rm -rf "$HOME/.cmake/packages/Zephyr-sdk"
"$ZEPHYR_SDK_INSTALL_DIR/setup.sh" -c

pip uninstall -y tt-flash tt-tools-common pyluwen
west packages pip --install
west blobs fetch tt-system-firmware

echo "Zephyr SDK: $ZEPHYR_SDK_INSTALL_DIR"
echo "Python: $(command -v python3) $(python3 --version)"
echo "Pip: $(command -v pip3) $(pip3 --version)"
echo "West: $(command -v west) $(west --version)"
echo "CMake: $(command -v cmake) $(cmake --version | head -n 1)"
echo "Ninja: $(command -v ninja) $(ninja --version)"
echo "Rustc: $(command -v rustc) $(rustc --version)"
echo "Cargo: $(command -v cargo) $(cargo --version)"
echo "PIP packages:"
pip3 freeze
echo "tt-kmd version: $(cat /sys/module/tenstorrent/version)"
