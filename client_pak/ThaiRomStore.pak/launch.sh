#!/bin/sh

# Set working directory
PAK_DIR="$(dirname "$0")"
cd "$PAK_DIR" || exit 1

# Start logging everything to launch.log on SD card
exec > "$PAK_DIR/launch.log" 2>&1
echo "=== Starting Thai ROM Store (Interactive UI) at $(date) ==="

SDCARD_PATH="/mnt/SDCARD"
if [ ! -d "$SDCARD_PATH" ] && [ -d "/mnt/sdcard" ]; then
    SDCARD_PATH="/mnt/sdcard"
fi
echo "SDCARD_PATH: $SDCARD_PATH"

# Ensure execute permissions on bundled binaries
chmod +x "$PAK_DIR/bin/curl" "$PAK_DIR/bin/store.elf" "$PAK_DIR/bin/show2.elf" "$PAK_DIR/bin/autoscan.sh" 2>/dev/null

export LD_LIBRARY_PATH=/usr/trimui/lib:$PAK_DIR/lib:$LD_LIBRARY_PATH
export PATH=$PAK_DIR/bin:/usr/trimui/bin:$SDCARD_PATH/.system/tg5040/bin:$SDCARD_PATH/.system/h700/bin:$PATH

SHOW_BIN="$PAK_DIR/bin/show2.elf"
if [ ! -x "$SHOW_BIN" ]; then
    if [ -x "/usr/trimui/bin/show2.elf" ]; then
        SHOW_BIN="/usr/trimui/bin/show2.elf"
    elif [ -x "$SDCARD_PATH/.system/tg5040/bin/show2.elf" ]; then
        SHOW_BIN="$SDCARD_PATH/.system/tg5040/bin/show2.elf"
    fi
fi
ICON_IMG="$PAK_DIR/icon.png"

show_msg() {
    TEXT="$1"
    TIME="$2"
    [ -z "$TIME" ] && TIME=2
    echo "[MSG] $TEXT ($TIME sec)"
    if [ -x "$SHOW_BIN" ] && [ -f "$ICON_IMG" ]; then
        "$SHOW_BIN" --mode=simple --image="$ICON_IMG" --text="$TEXT" --fontsize=20 --logoheight=160 --timeout="$TIME"
    else
        echo "$TEXT"
        sleep "$TIME"
    fi
}

# 1. Read Configuration
CONFIG_FILE="$PAK_DIR/config.txt"
MODE="AUTO"
SERVER_URL="http://192.168.1.118:8080"
ONLINE_URL=""

if [ -f "$CONFIG_FILE" ]; then
    CFG_MODE=$(grep '^MODE=' "$CONFIG_FILE" | cut -d '=' -f 2- | tr -d '\r\n ')
    CFG_LOCAL=$(grep '^LOCAL_URL=' "$CONFIG_FILE" | cut -d '=' -f 2- | tr -d '\r\n ')
    CFG_ONLINE=$(grep '^ONLINE_URL=' "$CONFIG_FILE" | cut -d '=' -f 2- | tr -d '\r\n ')
    CFG_OLD_SERVER=$(grep '^SERVER_URL=' "$CONFIG_FILE" | cut -d '=' -f 2- | tr -d '\r\n ')

    [ -n "$CFG_MODE" ] && MODE="$CFG_MODE"
    [ -n "$CFG_LOCAL" ] && SERVER_URL="$CFG_LOCAL"
    [ -z "$CFG_LOCAL" ] && [ -n "$CFG_OLD_SERVER" ] && SERVER_URL="$CFG_OLD_SERVER"
    [ -n "$CFG_ONLINE" ] && ONLINE_URL="$CFG_ONLINE"
fi

echo "Active Mode: $MODE"
echo "Config Local URL: $SERVER_URL"
echo "Config Online URL: $ONLINE_URL"

# Helper: Auto-scan subnet for PC serve
auto_scan_pc() {
    echo "Starting Wi-Fi Auto-Discovery for Mirai ROM Hub..."
    show_msg "IP changed! Auto-scanning Wi-Fi for PC..." 2

    MY_IP=$(ip -4 addr show wlan0 2>/dev/null | grep -oP '(?<=inet\s)\d+(\.\d+){3}' || ifconfig wlan0 2>/dev/null | awk '/inet / {print $2}')
    if [ -z "$MY_IP" ]; then
        MY_IP=$(ip route get 8.8.8.8 2>/dev/null | awk '{print $7}')
    fi
    echo "Device IP: $MY_IP"

    if [ -z "$MY_IP" ]; then
        echo "Could not determine local IP. Is Wi-Fi connected?"
        return 1
    fi

    SUBNET=$(echo "$MY_IP" | cut -d '.' -f 1-3)
    echo "Subnet: $SUBNET.*"

    CANDIDATES="118 200 100 101 102 103 104 105 106 107 108 109 110 111 112 113 114 115 116 117 119 120 2 3 4 5 10 20 50 1"
    
    for h in $CANDIDATES; do
        TEST_URL="http://$SUBNET.$h:8080"
        echo "Probing: $TEST_URL..."
        if curl -s -m 0.35 "$TEST_URL/api/info" 2>/dev/null | grep -q "server_ip"; then
            echo "SUCCESS: Found Mirai ROM Hub at $TEST_URL!"
            SERVER_URL="$TEST_URL"
            if [ -f "$CONFIG_FILE" ]; then
                sed -i "s|^LOCAL_URL=.*|LOCAL_URL=$TEST_URL|" "$CONFIG_FILE"
                sed -i "s|^SERVER_URL=.*|SERVER_URL=$TEST_URL|" "$CONFIG_FILE"
            fi
            show_msg "Found PC at $SUBNET.$h! Auto-Saved." 2
            return 0
        fi
    done

    echo "Auto-scan finished: PC not found in priority range."
    return 1
}

