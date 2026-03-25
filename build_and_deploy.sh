#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
BINARY_NAME="fbcp-ili9341"
INSTALL_PATH="/usr/local/sbin/$BINARY_NAME"
SERVICE_NAME="$BINARY_NAME.service"

echo "========================================"
echo "  fbcp-ili9341 Build & Deploy Script"
echo "========================================"

# --- Step 1: Build ---
echo ""
echo "[1/5] Building..."
if [ ! -d "$BUILD_DIR" ]; then
    mkdir -p "$BUILD_DIR"
fi

cd "$BUILD_DIR"

# Run cmake if no Makefile exists yet
make clean
if [ ! -f "Makefile" ]; then
    echo "  Running cmake..."
    cmake \
        -DSPI_BUS_CLOCK_DIVISOR=6 \
        -DADAFRUIT_ILI9341_PITFT=ON \
        -DGPIO_TFT_RESET_PIN=25 \
        -DGPIO_TFT_DATA_CONTROL=24 \
        -DSTATISTICS=0 \
        -DGPIO_TFT_BACKLIGHT=13 \
        -DBACKLIGHT_CONTROL=ON \
        -DENABLE_TOUCH_READER=ON \
        -DTOUCH_CS_PIN=7 \
        -DTOUCH_IRQ_PIN=21 \
        -DTOUCH_TARGET_FRAME_RATE=15 \
        -DDISPLAY_SWAP_BGR=ON \
        -DDISPLAY_ROTATE_180_DEGREES=ON \
        ..
fi

make -j$(nproc)
echo "  Build complete."

# --- Step 2: Write systemd service file ---
echo ""
echo "[2/5] Writing systemd service file..."
sudo tee /etc/systemd/system/$SERVICE_NAME > /dev/null << 'EOF'
[Unit]
Description=Framebuffer Copy to SPI
DefaultDependencies=no
After=systemd-modules-load.service
Before=basic.target
StartLimitIntervalSec=10
StartLimitBurst=10

[Service]
Type=simple
ExecStart=/usr/local/sbin/fbcp-ili9341
Restart=always
RestartSec=1

[Install]
WantedBy=sysinit.target
EOF
echo "  Service file written to /etc/systemd/system/$SERVICE_NAME"
sudo systemctl daemon-reload

# --- Step 3: Stop the running service ---
echo ""
echo "[3/5] Stopping running service..."
if systemctl is-active --quiet $SERVICE_NAME 2>/dev/null; then
    sudo systemctl stop $SERVICE_NAME
    echo "  Service stopped."
else
    # Also kill any manually-started instances
    if pidof $BINARY_NAME > /dev/null 2>&1; then
        sudo killall $BINARY_NAME 2>/dev/null || true
        sleep 1
        echo "  Killed running instance."
    else
        echo "  No running instance found."
    fi
fi

# --- Step 4: Install the new binary ---
echo ""
echo "[4/5] Installing new binary to $INSTALL_PATH..."
sudo cp "$BUILD_DIR/$BINARY_NAME" "$INSTALL_PATH"
sudo chmod 755 "$INSTALL_PATH"
echo "  Binary installed."

# --- Step 5: Enable and start service ---
echo ""
echo "[5/5] Enabling and starting service..."
sudo systemctl enable $SERVICE_NAME
sudo systemctl start $SERVICE_NAME
echo "  Service started."

echo ""
echo "========================================"
echo "  Done! Service status:"
echo "========================================"
systemctl status $SERVICE_NAME --no-pager -l || true
