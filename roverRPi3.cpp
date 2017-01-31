#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_nvic.h"
#include "inc/hw_gpio.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/pin_map.h"
#include "driverlib/sysctl.h"
#include "driverlib/uart.h"
#include "utils/uartstdio.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/timer.h"
#include "driverlib/fpu.h"
#include "driverlib/ssi.h"

//#include "roverKernel/rpiDriver.h"
#include "roverKernel/tm4c1294_hal.h"

#include "roverKernel/uartHW.h"
#include "roverKernel/taskScheduler.h"
#include "roverKernel/esp8266.h"

UartHW comm;
ESP8266 esp;

//#define __DEBUG_SESSION__

//RPIRover rpiRov(6.8f, 14.3f, 25.0f, 40);

void RxHook(uint8_t sockID, uint8_t *buf, uint16_t *len)
{
    comm.Send("Recvd(%d):  %s\n", sockID, buf);

    /*if ((buf[0] == 'H') && (buf[1] == 'e'))
    {
        uint8_t msg[30]={0};
        msg[0]=28;
        __taskSch->PushBackEntrySync(0, 0, 0);//0 time - run task ASAP
        __taskSch->AddStringArg(msg, 1);

        snprintf((char*)msg, 30,"Hello you on the other side!");
        __taskSch->PushBackEntrySync(0, 0, 0);//0 time - run task ASAP
        __taskSch->AddStringArg(msg, 28);
    }*/
}

int main(void)
{
    HAL_BOARD_CLOCK_Init();
    TaskScheduler ts;
    comm.InitHW();

    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOJ);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPION);

    GPIOPinTypeGPIOOutput(GPIO_PORTN_BASE, GPIO_PIN_0 | GPIO_PIN_1);
    GPIOPinWrite(GPIO_PORTN_BASE, GPIO_PIN_0 | GPIO_PIN_1, 0x00);

    GPIOPinTypeGPIOInput(GPIO_PORTJ_BASE,GPIO_PIN_0|GPIO_PIN_1);
    GPIOPadConfigSet(GPIO_PORTJ_BASE,GPIO_PIN_0|GPIO_PIN_1,GPIO_STRENGTH_8MA,GPIO_PIN_TYPE_STD_WPU);
    GPIOPinWrite(GPIO_PORTJ_BASE,GPIO_PIN_0|GPIO_PIN_1,0x00);


    esp.InitHW();
    esp.AddHook(RxHook);
    esp.ConnectAP("sgvfyj7a", "7vxy3b5d");

    comm.Send("Board initialized!\r\n");
    uint32_t cliID;

    cliID = esp.OpenTCPSock("192.168.0.11", 2701);

    if (cliID != ESP_STATUS_ERROR)
    {
        esp.GetClientBySockID(cliID)->SendTCP("Hello from ESP module!\0");
        HAL_DelayUS(2000000);
        esp.GetClientBySockID(cliID)->Close();
    }

    _taskEntry te1(ESP_UID,ESP_T_CONNTCP,0);
    char IPa[] = {"192.168.0.11"};
    uint16_t port = 2701;
    memcpy((void*)te1._args, IPa, 12);
    memcpy((void*)(te1._args+12), &port, 2);
    te1._argN = 14;
    ts.PushBackTask(te1);

    _taskEntry te2(ESP_UID,ESP_T_SENDTCP,0);
    char msg2[2];
    msg2[0] = 23;
    memcpy((void*)te2._args, msg2, 1);
    te2._argN = 23;
    ts.PushBackTask(te2);

    _taskEntry te3(ESP_UID,ESP_T_SENDTCP,0);
    char msg3[] = {"Hello from ESP module!!"};
    memcpy((void*)te3._args, msg3, 23);
    te3._argN = 23;
    ts.PushBackTask(te3);

    while(1)
    {
        TS_GlobalCheck();
    }

}
