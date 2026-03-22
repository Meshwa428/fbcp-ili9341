#pragma once

#include "config.h"
#include <stdint.h>

#ifdef ENABLE_TOUCH_READER

extern volatile bool isTouchModeActive;
extern volatile bool isTouchpadMode;
extern volatile uint64_t lastTouchTime;

void InitTouch(void);
void ReadTouch(void);
void LoadCalibration(void);
void RecreateUinputDevice(void);

#else

#define isTouchModeActive false
#define lastTouchTime 0

inline void InitTouch(void) {}
inline void ReadTouch(void) {}
inline void LoadCalibration(void) {}
inline void RecreateUinputDevice(void) {}

#endif
