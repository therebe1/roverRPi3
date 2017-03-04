/**
 * hal_common_tm4c.h
 *
 *  Created on: Mar 4, 2017
 *      Author: Vedran Mikov
 */
#include "roverKernel/hwconfig.h"

#ifndef ROVERKERNEL_HAL_TM4C1294_HAL_COMMON_TM4C_H_
#define ROVERKERNEL_HAL_TM4C1294_HAL_COMMON_TM4C_H_

#define HAL_OK                  0
/**     SysTick peripheral error codes      */
#define HAL_SYSTICK_PEROOR      1   /// Period value for SysTick is out of range
#define HAL_SYSTICK_SET_ERR     2   /// SysTick has already been configured
#define HAL_SYSTICK_NOTSET_ERR  3   /// SysTick hasn't been configured yet
/**     Engines error codes                 */
#define HAL_ENG_PWMOOR          4   /// PWM value out of range
#define HAL_ENG_EOOR            5   /// Engine ID out of range
#define HAL_ENG_ILLM            6   /// Illegal mask for H-bridge configuration


#ifdef __cplusplus
extern "C"
{
#endif

/// Global clock variable
extern uint32_t g_ui32SysClock;


extern void         HAL_DelayUS(uint32_t us);
extern void         HAL_BOARD_CLOCK_Init();
extern void         UNUSED (int32_t arg);
extern uint32_t     _TM4CMsToCycles(uint32_t ms);

extern void         HAL_SetPWM(uint32_t id, uint32_t pwm);
extern uint32_t     HAL_GetPWM(uint32_t id);

#ifdef __cplusplus
}
#endif

#endif /* ROVERKERNEL_HAL_TM4C1294_HAL_COMMON_TM4C_H_ */
