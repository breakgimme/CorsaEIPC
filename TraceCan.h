#pragma once
#include "src/MCP_CAN/mcp_can.h"

// Observe calls without changing the bundled driver's implementation or result.
void ipcCanTxLog(unsigned long id, uint8_t len, const uint8_t *data,
                 int result, uint32_t elapsed);
class TraceCan : public MCP_CAN {
public:
  explicit TraceCan(uint8_t cs) : MCP_CAN(cs) {}
  uint8_t sendMsgBuf(unsigned long id,uint8_t ext,uint8_t len,uint8_t *data) {
    unsigned long logged=(id&0x1FFFFFFFUL)|(ext?0x80000000UL:0);
    ipcCanTxLog(logged,len,data,-1,0);
    uint32_t start=micros();
    uint8_t result=MCP_CAN::sendMsgBuf(id,ext,len,data);
    ipcCanTxLog(logged,len,data,result,micros()-start);
    return result;
  }
  uint8_t sendMsgBuf(unsigned long id,uint8_t len,uint8_t *data) {
    ipcCanTxLog(id,len,data,-1,0);
    uint32_t start=micros();
    uint8_t result=MCP_CAN::sendMsgBuf(id,len,data);
    ipcCanTxLog(id,len,data,result,micros()-start);
    return result;
  }
};
