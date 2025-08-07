#ifndef _COMM_METER_EMU_H_
#define _COMM_METER_EMU_H_

#include "../../lib/eModbus-eModbus/ModbusServerRTU.h"

#define TASK_METER_EMU_PRIO 1
#define METER_EMU_PORT 9929

extern ModbusServerRTU MeterEmuMBserver;

void init_meter_emu();

#endif
