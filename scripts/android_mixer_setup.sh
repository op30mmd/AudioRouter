#!/system/bin/sh
# AudioRouter - Android ALSA Mixer / Speaker Routing Helper (Professional)
# Run with root privileges in Termux (su) or ADB root shell.
# Detects SoC family and attempts common speaker path routing via tinymix.

set -e

SHOW_HELP=0
LIST_ONLY=0
DRY_RUN=0
FORCE_SOC=""

for arg in "$@"; do
    case "$arg" in
        -h|--help) SHOW_HELP=1 ;;
        --list) LIST_ONLY=1 ;;
        --dry-run) DRY_RUN=1 ;;
        --qualcomm) FORCE_SOC="qualcomm" ;;
        --mediatek) FORCE_SOC="mediatek" ;;
    esac
done

if [ "$SHOW_HELP" = "1" ]; then
    cat <<EOF
AudioRouter Android Mixer Setup

Usage: $0 [options]

Options:
  -h, --help     Show this help
  --list         Only list sound cards and mixer controls, do not modify
  --dry-run      Show what would be done, without executing tinymix
  --qualcomm     Force Qualcomm routing path
  --mediatek     Force MediaTek routing path

What this script does:
  1. Checks root (uid 0 required)
  2. Fixes /dev/snd/* permissions to 666
  3. Lists /proc/asound/cards and available tinymix controls
  4. Attempts speaker routing via common tinymix controls:
     - Qualcomm: RX_CDC_DMA_RX_0, PRI_MI2S_RX, SLIM_0_RX, Speaker Function
     - MediaTek: Speaker, Speaker Switch, SPK, RX Digital Volume
  5. Reports result and suggests next steps

Requirements:
  - Root privileges (su)
  - tinymix from tinyalsa (pkg install alsa-utils or vendor binary)
  - Termux or ADB shell

After running, launch:
  ./bin/audiorouter_client -s <PC_IP> -p 44100 -d direct:/dev/snd/pcmC0D0p
EOF
    exit 0
fi

echo "============================================================"
echo " AudioRouter - Android ALSA Speaker Routing Setup"
echo "============================================================"

# Check root
if [ "$(id -u)" -ne 0 ]; then
    echo "[ERROR] This script must be run as root (su)."
    echo "        Example: su -c ./scripts/android_mixer_setup.sh"
    echo "        Or: su, then ./scripts/android_mixer_setup.sh"
    exit 1
fi

echo "[INFO] Running as uid=$(id -u) - root OK"

# Check tinymix
if ! command -v tinymix >/dev/null 2>&1; then
    echo "[WARN] tinymix not found in PATH."
    echo "       In Termux: pkg install alsa-utils"
    echo "       On device: tinymix may be in /vendor/bin or /system/bin"
    echo "       Trying common paths..."
    for p in /vendor/bin/tinymix /system/bin/tinymix /data/data/com.termux/files/usr/bin/tinymix; do
        if [ -x "$p" ]; then
            echo "[INFO] Found tinymix at $p"
            alias tinymix="$p" 2>/dev/null || true
            # Create wrapper
            tinymix() { "$p" "$@"; }
            break
        fi
    done
    if ! command -v tinymix >/dev/null 2>&1 && [ ! -n "$(type tinymix 2>/dev/null)" ]; then
        echo "[ERROR] tinymix still not found, listing cards only. Install alsa-utils."
        LIST_ONLY=1
    fi
fi

echo ""
echo "[1/4] Fixing permissions on /dev/snd/* and /dev/snd/pcm*..."
if [ "$DRY_RUN" = "1" ]; then
    echo "      [DRY-RUN] Would run: chmod 666 /dev/snd/*"
else
    chmod 666 /dev/snd/* 2>/dev/null || echo "[WARN] chmod 666 /dev/snd/* failed (some nodes may not exist)"
    ls -l /dev/snd/ 2>/dev/null || echo "[WARN] Cannot list /dev/snd/"
fi

echo ""
echo "[2/4] Checking ALSA sound cards (/proc/asound/cards)..."
if [ -f /proc/asound/cards ]; then
    cat /proc/asound/cards
else
    echo "[WARN] /proc/asound/cards not found, kernel may not have ALSA proc"
fi

echo ""
echo "[3/4] Detecting SoC family..."
SOC="unknown"
if [ -n "$FORCE_SOC" ]; then
    SOC="$FORCE_SOC"
    echo "[INFO] Forced SoC family: $SOC"
else
    if grep -qi "qualcomm\|qcom\|sm[0-9]" /proc/cpuinfo 2>/dev/null; then
        SOC="qualcomm"
    elif grep -qi "mediatek\|mt[0-9]" /proc/cpuinfo 2>/dev/null; then
        SOC="mediatek"
    elif [ -f /proc/asound/cards ] && grep -qi "qualcomm" /proc/asound/cards; then
        SOC="qualcomm"
    fi
    echo "[INFO] Detected SoC family: $SOC"
fi

echo ""
if [ "$LIST_ONLY" = "1" ]; then
    echo "[3b] Listing mixer controls (tinymix without args)..."
    tinymix 2>&1 | head -n 200 || echo "[WARN] tinymix listing failed"
    echo ""
    echo "[4/4] List-only mode, skipping routing. To apply routing, run without --list"
    exit 0
fi

echo "[4/4] Attempting speaker routing..."

# Helper function
try_tinymix() {
    desc="$1"
    shift
    if [ "$DRY_RUN" = "1" ]; then
        echo "      [DRY-RUN] tinymix $*   # $desc"
        return 0
    fi
    echo "      Trying: tinymix $*   # $desc"
    if tinymix "$@" 2>/dev/null; then
        echo "        -> OK"
    else
        echo "        -> not applicable / failed (ignored)"
    fi
}

# Common controls (many Qualcomm and MediaTek devices share these)
echo "  Applying common speaker routing controls..."

try_tinymix "Qualcomm RX CDC DMA" "RX_CDC_DMA_RX_0 Audio Mixer MultiMedia1" 1
try_tinymix "Qualcomm PRI MI2S RX" "PRI_MI2S_RX Audio Mixer MultiMedia1" 1
try_tinymix "Qualcomm SLIM RX" "SLIM_0_RX Audio Mixer MultiMedia1" 1
try_tinymix "Qualcomm TERT MI2S RX" "TERT_MI2S_RX Audio Mixer MultiMedia1" 1
try_tinymix "Qualcomm QUAT MI2S RX" "QUAT_MI2S_RX Audio Mixer MultiMedia1" 1
try_tinymix "Speaker Function On" "Speaker Function" "On"
try_tinymix "Speaker Switch" "Speaker Switch" 1
try_tinymix "SPK Switch" "SPK" 1
try_tinymix "SpkrLeft Switch" "SpkrLeft Switch" 1
try_tinymix "SpkrRight Switch" "SpkrRight Switch" 1
try_tinymix "Ext Speaker Switch" "Ext Spk Switch" 1

# Volume controls
try_tinymix "RX1 Digital Volume" "RX1 Digital Volume" 84
try_tinymix "RX2 Digital Volume" "RX2 Digital Volume" 84
try_tinymix "RX3 Digital Volume" "RX3 Digital Volume" 84
try_tinymix "RX7 Digital Volume" "RX7 Digital Volume" 84
try_tinymix "Speaker Volume" "Speaker Volume" 100
try_tinymix "Speaker Digital Volume" "Speaker Digital Volume" 100

if [ "$SOC" = "qualcomm" ]; then
    echo "  Applying Qualcomm-specific additional controls..."
    try_tinymix "QUAT_MI2S_RX Port Mixer" "QUAT_MI2S_RX Port Mixer CODEC_DMA_LPAIF_RXTX" 1
    try_tinymix "PRI_MI2S_RX Port Mixer" "PRI_MI2S_RX Port Mixer CODEC_DMA_LPAIF_RXTX" 1
    try_tinymix "SEC_MI2S_RX Audio Mixer" "SEC_MI2S_RX Audio Mixer MultiMedia1" 1
    try_tinymix "TERT_MI2S_RX Port Mixer" "TERT_MI2S_RX Port Mixer QUAT_MI2S_TX" 1
elif [ "$SOC" = "mediatek" ]; then
    echo "  Applying MediaTek-specific controls..."
    try_tinymix "MTK Speaker Switch" "Speaker_Amp_Switch" 1
    try_tinymix "MTK Ext Spk Amp Switch" "Ext_Speaker_Amp_Switch" 1
fi

echo ""
echo "============================================================"
echo " Routing setup completed"
echo "============================================================"
echo " Next steps:"
echo "  1. Keep this root shell or run: su"
echo "  2. ./bin/audiorouter_client --list-devices"
echo "  3. ./bin/audiorouter_client -s <PC_IP> -p 44100 -d direct:/dev/snd/pcmC0D0p"
echo ""
echo " If still no sound:"
echo "  - Try alternative devices: hw:0,0 , agm , direct:/dev/snd/pcmC0D1p"
echo "  - Run diagnostic: ./scripts/android_diagnose.sh"
echo "  - Check if another app holds the PCM device"
echo "============================================================"
