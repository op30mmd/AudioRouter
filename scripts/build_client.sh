#!/bin/bash
# AudioRouter - Build Client (Linux / Android Termux)
set -e

echo "================================================="
echo " Building AudioRouter Client"
echo "================================================="

mkdir -p bin build
make client

echo ""
echo "================================================="
echo " Client built successfully: bin/audiorouter_client"
echo "================================================="
