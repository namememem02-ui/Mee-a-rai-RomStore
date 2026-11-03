#!/bin/sh
# Fast Auto-Scan for Mee-a-rai ROM Store PC Server
CURL_BIN="./bin/curl"
[ ! -x "$CURL_BIN" ] && CURL_BIN="curl"

rm -f /tmp/found_server.txt

# 1. Determine Local Wi-Fi IP and Subnet (Busybox compatible)
MY_IP=$(ip -4 addr show wlan0 2>/dev/null | awk '/inet / {print $2}' | cut -d/ -f1)
if [ -z "$MY_IP" ]; then
    MY_IP=$(ifconfig wlan0 2>/dev/null | grep 'inet ' | sed 's/.*addr://;s/ .*//')
fi
if [ -z "$MY_IP" ]; then
    MY_IP=$(ip route get 8.8.8.8 2>/dev/null | awk '{print $7}')
fi

if [ -z "$MY_IP" ]; then
    echo "NO_WIFI"
    exit 1
fi

SUBNET=$(echo "$MY_IP" | cut -d '.' -f 1-3)
echo "Subnet detected: $SUBNET.*"

# Ping broadcast in background to populate ARP table
ping -c 2 -W 1 -b "$SUBNET.255" >/dev/null 2>&1 &

# 2. Extract active hosts from ARP table (/proc/net/arp)
ARP_HOSTS=""
if [ -f /proc/net/arp ]; then
    ARP_HOSTS=$(awk 'NR>1 && $1 ~ /^[0-9]/ {print $1}' /proc/net/arp 2>/dev/null | grep "^$SUBNET\." | cut -d '.' -f 4)
fi

# High-priority candidates: common PC DHCP hosts (including 240!), and typical ranges
PRIORITY="240 100 101 102 103 104 105 106 107 108 109 110 111 112 113 114 115 116 117 118 119 120 200 2 3 4 5 10 20 50 1"

# Combine candidates uniquely
ALL_CANDIDATES=""
for h in $ARP_HOSTS $PRIORITY; do
    case " $ALL_CANDIDATES " in
        *" $h "*) ;;
        *) ALL_CANDIDATES="$ALL_CANDIDATES $h" ;;
    esac
done

check_ip() {
    target="http://$SUBNET.$1:8080"
    if $CURL_BIN -s -m 0.8 "$target/api/info" 2>/dev/null | grep -q "server_ip"; then
        echo "$target" > /tmp/found_server.txt
        echo "FOUND: $target"
        return 0
    fi
    return 1
}

# Phase 1: Probe priority and ARP candidates in parallel (batches of 12)
BATCH=""
for h in $ALL_CANDIDATES; do
    check_ip "$h" &
    BATCH="$BATCH $!"
    if [ $(echo "$BATCH" | wc -w) -ge 12 ]; then
        wait $BATCH 2>/dev/null
        BATCH=""
        if [ -s /tmp/found_server.txt ]; then
            exit 0
        fi
    fi
done
[ -n "$BATCH" ] && wait $BATCH 2>/dev/null

if [ -s /tmp/found_server.txt ]; then
    exit 0
fi

# Phase 2: If still not found, scan wider range 1..254 in parallel batches of 25
for h in $(seq 1 254); do
    case " $ALL_CANDIDATES " in
        *" $h "*) continue ;;
    esac
    check_ip "$h" &
    BATCH="$BATCH $!"
    if [ $(echo "$BATCH" | wc -w) -ge 25 ]; then
        wait $BATCH 2>/dev/null
        BATCH=""
        if [ -s /tmp/found_server.txt ]; then
            exit 0
        fi
    fi
done
[ -n "$BATCH" ] && wait $BATCH 2>/dev/null

if [ -s /tmp/found_server.txt ]; then
    exit 0
fi

echo "NOT_FOUND"
exit 1
