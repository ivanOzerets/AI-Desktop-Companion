#pragma once
#include "types.h"

float flightGlobalT(const std::string& phase, int fIdx, int fTotal);
void  bezierAt(float t, int& outX, int& outY);
void  planFly();
void  startFlySequence();
void  checkLedgeValidity();
