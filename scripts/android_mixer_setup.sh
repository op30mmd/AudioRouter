#!/system/bin/sh
# AudioRouter - Android Mixer / Speaker Routing Helper
# Run with root privileges ('su') in Termux or ADB shell.

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

echo "[3/3] Attempting standard tinymix routing to speaker..."
# Qualcomm Snapdragon common speaker routing controls
tinymix "RX_CDC_DMA_RX_0 Audio Mixer MultiMedia1" 1 2>/dev/null || true
tinymix "PRI_MI2S_RX Audio Mixer MultiMedia1" 1 2>/dev/null || true
tinymix "SLIM_0_RX Audio Mixer MultiMedia1" 1 2>/dev/null || true
tinymix "TERT_MI2S_RX Audio Mixer MultiMedia1" 1 2>/dev/null || true
tinymix "Speaker Function" "On" 2>/dev/null || true
tinymix "Speaker Switch" 1 2>/dev/null || true
tinymix "SPK" 1 2>/dev/null || true
tinymix "RX1 Digital Volume" 84 2>/dev/null || true
tinymix "RX2 Digital Volume" 84 2>/dev/null || true

echo "Routing check completed. Launch audiorouter_client now."
