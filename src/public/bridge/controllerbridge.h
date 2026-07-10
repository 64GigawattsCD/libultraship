#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ControllerBlockGameInput(uint16_t inputBlockId);
void ControllerUnblockGameInput(uint16_t inputBlockId);
uint8_t ControllerHasForceFeedback(uint8_t port);
void ControllerFFBPlayConstant(uint8_t port, float directionX, float directionY, uint16_t strength, uint16_t lengthMs);
void ControllerFFBPlayConstantWithEnvelope(uint8_t port, float directionX, float directionY, uint16_t strength,
                                           uint16_t lengthMs, uint16_t attackMs, uint16_t fadeMs);
void ControllerFFBPlayPeriodic(uint8_t port, float directionX, float directionY, uint16_t strength, uint16_t periodMs,
                               uint16_t lengthMs);
void ControllerFFBPlayPeriodicWithFade(uint8_t port, float directionX, float directionY, uint16_t strength,
                                       uint16_t periodMs, uint16_t lengthMs, uint16_t fadeMs);
void ControllerFFBUpdateSpringDamper(uint8_t port, uint8_t enabled, uint16_t springStrength, uint16_t damperStrength);
void ControllerFFBStopAll(uint8_t port);

#ifdef __cplusplus
};
#endif
