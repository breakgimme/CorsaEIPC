"""Run host-side validation and a full Arduino CLI build for ESP32.

Requires Arduino CLI with the esp32:esp32 core and a C++ compiler.
Build artifacts stay in this project's build folder.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
from check_key108 import generate_reference_test

root = Path(__file__).resolve().parent
# Catch modes declared in the header but rejected by this bundled driver's switch.
driver = (root / 'src/MCP_CAN/mcp_can.cpp').read_text()
driver = re.sub(r'/\*.*?\*/|//[^\n]*', '', driver, flags=re.S)
mode_switch = driver.split('switch(canIDMode)', 1)[1].split('default:', 1)[0]
supported = set(re.findall(r'case\s*\(\s*(MCP_\w+)\s*\)', mode_switch))
firmware = (root / 'Firmware.h').read_text()
selected = re.findall(r'can\.begin\(\s*(MCP_\w+)', firmware)
assert selected and all(mode in supported for mode in selected), (selected, supported)
print('Bundled MCP_CAN initialization mode compatibility passed.', flush=True)
assert 'can.begin(MCP_ANY,IPC_SPEED,IPC_CLOCK)' in firmware
assert '#define IPC_CLOCK MCP_8MHZ' in firmware
assert '#define IPC_SPEED CAN_33K3BPS' in firmware
assert not re.search(r'can.init_(?:Mask|Filt)\(',firmware)
print('MCP2515 clock, bitrate and reception-mode checks passed.',flush=True)
build = root / 'build'
build.mkdir(exist_ok=True)
# Arduino requires the sketch folder and entry point to share a name.
# Stage only firmware sources so any GitHub checkout directory name works.
stage = build / 'sketch' / 'CorsaEIPC'
if stage.exists():
    shutil.rmtree(stage)
stage.mkdir(parents=True)
files = [p for p in root.iterdir() if p.is_file()] + list((root/'src').rglob('*'))
for source in files:
    if not source.is_file() or source.suffix not in {'.ino','.h','.hpp','.cpp','.c','.S'}:
        continue
    target = stage / source.relative_to(root)
    target.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(source,target)
key_test=generate_reference_test(build)
edit_test=build/'option_edit_test.cpp'
edit_test.write_text('''#include "OptionEdit.h"
constexpr bool allCataloguePermissions() {
  constexpr uint8_t dids[]={2,3,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA8};
  unsigned checked=0;
  for(auto did:dids) {
    uint8_t before[32]={},after[32]={},unknown[32]={}; bool hasUnknown=false;
    for(unsigned i=0;i<32;++i) before[i]=after[i]=unknown[i]=0xA5;
    for(const auto &o:OPTIONS) if(o.did==did) {
      ++checked;
      if(o.flags&1) { unknown[o.byte]^=uint8_t(1<<o.bit); hasUnknown=true; }
      else after[o.byte]^=uint8_t(1<<o.bit);
    }
    if(!validOptionChanges(did,before,after,32)) return false;
    if(hasUnknown && validOptionChanges(did,before,unknown,32)) return false;
  }
  uint8_t before[32]={},after[32]={};
  after[0]=1;
  return checked==1312 && !validOptionChanges(0x90,before,after,17) &&
         validOptionChanges(0xA3,before,before,8);
}
static_assert(allCataloguePermissions(),"Graphical edits must preserve all unknown bits");
static_assert(validDidLength(0xA4,5) && validDidLength(0xA4,11),"A4 short/full variants must be accepted");
static_assert(!validDidLength(0xA4,0) && !validDidLength(0xA4,33),"Invalid configuration lengths must be rejected");
static_assert(validDidLength(0x90,17) && !validDidLength(0x90,5),"VIN must stay exactly 17 bytes");
''')
compiler = os.environ.get('CXX', 'g++')
for source in (key_test, edit_test):
    subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra',
                    '-fconstexpr-ops-limit=100000000', '-I'+str(root),
                    '-c', str(source), '-o', str(source.with_suffix('.o'))], check=True)
print('All 65,536 algorithm 108 keys and 1,312 option permissions passed.', flush=True)
subprocess.run([os.environ.get('ARDUINO_CLI', 'arduino-cli'), 'compile',
                '--fqbn', 'esp32:esp32:esp32', '--warnings', 'all',
                '--build-path', str(build / 'firmware'), str(stage)], check=True)
print('Full ESP32 firmware build passed.', flush=True)
