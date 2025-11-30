#!/bin/sh

# monitor.sh
# Usage:
#   Put one or more interfaces into monitor mode:
#       ./monitor.sh monitor <iface> [iface...]
#
#   Restore one or more interfaces back to managed mode:
#       ./monitor.sh restore <iface> [iface...]

CONFIG_FILE="/etc/wfb.conf"

CHANNEL_DEFAULT="161"
BANDWIDTH_DEFAULT="HT20"
TXPOWER_DEFAULT="500"
REGION_DEFAULT="US"

# Load simple VAR=VALUE overrides from /etc/wfb.conf when present

ENV_CHANNEL="${CHANNEL:-}"
ENV_BANDWIDTH="${BANDWIDTH:-}"
ENV_TXPOWER="${TXPOWER:-}"
ENV_REGION="${REGION:-}"

if [ -r "$CONFIG_FILE" ]; then
    while IFS= read -r line; do
        case "$line" in
            [A-Za-z_]*=*)
                key=${line%%=*}
                value=${line#*=}
                case "$key" in
                    CHANNEL|BANDWIDTH|TXPOWER|REGION)
                        eval "$key=\"${value}\""
                        ;;
                esac
                ;;
        esac
    done < "$CONFIG_FILE"
fi

CHANNEL="${ENV_CHANNEL:-${CHANNEL:-$CHANNEL_DEFAULT}}"
BANDWIDTH="${ENV_BANDWIDTH:-${BANDWIDTH:-$BANDWIDTH_DEFAULT}}"
TXPOWER="${ENV_TXPOWER:-${TXPOWER:-$TXPOWER_DEFAULT}}"
REGION="${ENV_REGION:-${REGION:-$REGION_DEFAULT}}"

usage() {
    echo "Usage:"
    echo "  $0 monitor <iface> [iface...]"
    echo "  $0 restore <iface> [iface...]"
    exit 1
}

if [ "$#" -lt 2 ]; then
    usage
fi

MODE="$1"
shift

# Set regulatory domain early
if ! iw reg set "$REGION"; then
    echo "Warning: failed to set regulatory region to $REGION"
fi

set -e

# Check if nmcli is available
if command -v nmcli >/dev/null 2>&1; then
    HAVE_NMCLI=1
else
    HAVE_NMCLI=0
fi

nm_set_managed() {
    local iface="$1"
    local state="$2"  # yes/no

    if [ "$HAVE_NMCLI" -eq 1 ]; then
        nmcli device set "$iface" managed "$state" || \
            echo "Warning: nmcli failed for $iface (managed $state)"
    else
        echo "Info: nmcli not found, skipping NetworkManager control for $iface"
    fi
}

monitor_mode() {
    if [ "$#" -lt 1 ]; then
        echo "monitor: need at least one interface" >&2
        usage
    fi

    for IFACE in "$@"; do
        echo ">>> Enabling monitor mode on $IFACE"

        # Let NetworkManager stop managing it (if nmcli exists)
        nm_set_managed "$IFACE" no

        sleep 1

        ip link set "$IFACE" down || { echo "Failed to bring $IFACE down"; continue; }

        # Set monitor mode
        if ! iw dev "$IFACE" set monitor otherbss; then
            echo "Failed to set monitor mode on $IFACE"
            continue
        fi

        ip link set "$IFACE" up || { echo "Failed to bring $IFACE up"; continue; }

        # Set channel/bandwidth
        if ! iw dev "$IFACE" set channel "$CHANNEL" "$BANDWIDTH"; then
            echo "Warning: failed to set channel on $IFACE"
        fi

        if ! iw dev "$IFACE" set txpower fixed "$TXPOWER"; then
            echo "Warning: failed to set txpower on $IFACE"
        fi

        echo ">>> $IFACE now in monitor mode on channel $CHANNEL $BANDWIDTH"
        echo
    done
}

restore_mode() {
    if [ "$#" -lt 1 ]; then
        echo "restore: need at least one interface" >&2
        usage
    fi

    for IFACE in "$@"; do
        echo ">>> Restoring $IFACE to managed mode"

        ip link set "$IFACE" down || echo "Warning: failed to bring $IFACE down"

        # Try to set back to managed/station mode
        if ! iw dev "$IFACE" set type managed 2>/dev/null; then
            iw dev "$IFACE" set type station 2>/dev/null || \
                echo "Warning: failed to set $IFACE back to managed/station"
        fi

        ip link set "$IFACE" up || echo "Warning: failed to bring $IFACE up"

        # Let NetworkManager manage it again (if nmcli exists)
        nm_set_managed "$IFACE" yes

        echo ">>> $IFACE restored"
        echo
    done
}

case "$MODE" in
    monitor)
        monitor_mode "$@"
        ;;
    restore)
        restore_mode "$@"
        ;;
    *)
        usage
        ;;
esac

exit 0

