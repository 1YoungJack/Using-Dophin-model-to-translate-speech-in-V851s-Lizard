#!/bin/bash

date -s "2025-10-01 12:00:00"

ifconfig wlan0 down
killall wpa_supplicant 2>/dev/null
rm -f /var/run/wpa_supplicant/wlan0

ifconfig wlan0 up
mkdir -p /var/run/wpa_supplicant
wpa_supplicant -B -c /etc/wpa_supplicant.conf -i wlan0
udhcpc -i wlan0