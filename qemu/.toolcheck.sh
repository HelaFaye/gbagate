#!/usr/bin/env bash
# throwaway toolchain probe
for t in qemu-system-xtensa mgba mgba-qt mgba-headless arm-none-eabi-gcc \
         idf.py socat python3 pio; do
    p="$(command -v "$t" 2>/dev/null || true)"
    printf '%-22s %s\n' "$t" "${p:-MISSING}"
done
echo "IDF_PATH=${IDF_PATH:-unset}"
echo "--- /opt ---";           ls /opt 2>/dev/null || true
echo "--- ~/.espressif ---";   ls "$HOME/.espressif" 2>/dev/null | head || true
echo "--- devkitpro ---";      ls /opt/devkitpro 2>/dev/null || true
echo "--- apt gba/arm ---";    dpkg -l 2>/dev/null | grep -iE 'arm-none-eabi|devkit|mgba' || echo "(none packaged)"
