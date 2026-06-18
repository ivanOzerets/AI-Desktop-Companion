#pragma once
#include "types.h"

void loadIdentityTimes();
int  currentTimeMinutes();
bool isNighttime();
int  inactivitySeconds();
void enterSleep();
void wakeUp();
void updateSleepState();