# 2. Connection Strategy
CONNECTED=0
ACTIVE_URL=""

if [ "$MODE" = "ONLINE" ] && [ -n "$ONLINE_URL" ]; then
    echo "Mode is ONLINE, testing $ONLINE_URL..."
    show_msg "Connecting to Cloud: $ONLINE_URL..." 2
    if curl -s -m 5 "$ONLINE_URL/api/info" 2>/dev/null | grep -q "supported_systems"; then
        CONNECTED=1
        ACTIVE_URL="$ONLINE_URL"
    else
        show_msg "Online URL unreachable! Checking Local..." 2
    fi
fi

if [ $CONNECTED -eq 0 ]; then
    echo "Testing Local URL: $SERVER_URL..."
    show_msg "Connecting to Store: $SERVER_URL..." 1
    if curl -s -m 2.5 "$SERVER_URL/api/info" 2>/dev/null | grep -q "supported_systems"; then
        CONNECTED=1
        ACTIVE_URL="$SERVER_URL"
    else
        echo "Local URL failed! Triggering Auto-Scan..."
        if auto_scan_pc; then
            CONNECTED=1
            ACTIVE_URL="$SERVER_URL"
        fi
    fi
fi

if [ $CONNECTED -eq 0 ] && [ -n "$ONLINE_URL" ]; then
    echo "Local failed. Trying fallback Online URL: $ONLINE_URL..."
    show_msg "Connecting to Online Fallback..." 2
    if curl -s -m 5 "$ONLINE_URL/api/info" 2>/dev/null | grep -q "supported_systems"; then
        CONNECTED=1
        ACTIVE_URL="$ONLINE_URL"
    fi
fi

CATALOG_FILE="/tmp/catalog.txt"

if [ $CONNECTED -eq 0 ]; then
    echo "Notice: Could not connect at startup. Entering Store in Offline/Settings mode..."
    ACTIVE_URL="$SERVER_URL"
    [ ! -f "$CATALOG_FILE" ] && touch "$CATALOG_FILE"
else
    echo "Connected successfully to: $ACTIVE_URL"
    # 3. Fetch Catalog before launching interactive UI
    rm -f "$CATALOG_FILE"

    curl -L -s --connect-timeout 5 "$ACTIVE_URL/api/catalog/sync" -o "$CATALOG_FILE"
    TOTAL_GAMES=$(wc -l < "$CATALOG_FILE" 2>/dev/null || echo 0)
    echo "Fetched $TOTAL_GAMES games from server into $CATALOG_FILE"
fi

# 4. Launch Native Interactive Store UI with OTA Auto-Update
while true; do
    echo "Launching Interactive Store UI..."
    "$PAK_DIR/bin/store.elf" "$ACTIVE_URL"
    APP_EXIT=$?
    echo "store.elf exited with code: $APP_EXIT"

    # Exit code 42 indicates an in-app OTA update was downloaded and requested restart
    if [ "$APP_EXIT" -eq 42 ]; then
        echo "=== Installing OTA Update Package ==="
        if [ -f "/tmp/store.elf.update" ] && [ -s "/tmp/store.elf.update" ]; then
            echo "Applying updated store.elf..."
            rm -f "$PAK_DIR/bin/store.elf"
            cp -f "/tmp/store.elf.update" "$PAK_DIR/bin/store.elf"
            chmod +x "$PAK_DIR/bin/store.elf"
            rm -f "/tmp/store.elf.update"
        fi

        if [ -f "/tmp/launch.sh.update" ] && [ -s "/tmp/launch.sh.update" ]; then
            echo "Applying updated launch.sh..."
            cp -f "/tmp/launch.sh.update" "$PAK_DIR/launch.sh"
            chmod +x "$PAK_DIR/launch.sh"
            rm -f "/tmp/launch.sh.update"
        fi

        if [ -f "/tmp/autoscan.sh.update" ] && [ -s "/tmp/autoscan.sh.update" ]; then
            echo "Applying updated autoscan.sh..."
            cp -f "/tmp/autoscan.sh.update" "$PAK_DIR/bin/autoscan.sh"
            chmod +x "$PAK_DIR/bin/autoscan.sh"
            rm -f "/tmp/autoscan.sh.update"
        fi

        sync
        echo "OTA Update installed successfully! Restarting store.elf..."
        sleep 1
        continue
    fi

    # Normal user exit
    break
done

# Sync any written files to SD card
sync
echo "Exited Interactive Store cleanly. Returning to NextUI."
exit 0
