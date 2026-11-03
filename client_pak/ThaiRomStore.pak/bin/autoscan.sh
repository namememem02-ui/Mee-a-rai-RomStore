#!/bin/sh
# Auto-scan local subnet for Mirai ROM Hub serve
CURL_BIN="./bin/curl"
[ ! -x "$CURL_BIN" ] && CURL_BIN="curl"

MY_IP=$(ip -4 addr show wlan0 2>/dev/null | grep -oP '(?<=inet\s)\d+(\.\d+){3}' || ifconfig wlan0 2>/dev/null | awk '/inet / {print $2}')
if [ -z "$MY_IP" ]; then
    MY_IP=$(ip route get 8.8.8.8 2>/dev/null | awk '{print $7}')
fi

if [ -z "$MY_IP" ]; then
    echo "NO_WIFI"
    exit 1
fi

SUBNET=$(echo "$MY_IP" | cut -d '.' -f 1-3)
CANDIDATES="118 200 100 101 102 103 104 105 106 107 108 109 110 111 112 113 114 115 116 117 119 120 2 3 4 5 10 20 50 1"

for h in $CANDIDATES; do
    TEST_URL="http://$SUBNET.$h:8080"
    if $CURL_BIN -s -m 0.35 "$TEST_URL/api/info" 2>/dev/null | grep -q "server_ip"; then
        echo "$TEST_URL" > /tmp/found_server.txt
        echo "FOUND: $TEST_URL"
        exit 0
    fi
done

echo "NOT_FOUND"
exit 1
