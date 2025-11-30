#!/bin/sh
# shaper.sh - simple HTB shaper for Wi-Fi using MCS + bandwidth
#
# Usage:
#   ./shaper.sh <iface> <mcs0-7> <bw_mhz>
#   ./shaper.sh <iface> clear
#   ./shaper.sh <iface> show
#
# Examples:
#   ./shaper.sh wlx40a5ef2f229b 2 20     # apply MCS2 @ 20 MHz
#   ./shaper.sh wlx40a5ef2f229b clear    # remove qdisc
#   ./shaper.sh wlx40a5ef2f229b show     # show status
#
# Assumptions:
# - 802.11n HT, 1 spatial stream, long GI
# - HT20 theoretical PHY rates (kbit/s):
#     MCS0..7 = 6500, 13000, 19500, 26000, 39000, 52000, 58500, 65000
# - We shape to 65% of that, scaled by (bw_mhz / 20)
# - Class layout:
#     - root: total shaped rate (1:99)
#     - 1:1   video      (fwmarks 1, 2)
#     - 1:10  telemetry  (fwmarks 10, 11)
#     - 1:20  tunnel     (fwmarks 20, 21)
#     - 1:100 default/unclassified

CONFIG_FILE="/etc/wfb.conf"

MCS_DEFAULT="2"
BW_DEFAULT="20"

# Load simple VAR=VALUE overrides from /etc/wfb.conf when present

IFACE_RAW="$1"
CMD="$2"
BW_MHZ="$3"

CONFIG_MCS=""
CONFIG_BW_MHZ=""

if [ -r "$CONFIG_FILE" ]; then
    while IFS= read -r line; do
        case "$line" in
            [A-Za-z_]*=*)
                key=${line%%=*}
                value=${line#*=}
                case "$key" in
                    MCS|mcs)
                        CONFIG_MCS="$value"
                        ;;
                    WFB_BW_MHZ|wfb_bw_mhz|BW_MHZ|bw_mhz)
                        CONFIG_BW_MHZ="$value"
                        ;;
                esac
                ;;
        esac
    done < "$CONFIG_FILE"
fi

CMD="${CMD:-${CONFIG_MCS:-$MCS_DEFAULT}}"
BW_MHZ="${BW_MHZ:-${CONFIG_BW_MHZ:-$BW_DEFAULT}}"

usage() {
    echo "Usage:"
    echo "  $0 <iface> <mcs0-7> <bw_mhz>"
    echo "  $0 <iface> clear"
    echo "  $0 <iface> show"
    exit 1
}

if [ -z "$IFACE_RAW" ]; then
    usage
fi

IFACE_LIST="$IFACE_RAW"

# SHOW mode
if [ "$CMD" = "show" ]; then
    for IFACE in $IFACE_LIST; do
        echo "=== qdisc on $IFACE ==="
        tc -s qdisc show dev "$IFACE"
        echo
        echo "=== classes on $IFACE ==="
        tc -s class show dev "$IFACE"
        echo
        echo "=== filters on $IFACE (parent 1:) ==="
        tc filter show dev "$IFACE" parent 1:
        echo
    done
    exit 0
fi

# CLEAR mode
if [ "$CMD" = "clear" ]; then
    for IFACE in $IFACE_LIST; do
        echo "Clearing qdisc on $IFACE"
        tc qdisc del dev "$IFACE" root 2>/dev/null
    done
    exit 0
fi

# From here, CMD is assumed to be an MCS, and BW_MHZ must be provided
MCS="$CMD"

if [ -z "$MCS" ] || [ -z "$BW_MHZ" ]; then
    usage
fi

case "$MCS" in
    0) BASE_KBIT=6500 ;;
    1) BASE_KBIT=13000 ;;
    2) BASE_KBIT=19500 ;;
    3) BASE_KBIT=26000 ;;
    4) BASE_KBIT=39000 ;;
    5) BASE_KBIT=52000 ;;
    6) BASE_KBIT=58500 ;;
    7) BASE_KBIT=65000 ;;
    *)
        echo "Unsupported MCS '$MCS' (must be 0..7, 'clear' or 'show')" >&2
        exit 1
        ;;
esac

# Basic sanity on BW_MHZ: must be integer
case "$BW_MHZ" in
    *[!0-9]*|'')
        echo "Bandwidth must be an integer MHz (e.g. 20, 40)" >&2
        exit 1
        ;;
esac

