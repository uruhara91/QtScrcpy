#!/bin/bash

# --- CONFIGURATION ---
# Pastikan nama file APK di folder PC sesuai dengan ini
APK="sndcpy.apk" 
PACKAGE="com.aaudio.forwarder"
MAIN_ACTIVITY=".MainActivity"
# Default port jika tidak dipassing argumen
SNDCPY_PORT=28200
# ---------------------

# Deteksi ADB
if command -v adb &> /dev/null; then
    ADB=adb
else
    ADB=/usr/bin/adb
fi

# Parse arguments (Serial & Port yang dikirim oleh QtScrcpy)
serial=""
if [[ $# -ge 2 ]]; then
    serial="-s $1"
    SNDCPY_PORT=$2
fi

echo "🎵 AAudio Forwarder Launcher"

# Tunggu device connect
$ADB $serial wait-for-device

# ==========================================
# 1. INSTALLATION CHECK
# ==========================================
# Cek apakah app sudah terinstall dengan versi yang benar?
# Untuk simplicity di QtScrcpy, kita asumsikan jika file APK ada, kita coba update/install.
if [[ -f "$APK" ]]; then
    echo "📦 Found APK locally. Installing/Updating..."
    # -t: allow test packages, -r: replace existing, -g: grant runtime permissions
    $ADB $serial install -t -r -g "$APK" > /dev/null 2>&1
else
    echo "ℹ️  APK file not found locally. Assuming app is installed on device."
fi

# ==========================================
# 2. PERMISSION INJECTION (THE MAGIC)
# ==========================================
echo "🔑 Granting Permissions..."

# A. Record Audio (Standar)
$ADB $serial shell pm grant $PACKAGE android.permission.RECORD_AUDIO > /dev/null 2>&1

# B. Notification (Wajib untuk Android 13+ foreground service)
$ADB $serial shell pm grant $PACKAGE android.permission.POST_NOTIFICATIONS > /dev/null 2>&1

# C. BYPASS "START CASTING" POPUP (AppOps)
# Ini kuncinya agar tidak perlu klik "Start Now" di HP setiap kali connect.
# Note: Tidak semua ROM Android 11+ patuh pada ini, tapi di AOSP/Pixel/Samsung ini bekerja.
$ADB $serial shell appops set $PACKAGE PROJECT_MEDIA allow > /dev/null 2>&1

# ==========================================
# 3. NETWORK SETUP
# ==========================================
# C++ code saya sebelumnya sudah melakukan ini, tapi tidak ada salahnya double-check
# di script agar script ini bisa dipakai standalone (tanpa QtScrcpy) jika perlu.
echo "🔄 Setting up ADB Reverse..."
$ADB $serial reverse tcp:$SNDCPY_PORT tcp:$SNDCPY_PORT

# ==========================================
# 4. LAUNCH SERVICE
# ==========================================
echo "🚀 Launching Audio Service..."

# Stop dulu biar fresh
$ADB $serial shell am force-stop $PACKAGE > /dev/null 2>&1

# Start Activity dengan parameter PORT
# Activity akan menyalakan Service, lalu Activity-nya close sendiri (finish).
$ADB $serial shell am start -n "$PACKAGE/$MAIN_ACTIVITY" \
    --ei "PORT" $SNDCPY_PORT > /dev/null 2>&1

# Cek status
sleep 1
if $ADB $serial shell pidof $PACKAGE > /dev/null; then
    echo "✅ Audio Service Running on Port $SNDCPY_PORT"
else
    echo "⚠️  Service failed to start. Check Logcat."
    exit 1
fi

# Script selesai, QtScrcpy akan lanjut handshake via TCP.
exit 0