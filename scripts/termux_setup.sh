#!/data/data/com.termux/files/usr/bin/bash
# AudioRouter - Termux Android Client Setup Script
set -e

echo "================================================="
echo " Setting up AudioRouter in Termux (Android ALSA)"
echo "================================================="

# Update package lists
echo "[1/4] Updating Termux package lists..."
pkg update -y || true

# Install required build packages
echo "[2/4] Installing compiler and ALSA tools..."
pkg install -y clang make alsa-lib alsa-utils tsu || true

# Build AudioRouter Client
echo "[3/4] Compiling AudioRouter Client with clang++..."
CXX=clang++ make client

echo "[4/4] Setup complete!"
echo ""
echo "To run AudioRouter Client with root privileges, execute:"
echo "  su"
echo "  ./bin/audiorouter_client -s <PC_IP_ADDRESS> -p 44100"
echo ""
echo "Or use the automated helper:"
echo "  ./scripts/termux_run.sh <PC_IP_ADDRESS>"
echo "================================================="