# Theoretical PHY rate scaled by bandwidth:
#   theo_kbit = BASE_KBIT * BW_MHZ / 20
THEO_KBIT=$(( BASE_KBIT * BW_MHZ / 20 ))

# Use 65% of that as shaping rate
ROOT_KBIT=$(( THEO_KBIT * 65 / 100 ))

# Avoid silly-low values
if [ "$ROOT_KBIT" -lt 1000 ]; then
    ROOT_KBIT=1000
fi

# Split between classes:
# video ~80%, telemetry ~10%, tunnel ~10% of ROOT_KBIT
VIDEO_KBIT=$(( ROOT_KBIT * 80 / 100 ))
TEL_KBIT=$(( ROOT_KBIT * 10 / 100 ))
TUN_KBIT=$(( ROOT_KBIT * 10 / 100 ))

# Ensure some minimums
[ "$VIDEO_KBIT" -lt 1000 ] && VIDEO_KBIT=1000
[ "$TEL_KBIT"   -lt 128 ]  && TEL_KBIT=128
[ "$TUN_KBIT"   -lt 128 ]  && TUN_KBIT=128

for IFACE in $IFACE_LIST; do
    echo "Configuring qdisc on $IFACE:"
    echo "  MCS:          $MCS"
    echo "  Bandwidth:    ${BW_MHZ} MHz"
    echo "  Theoretical:  ${THEO_KBIT} kbit/s (PHY)"
    echo "  Shaping @65%: ${ROOT_KBIT} kbit/s"
    echo "    video:      ${VIDEO_KBIT} kbit/s    fwmarks: 1, 2"
    echo "    telemetry:  ${TEL_KBIT} kbit/s     fwmarks: 10, 11"
    echo "    tunnel:     ${TUN_KBIT} kbit/s     fwmarks: 20, 21"

    # Cleanup old root qdisc
    tc qdisc del dev "$IFACE" root 2>/dev/null

    # Root qdisc + root class (1:99 is the shaped bucket)
    tc qdisc add dev "$IFACE" handle 1: root htb default 100
    tc class add dev "$IFACE" parent 1: classid 1:99 htb rate "${ROOT_KBIT}kbit" ceil "${ROOT_KBIT}kbit"

    # Default/unclassified traffic: very low priority
    tc class add dev "$IFACE" parent 1:99 classid 1:100 htb rate 1kbit ceil "${ROOT_KBIT}kbit" prio 100 quantum 1000
    tc qdisc add dev "$IFACE" handle 100: parent 1:100 pfifo

    ########################################
    # Filters using u32 on fwmark instead of 'fw' classifier
    ########################################

    # Video class (fwmarks 1 and 2)
    tc class add dev "$IFACE" parent 1:99 classid 1:1 htb rate "${VIDEO_KBIT}kbit" ceil "${ROOT_KBIT}kbit" prio 2
    tc filter add dev "$IFACE" parent 1: protocol ip prio 1 u32 \
        match mark 1 0xffffffff flowid 1:1
    tc filter add dev "$IFACE" parent 1: protocol ip prio 1 u32 \
        match mark 2 0xffffffff flowid 1:1
    tc qdisc add dev "$IFACE" handle 101: parent 1:1 pfifo

    # Telemetry class (fwmarks 10, 11)
    tc class add dev "$IFACE" parent 1:99 classid 1:10 htb rate "${TEL_KBIT}kbit" ceil "${ROOT_KBIT}kbit" prio 1
    tc filter add dev "$IFACE" parent 1: protocol ip prio 1 u32 \
        match mark 10 0xffffffff flowid 1:10
    tc filter add dev "$IFACE" parent 1: protocol ip prio 1 u32 \
        match mark 11 0xffffffff flowid 1:10
    tc qdisc add dev "$IFACE" handle 102: parent 1:10 pfifo

    # Tunnel class (fwmarks 20, 21)
    tc class add dev "$IFACE" parent 1:99 classid 1:20 htb rate "${TUN_KBIT}kbit" ceil "${ROOT_KBIT}kbit" prio 3
    tc filter add dev "$IFACE" parent 1: protocol ip prio 1 u32 \
        match mark 20 0xffffffff flowid 1:20
    tc filter add dev "$IFACE" parent 1: protocol ip prio 1 u32 \
        match mark 21 0xffffffff flowid 1:20
    tc qdisc add dev "$IFACE" handle 103: parent 1:20 pfifo

    echo
done

exit 0
