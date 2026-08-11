#!/system/bin/sh
# AudioRouter - Android Mixer / Speaker Routing Helper
# Run with root privileges ('su') in Termux or ADB shell.
# NOTE: audiorouter_client already applies this routing automatically at
#       startup (AndroidHelpers::apply_speaker_routing). This script is for
#       manual diagnostics or when the client's built-in routing is not enough.

echo "================================================="
echo " Android ALSA Speaker Routing & Mixer Config"
echo "================================================="

# Check root
if [ "$(id -u)" -ne 0 ]; then
    echo "Error: This script must be run as root (su)."
    exit 1
fi

echo "[1/3] Enabling permissions on /dev/snd/*..."
chmod 666 /dev/snd/* 2>/dev/null || true

echo "[2/3] Checking ALSA sound cards..."
cat /proc/asound/cards

echo "[3/3] Applying verified RX_MACRO speaker routing (Snapdragon 680)..."
# Same controls as AndroidHelpers::apply_speaker_routing() in src/client/android_helpers.cpp
tinymix "RX_MACRO RX2 MUX" "AIF2_PB" 2>/dev/null || true
tinymix "RX INT2_1 MIX1 INP0" "RX2" 2>/dev/null || true
tinymix "AUX_RDAC Switch" "1" 2>/dev/null || true

echo "Routing check completed. Launch audiorouter_client now."
