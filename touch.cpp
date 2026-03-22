#include "config.h"

#ifdef ENABLE_TOUCH_READER

#include "touch.h"
#include "spi.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <string.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include "gpu.h"
#include "util.h"
#include <stdlib.h>

#ifndef TOUCH_SWAP_XY
#define TOUCH_SWAP_XY 1
#endif
#ifndef TOUCH_INVERT_X
#define TOUCH_INVERT_X 0
#endif
#ifndef TOUCH_INVERT_Y
#define TOUCH_INVERT_Y 0
#endif

// Dynamic Calibration values (can be updated at runtime)
int TOUCH_X_MIN = 322;
int TOUCH_X_MAX = 3731;
int TOUCH_Y_MIN = 246;
int TOUCH_Y_MAX = 3599;

#ifndef TOUCH_SWAP_XY
#define TOUCH_SWAP_XY 1
#endif
#ifndef TOUCH_INVERT_X
#define TOUCH_INVERT_X 0
#endif
#ifndef TOUCH_INVERT_Y
#define TOUCH_INVERT_Y 0
#endif

volatile bool isTouchModeActive = false;
volatile bool isTouchpadMode = true;
volatile uint64_t lastTouchTime = 0;
static uint64_t lastTapTime = 0;
static int tapCount = 0;

// Gesture state
static uint64_t gestureStartTime = 0;
static int gestureStartX = -1, gestureStartY = -1;
static bool isDragging = false;
// Tap duration thresholds (microseconds) — override via cmake -DGESTURE_TAP_MAX_US=xxx etc.
#ifndef GESTURE_TAP_MAX_US
#define GESTURE_TAP_MAX_US    300000  // < 300ms = tap (left click)
#endif
#ifndef GESTURE_LONG_PRESS_US
#define GESTURE_LONG_PRESS_US 600000  // >= 600ms = long press (right click)
#endif
#ifndef GESTURE_DRAG_PX
#define GESTURE_DRAG_PX       30      // movement > 30 units = drag (no click)
#endif

void LoadCalibration()
{
    FILE *f = fopen("/etc/X11/xorg.conf.d/99-calibration.conf", "r");
    if (!f) f = fopen("/usr/share/X11/xorg.conf.d/99-calibration.conf", "r");
    if (!f) {
        printf("[Touch] Calibration file not found, using defaults.\n");
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int val;
        if (sscanf(line, " Option \"MinX\" \"%d\"", &val) == 1) TOUCH_X_MIN = (val * 4095) / 65535;
        if (sscanf(line, " Option \"MaxX\" \"%d\"", &val) == 1) TOUCH_X_MAX = (val * 4095) / 65535;
        if (sscanf(line, " Option \"MinY\" \"%d\"", &val) == 1) TOUCH_Y_MIN = (val * 4095) / 65535;
        if (sscanf(line, " Option \"MaxY\" \"%d\"", &val) == 1) TOUCH_Y_MAX = (val * 4095) / 65535;
    }
    fclose(f);
    printf("[Touch] Dynamic Calibration Loaded: X=%d..%d, Y=%d..%d\n", TOUCH_X_MIN, TOUCH_X_MAX, TOUCH_Y_MIN, TOUCH_Y_MAX);
}

static int uinput_fd = -1;

static void CreateUinputDevice()
{
    if (uinput_fd >= 0) return; // Already open

    uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) {
        printf("Failed to open /dev/uinput for touch controller\n");
        return;
    }

    ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_RIGHT);

    // Only register BTN_TOOL_FINGER in touchpad mode — this is what makes
    // libinput classify the device as touchpad vs touchscreen
    if (isTouchpadMode) {
        ioctl(uinput_fd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
    }

    ioctl(uinput_fd, UI_SET_EVBIT, EV_ABS);
    ioctl(uinput_fd, UI_SET_ABSBIT, ABS_X);
    ioctl(uinput_fd, UI_SET_ABSBIT, ABS_Y);

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "fbcp-ili9341-touch");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor = 0x1234;
    uidev.id.product = 0x5678;
    uidev.id.version = 1;

    uidev.absmin[ABS_X] = 0;
    uidev.absmax[ABS_X] = 4095;
    uidev.absmin[ABS_Y] = 0;
    uidev.absmax[ABS_Y] = 4095;

    if (write(uinput_fd, &uidev, sizeof(uidev)) < 0) {
        printf("Failed to write to uinput device\n");
        return;
    }

    if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
        printf("Failed to create uinput device\n");
        return;
    }

    printf("[Touch] uinput device created in %s mode\n", isTouchpadMode ? "Touchpad" : "Touchscreen");
}

// Called on SIGUSR2 to switch modes — destroys and recreates the uinput device
static bool wasTouching = false;

// so libinput reclassifies the device type
void RecreateUinputDevice()
{
    if (uinput_fd >= 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        uinput_fd = -1;
    }
    wasTouching = false;
    usleep(100000); // 100ms — give X11 time to detect device removal
    CreateUinputDevice();
}

