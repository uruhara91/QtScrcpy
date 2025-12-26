#!/bin/bash

# ===== KONFIGURASI =====
# Sesuaikan nama package dan main activity kamu
AAUDIO_APK="app-release.apk"
PACKAGE="com.aaudio.forwarder"
MAIN_ACTIVITY=".MainActivity"

# Default Port
SNDCPY_PORT=28200

# Deteksi ADB
if command -v adb &> /dev/null; then
    ADB=adb
else
    ADB=/usr/bin/adb
fi

# Parse arguments (serial & port dari QtScrcpy)
serial=""
if [[ $# -ge 2 ]]; then
    serial="-s $1"
    SNDCPY_PORT=$2
fi

echo "🎵 AAudio Forwarder - Reverse Mode"

$ADB $serial wait-for-device

# 1. Install/Update APK (Skip if exist for speed, uncomment if needed)
# $ADB $serial install -r -g "$AAUDIO_APK" 

# 2. CRITICAL: ADB REVERSE
# Memetakan port HP 28200 -> PC 28200
# PC (QtScrcpy) sudah Listening. HP (App) akan Connect.
echo "🔄 Setting up reverse tunnel (HP:Connect -> PC:Listen)..."
$ADB $serial reverse tcp:$SNDCPY_PORT tcp:$SNDCPY_PORT || {
    echo "❌ Reverse tunnel failed!"
    # Fallback: remove reverse just in case
    $ADB $serial reverse --remove tcp:$SNDCPY_PORT
    exit 1
}

# 3. Force stop & Grant permissions
$ADB $serial shell am force-stop $PACKAGE 2>/dev/null
$ADB $serial shell appops set $PACKAGE PROJECT_MEDIA allow 2>/dev/null

# 4. Start activity
# Kita kirim Intent Extra "PORT" agar App tahu harus connect ke mana
echo "🚀 Launching app..."
$ADB $serial shell am start -n "$PACKAGE/$MAIN_ACTIVITY" \
    --ei "PORT" $SNDCPY_PORT > /dev/null 2>&1

echo "✅ App launched. Connection handled by QtScrcpy Server."
# Script ini boleh exit, tidak perlu loop polling karena 
# QtScrcpy yang akan handle koneksi putus/nyambung.
exit 0