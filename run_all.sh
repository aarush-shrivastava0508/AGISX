#!/bin/bash
set -e

# Generate trajectory data if missing
if [ ! -f "data/trajectories/profile_alpha_ballistic.csv" ]; then
    echo "[SETUP] Generating missing trajectory datasets..."
    python3 data/generate_dataset.py
fi

# Build if needed
if [ ! -d "build_local" ]; then
    mkdir -p build_local
    cmake -B build_local -S .
fi
cmake --build build_local -j$(nproc)

echo "[AGISX] Starting nodes in reverse dependency order..."
trap 'echo ""; echo "[AGISX] Shutting down simulation nodes..."; kill $(jobs -p) 2>/dev/null; exit 0' SIGINT SIGTERM EXIT

# 1. Guidance (Stage 4)
./build_local/stage4_kinetic_ppn &
sleep 0.5

# 2. C2 WTA Brain (Stage 3)
./build_local/stage3_c2_wta_brain &
sleep 0.5

# 3. EKF Tracker (Stage 2)
./build_local/stage2_ekf_tracker &
sleep 0.5

# 4. Threat Generator (Stage 1 - foreground)
echo "[AGISX] All downstream listeners online. Launching Stage 1..."
./build_local/stage1_threat_gen