void InitTouch()
{
    LoadCalibration();
    CreateUinputDevice();

    SET_GPIO_MODE(TOUCH_CS_PIN, 0x01); // output
    SET_GPIO(TOUCH_CS_PIN); // HIGH

    SET_GPIO_MODE(TOUCH_IRQ_PIN, 0x00); // input
    
    printf("Touch controller initialized\n");
}

// Send a momentary button click (press + release)
static void EmitButton(int code)
{
    if (uinput_fd < 0) return;
    struct input_event ev[4];
    memset(ev, 0, sizeof(ev));
    // press
    ev[0].type = EV_KEY; ev[0].code = code; ev[0].value = 1;
    ev[1].type = EV_SYN; ev[1].code = SYN_REPORT;
    // release
    ev[2].type = EV_KEY; ev[2].code = code; ev[2].value = 0;
    ev[3].type = EV_SYN; ev[3].code = SYN_REPORT;
    write(uinput_fd, ev, sizeof(ev));
}

// Emit absolute coordinates (used by both modes for movement)
static void EmitAbsMove(int x, int y)
{
    if (uinput_fd < 0) return;
    struct input_event ev[3];
    memset(ev, 0, sizeof(ev));
    ev[0].type = EV_ABS; ev[0].code = ABS_X; ev[0].value = x;
    ev[1].type = EV_ABS; ev[1].code = ABS_Y; ev[1].value = y;
    ev[2].type = EV_SYN; ev[2].code = SYN_REPORT;
    write(uinput_fd, ev, sizeof(ev));
}

static uint16_t SPIXfer16(uint8_t cmd)
{
    spi->fifo = cmd;
    while(!(spi->cs & (BCM2835_SPI0_CS_RXD|BCM2835_SPI0_CS_DONE))) /*nop*/;
    uint32_t dump = spi->fifo; (void)dump;

    spi->fifo = 0x00;
    while(!(spi->cs & (BCM2835_SPI0_CS_RXD|BCM2835_SPI0_CS_DONE))) /*nop*/;
    uint8_t high = spi->fifo;

    spi->fifo = 0x00;
    while(!(spi->cs & (BCM2835_SPI0_CS_RXD|BCM2835_SPI0_CS_DONE))) /*nop*/;
    uint8_t low = spi->fifo;

    uint16_t val = (high << 8) | low;
    val >>= 3;
    return val & 0x0FFF;
}

