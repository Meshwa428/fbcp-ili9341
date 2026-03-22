#!/bin/bash
BINARY_NAME="fbcp-ili9341"
STATE_FILE="/tmp/fbcp-touch-state"
PID=$(pidof $BINARY_NAME 2>/dev/null)

# Init state file if missing
if [ ! -f "$STATE_FILE" ]; then
    echo "touch=off" > "$STATE_FILE"
    echo "mode=touchscreen" >> "$STATE_FILE"
fi

if [ -z "$PID" ]; then
    echo "Error: $BINARY_NAME is not running."
    exit 1
fi

read_state() { grep "^$1=" "$STATE_FILE" 2>/dev/null | cut -d= -f2; }

show_status() {
    echo "  PID:        $PID"
    echo "  Touch:      $(read_state touch)"
    echo "  Input Mode: $(read_state mode)"
}

case "$1" in
    on)
        if [ "$(read_state touch)" = "on" ]; then
            echo "Touch is already ON."; show_status; exit 0
        fi
        kill -USR1 "$PID"; sleep 0.2
        echo "Touch enabled."; show_status
        ;;
    off)
        if [ "$(read_state touch)" = "off" ]; then
            echo "Touch is already OFF."; show_status; exit 0
        fi
        kill -USR1 "$PID"; sleep 0.2
        echo "Touch disabled."; show_status
        ;;
    touchpad|tp)
        if [ "$(read_state mode)" = "touchpad" ]; then
            echo "Already in Touchpad mode."; show_status; exit 0
        fi
        kill -USR2 "$PID"; sleep 0.5
        echo "Switched to Touchpad mode."; show_status
        ;;
    touchscreen|ts)
        if [ "$(read_state mode)" = "touchscreen" ]; then
            echo "Already in Touchscreen mode."; show_status; exit 0
        fi
        kill -USR2 "$PID"; sleep 0.5
        echo "Switched to Touchscreen mode."; show_status
        ;;
    status|s)
        echo "$BINARY_NAME status:"; show_status
        ;;
    *)
        echo "Usage: $0 {on|off|touchpad|touchscreen|status}"
        echo ""
        echo "  on / off          - Enable/disable touch input"
        echo "  touchpad    (tp)  - Relative movement + libinput tapping"
        echo "  touchscreen (ts)  - Absolute positioning + direct click"
        echo "  status      (s)   - Show current state"
        exit 1
        ;;
esac
