// Opel Corsa E IPC diagnostics, using the bundled MCP_CAN driver.
// See README.md for hardware, security key entry, and verification limits.
#include "Firmware.h"

void setup() { ipcSetup(); }
void loop() { ipcLoop(); }