void ReadTouch()
{
    // If not in touch mode, we ONLY perform double-tap wake detection via GPIO (no SPI)
    bool isTouching = (GET_GPIO(TOUCH_IRQ_PIN) == 0);
    uint64_t now = tick();

    if (!isTouching) {
        if (wasTouching) {
            wasTouching = false;
            
            if (isTouchpadMode) {
                // Touchpad Mode: pen-up with BTN_TOOL_FINGER
                // libinput handles tapping/gestures automatically
                struct input_event ev[3];
                memset(ev, 0, sizeof(ev));
                ev[0].type = EV_KEY; ev[0].code = BTN_TOUCH; ev[0].value = 0;
                ev[1].type = EV_KEY; ev[1].code = BTN_TOOL_FINGER; ev[1].value = 0;
                ev[2].type = EV_SYN; ev[2].code = SYN_REPORT;
                write(uinput_fd, ev, sizeof(ev));
            } else {
                // Touchscreen Mode: pen-up first, then gesture engine
                struct input_event ev[2];
                memset(ev, 0, sizeof(ev));
                ev[0].type = EV_KEY; ev[0].code = BTN_TOUCH; ev[0].value = 0;
                ev[1].type = EV_SYN; ev[1].code = SYN_REPORT;
                write(uinput_fd, ev, sizeof(ev));

                // Then check for tap/long-press gestures
                uint64_t duration = now - gestureStartTime;
                if (!isDragging) {
                    if (duration < GESTURE_TAP_MAX_US) {
                        printf("[Touch] Tap → Left Click\n");
                        EmitButton(BTN_LEFT);
                    } else if (duration >= GESTURE_LONG_PRESS_US) {
                        printf("[Touch] Long Press → Right Click\n");
                        EmitButton(BTN_RIGHT);
                    }
                }
            }
        }
        return;
    }

    if (!wasTouching) {
        lastTouchTime = now;
        if (isTouchpadMode) {
            // Touchpad Mode: pen-down with BTN_TOOL_FINGER
            struct input_event ev[3];
            memset(ev, 0, sizeof(ev));
            ev[0].type = EV_KEY; ev[0].code = BTN_TOUCH; ev[0].value = 1;
            ev[1].type = EV_KEY; ev[1].code = BTN_TOOL_FINGER; ev[1].value = 1;
            ev[2].type = EV_SYN; ev[2].code = SYN_REPORT;
            write(uinput_fd, ev, sizeof(ev));
        } else {
            // Touchscreen Mode: standard pen-down (BTN_TOUCH held during contact)
            struct input_event ev[2];
            memset(ev, 0, sizeof(ev));
            ev[0].type = EV_KEY; ev[0].code = BTN_TOUCH; ev[0].value = 1;
            ev[1].type = EV_SYN; ev[1].code = SYN_REPORT;
            write(uinput_fd, ev, sizeof(ev));
        }
    }

    // Full SPI touch tracking only if mode is ACTIVE
    if (!isTouchModeActive) return;

    printf("[Touch] IRQ asserted, starting touch read\n");

    // Deselect the display explicitly so it doesn't eavesdrop the Touch SPI data
#if !defined(DISPLAY_NEEDS_CHIP_SELECT_SIGNAL)
#ifdef DISPLAY_USES_CS1
    SET_GPIO(GPIO_SPI0_CE1);
#else
    SET_GPIO(GPIO_SPI0_CE0);
#endif
#endif

    // Prepare a SPI setting that uses a dummy hardware CS (2) to prevent the hardware from asserting CE0/CE1.
    uint32_t TOUCH_SPI_SETTINGS = (DISPLAY_SPI_DRIVE_SETTINGS & ~3) | 2;

    printf("[Touch] Clearing SPI state...\n");
    spi->cs = BCM2835_SPI0_CS_CLEAR | TOUCH_SPI_SETTINGS;
    __sync_synchronize();

    printf("[Touch] Halving SPI speed locally to ~2MHz...\n");
    spi->clk = 200;
    __sync_synchronize();

    printf("[Touch] Setting TA...\n");
    spi->cs = BCM2835_SPI0_CS_TA | TOUCH_SPI_SETTINGS;

    uint32_t avgX = 0, avgY = 0;
    const int NUM_SAMPLES = 4;

    for (int i = 0; i < NUM_SAMPLES; ++i) {
        CLEAR_GPIO(TOUCH_CS_PIN);
        avgY += SPIXfer16(0x90);
        SET_GPIO(TOUCH_CS_PIN);

        CLEAR_GPIO(TOUCH_CS_PIN);
        avgX += SPIXfer16(0xD0);
        SET_GPIO(TOUCH_CS_PIN);
    }
    
    uint16_t x = avgX / NUM_SAMPLES;
    uint16_t y = avgY / NUM_SAMPLES;

    spi->cs = BCM2835_SPI0_CS_CLEAR | TOUCH_SPI_SETTINGS;
    __sync_synchronize();

    spi->clk = SPI_BUS_CLOCK_DIVISOR;
    __sync_synchronize();

    spi->cs = BCM2835_SPI0_CS_TA | DISPLAY_SPI_DRIVE_SETTINGS;
    spi->dlen = 2; // Restore fast 8-clock SPI mode
    __sync_synchronize();

    // Re-select the display
#if !defined(DISPLAY_NEEDS_CHIP_SELECT_SIGNAL)
#ifdef DISPLAY_USES_CS1
    CLEAR_GPIO(GPIO_SPI0_CE1);
#else
    CLEAR_GPIO(GPIO_SPI0_CE0);
#endif
#endif

#if TOUCH_SWAP_XY
    uint16_t temp = x;
    x = y;
    y = temp;
#endif

    // Apply Calibration: Clamp and Stretch from raw ADC range to 0..4095
    if (x < TOUCH_X_MIN) x = TOUCH_X_MIN;
    if (x > TOUCH_X_MAX) x = TOUCH_X_MAX;
    if (y < TOUCH_Y_MIN) y = TOUCH_Y_MIN;
    if (y > TOUCH_Y_MAX) y = TOUCH_Y_MAX;
    
    x = (uint16_t)(((uint32_t)(x - TOUCH_X_MIN) * 4095) / (TOUCH_X_MAX - TOUCH_X_MIN));
    y = (uint16_t)(((uint32_t)(y - TOUCH_Y_MIN) * 4095) / (TOUCH_Y_MAX - TOUCH_Y_MIN));

#if TOUCH_INVERT_X
    x = 4095 - x;
#endif

#if TOUCH_INVERT_Y
    y = 4095 - y;
#endif

    // EMA filter init / reset on first contact
    static int filteredX = -1, filteredY = -1;
    if (!wasTouching) {
        filteredX = x;
        filteredY = y;
        // Record gesture start
        gestureStartTime = now;
        gestureStartX = x;
        gestureStartY = y;
        isDragging = false;
    } else {
        filteredX = (x * 40 + filteredX * 60) / 100;
        filteredY = (y * 40 + filteredY * 60) / 100;
    }

    // Check if finger has moved enough to count as a drag
    if (!isDragging) {
        int dx = abs(filteredX - gestureStartX);
        int dy = abs(filteredY - gestureStartY);
        if (dx > GESTURE_DRAG_PX || dy > GESTURE_DRAG_PX)
            isDragging = true;
    }

    // Deadzone: only emit move if position changed enough
    static int lastEmittedX = -1, lastEmittedY = -1;
    const int DEADZONE = 16;
    if (abs(filteredX - lastEmittedX) > DEADZONE || abs(filteredY - lastEmittedY) > DEADZONE || !wasTouching) {
        // Both modes use absolute coordinates
        // In touchpad mode, libinput converts ABS to relative internally
        // In touchscreen mode, ABS maps directly to screen position
        EmitAbsMove(filteredX, filteredY);
        lastEmittedX = filteredX;
        lastEmittedY = filteredY;
    }
    wasTouching = true;
}

#endif
