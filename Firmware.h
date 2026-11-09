#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <stdarg.h>
#include <esp_random.h>
#include "TraceCan.h"
#include "Options.h"
#include "WebUI.h"
#include "Key108.h"
#include "OptionEdit.h"
#include "OptionCatalogue.h"

// ESP32 Devkit / MCP2515 defaults. Check the crystal on your board.
#define IPC_CS 5
#define CAN_INT 2
#define SERIAL_BAUD_RATE 921600
#define IPC_SCK 18
#define IPC_MISO 19
#define IPC_MOSI 23
#define IPC_CLOCK MCP_8MHZ
#define IPC_SPEED CAN_33K3BPS
static const uint16_t TX_ID = 0x24C, RX_ID = 0x64C;
static const char *AP_NAME = "CorsaE-IPC", *AP_PASSWORD = "corsa-ipc-setup";

static TraceCan can(IPC_CS);
static WebServer web(80);
static Preferences storage;
static bool canReady = false, nvsReady = false, session = false, unlocked = false;
static const char *canInitStage = "not started";
static uint8_t canInitCode = CAN_FAILINIT;
static bool seedPending = false, traceFrames = true;
static uint16_t currentSeed = 0;
static uint32_t seedAt = 0, lastActivity = 0, lastUserRequest = 0;
static uint8_t lastNrc=0;
static constexpr uint32_t RESPONSE_TIMEOUT_MS=10000, REQUEST_DEADLINE_MS=30000;
static String output;
static bool capture = false;
// Bounded recent log for browser polling, including activity outside commands.
static char logRing[32768];
static uint64_t logWritten=0;
static uint32_t logBoot=0;
static void appendLog(const char *text) {
  while(*text) { logRing[logWritten%sizeof(logRing)]=*text++; ++logWritten; }
}
static const uint8_t dids[] = {0x02,0x03,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA8,0x90};
static const uint8_t sizes[] = {32,32,20,12,8,11,32,12,5,17};
static uint8_t stagedDid = 0, stagedOld[32], stagedNew[32], stagedSize = 0;
static bool staged = false, vinVerified = false;
static uint8_t editorBase[9][32];
static uint8_t editorSize[9]={0};
static uint32_t editorToken[9]={0};
static void invalidateEditor() { memset(editorToken,0,sizeof(editorToken)); }

static void say(const char *fmt, ...) {
  char buf[768]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
  Serial.print(buf);
  appendLog(buf);
  if (capture && output.length() < 80000) output += buf;
}
static void logFrame(const char *direction,unsigned long id,uint8_t len,const uint8_t *data) {
  if(!traceFrames) return;
  bool ext=id&0x80000000UL, rtr=id&0x40000000UL;
  char line[144];
  int used=snprintf(line,sizeof(line),"[%lu ms] %s %s ID=%0*lX DLC=%u%s DATA=",
      millis(),direction,ext?"EXT":"STD",ext?8:3,id&0x1FFFFFFFUL,len,rtr?" RTR":"");
  if(!rtr) for(uint8_t i=0;i<min(uint8_t(8),len);++i)
    used+=snprintf(line+used,sizeof(line)-used," %02X",data[i]);
  say("%s\n",line);
}
void ipcCanTxLog(unsigned long id,uint8_t len,const uint8_t *data,int result,uint32_t elapsed) {
  if(result<0) { logFrame("TX-TRY",id,len,data); return; }
  if(traceFrames || result!=CAN_OK) say("[%lu ms] TX-RESULT ID=%08lX code=%d elapsed=%lu us%s\n",
      millis(),id&0x1FFFFFFFUL,result,elapsed,result==CAN_OK?" complete":" NOT CONFIRMED");
}
static void serveLogs() {
  uint64_t after=strtoull(web.arg("after").c_str(),nullptr,10);
  uint64_t oldest=logWritten>sizeof(logRing)?logWritten-sizeof(logRing):0;
  bool reset=web.arg("boot")!=String(logBoot);
  if(reset || after>logWritten) after=oldest;
  bool lost=after<oldest;
  if(lost) after=oldest;
  // Bound each transfer; client advances only to the returned cursor.
  uint64_t end=min(logWritten,after+uint64_t(sizeof(logRing)));
  String body; body.reserve(size_t(end-after)+100);
  if(lost) body+="[Log buffer overrun: older output lost; USB serial can record continuously.]\n";
  for(uint64_t i=after;i<end;++i) body+=logRing[i%sizeof(logRing)];
  char cursor[24]; snprintf(cursor,sizeof(cursor),"%llu",(unsigned long long)end);
  web.sendHeader("X-Log-Cursor",cursor);
  web.sendHeader("X-Log-Boot",String(logBoot));
  web.sendHeader("Cache-Control","no-store");
  web.send(200,"text/plain",body);
}
static int indexOf(uint8_t did) {
  for (unsigned i=0;i<sizeof(dids);++i) if (dids[i]==did) return i;
  return -1;
}
static void revoke() { unlocked=false; seedPending=false; vinVerified=false; }
static void stopDiagnostics() { session=false; revoke(); }
static void printCalculatedKey() {
  if(!seedPending || millis()-seedAt>30000) {
    say("ERROR: request a fresh seed before calculating its key. No CAN request was sent.\n"); return;
  }
  say("CALCULATED_KEY=%04X SEED=%04X ALGORITHM=108\n",corsaEKey108(currentSeed),currentSeed);
  say("Calculation only; review and submit the key once to unlock.\n");
}
static const char *nrcName(uint8_t nrc) {
  switch(nrc) {
    case 0x12: return "subFunctionNotSupported";
    case 0x22: return "conditionsNotCorrect";
    case 0x31: return "requestOutOfRange";
    case 0x35: return "invalidKey";
    case 0x36: return "exceedNumberOfAttempts";
    case 0x37: return "requiredTimeDelayNotExpired";
    case 0x78: return "responsePending";
    default: return "unmapped NRC";
  }
}
static void canStatus() {
  say("CAN=%s; stage=%s; result=%u. CS=%u SCK=%u MISO=%u MOSI=%u; crystal=%s; 33.3 kbps; TX=%03X RX=%03X\n",
      canReady?"ready":"FAILED",canInitStage,canInitCode,IPC_CS,IPC_SCK,IPC_MISO,IPC_MOSI,
      IPC_CLOCK==MCP_8MHZ?"8 MHz":IPC_CLOCK==MCP_16MHZ?"16 MHz":"other",TX_ID,RX_ID);
  if(canReady) {
    uint8_t flags=can.getError(),tx=can.errorCountTX(),rx=can.errorCountRX();
    say("EFLG=%02X TEC=%u REC=%u; bus-off=%s; TX-passive=%s; RX-passive=%s; driver TX timeout=%u us\n",
        flags,tx,rx,(flags&MCP_EFLG_TXBO)?"yes":"no",(flags&MCP_EFLG_TXEP)?"yes":"no",(flags&MCP_EFLG_RXEP)?"yes":"no",TIMEOUTVALUE);
  }
  say("Tester-present keepalive=%s; security=%s; pending seed=%s.\n",session?"on":"off",unlocked?"unlocked":"locked",seedPending?"yes":"no");
}
static void initCan() {
  invalidateEditor();
  canReady=false; session=false; staged=false; revoke();
  pinMode(IPC_CS,OUTPUT);
  pinMode(CAN_INT,INPUT);
  // The driver's begin() starts default SPI using the default pins.
  canInitStage="controller begin";
  while((canInitCode=can.begin(MCP_ANY,IPC_SPEED,IPC_CLOCK))!=CAN_OK) {
    say("CAN BUS Shield init fail; retrying in 100 ms.\n");
    delay(100);
  }
  canInitStage="normal mode"; canInitCode=can.setMode(MCP_NORMAL);
  canReady=canInitCode==CAN_OK;
  if(canReady) canInitStage="complete";
  canStatus();
}
static void dump(uint8_t did, const uint8_t *data, size_t n) {
  say("DID %02X =", did); for(size_t i=0;i<n;++i) say(" %02X",data[i]); say("\n");
  if(did==0x90) { say("VIN: "); for(size_t i=0;i<n;++i) say("%c",data[i]>=32&&data[i]<127?data[i]:'.'); say("\n"); }
}
static bool sendFrame(uint16_t id, const uint8_t *data, uint8_t len=8) {
  if(!canReady) { say("ERROR: CAN is not ready.\n"); return false; }
  uint8_t result=can.sendMsgBuf(id,0,len,const_cast<uint8_t *>(data));
  // With the driver's 2500 us timeout, a slow CAN frame can
  // remain queued. Await its ECU reply; never relabel a timeout as success.
  if(result==CAN_SENDMSGTIMEOUT) {
    say("Driver TX wait expired; frame may still be queued. Waiting for ECU response without resending.\n");
    return true;
  }
  if(result!=CAN_OK) {
    say("ERROR: CAN transmit failed, code=%u (%s).\n",result,
        result==CAN_SENDMSGTIMEOUT?"transmission completion timeout":result==CAN_GETTXBFTIMEOUT?"no free TX buffer":"driver error");
    session=false; revoke();
    say("Controller remains enabled. No automatic request retry.\n");
    return false;
  }
  return true;
}
static bool frame(uint8_t *data, uint8_t &len, uint32_t timeout) {
  uint32_t start=millis();
  while(millis()-start<timeout) {
    if(can.checkReceive()==CAN_MSGAVAIL) {
      unsigned long id;
      // This overload encodes extended/RTR flags into id, so both are rejected.
      if(can.readMsgBuf(&id,&len,data)!=CAN_OK) continue;
      logFrame("RX",id,len,data);
      if(id!=RX_ID || len<1 || len>8) continue;
      return true;
    }
    delay(1);
  }
  say("ERROR: CAN/ISO-TP timeout.\n"); return false;
}
static void discardRx() {
  uint8_t d[8],n; unsigned long id;
  // Bounded even on a busy vehicle bus.
  for(int i=0;i<64 && can.checkReceive()==CAN_MSGAVAIL;++i)
    if(can.readMsgBuf(&id,&n,d)==CAN_OK) logFrame("RX-STALE",id,n,d);
}
static bool flow(uint8_t &block, uint32_t &gapUs) {
  uint32_t start=millis(); int waits=0;
  while(millis()-start<3000) {
    uint8_t d[8],n;
    if(!frame(d,n,1000)) return false;
    if(n<3 || (d[0]>>4)!=3) { say("ERROR: expected flow control.\n"); return false; }
    uint8_t status=d[0]&15;
    if(status==1 && ++waits<=3) continue;
    if(status!=0) { say("ERROR: FC WAIT limit/overflow/status %u.\n",status); return false; }
    block=d[1];
    if(d[2]<=0x7F) gapUs=uint32_t(d[2])*1000;
    else if(d[2]>=0xF1 && d[2]<=0xF9) gapUs=(d[2]-0xF0)*100;
    else { say("ERROR: reserved STmin.\n"); return false; }
    return true;
  }
  return false;
}
static bool sendPayload(const uint8_t *p, size_t n) {
  if(n==0 || n>64) return false;
  uint8_t d[8]={0};
  if(n<=7) { d[0]=n; memcpy(d+1,p,n); return sendFrame(TX_ID,d); }
  d[0]=0x10|(n>>8); d[1]=n; memcpy(d+2,p,6);
  if(!sendFrame(TX_ID,d)) return false;
  uint8_t block=0; uint32_t gap=0;
  if(!flow(block,gap)) return false;
  size_t pos=6; uint8_t seq=1,count=0;
  while(pos<n) {
    // Conservative minimum spacing also avoids overflowing MCP2515 receive buffers.
    if(gap>=1000) delay((gap+999)/1000); else delayMicroseconds(gap);
    memset(d,0,8); d[0]=0x20|seq; size_t take=min(size_t(7),n-pos);
    memcpy(d+1,p+pos,take);
    if(!sendFrame(TX_ID,d)) return false;
    pos+=take; seq=(seq+1)&15;
    if(block && ++count==block && pos<n) { count=0; if(!flow(block,gap)) return false; }
  }
  return true;
}
static bool receivePayload(uint8_t *p, size_t &n, uint32_t timeout) {
  uint8_t d[8],len;
  if(!frame(d,len,timeout)) return false;
  if((d[0]>>4)==0) {
    n=d[0]&15;
    if(n==0 || n>7 || len<n+1) { say("ERROR: malformed single frame.\n"); return false; }
    memcpy(p,d+1,n); return true;
  }
  if((d[0]>>4)!=1 || len!=8) { say("ERROR: expected ISO-TP first frame.\n"); return false; }
  n=((d[0]&15)<<8)|d[1];
  if(n<=7 || n>64) { say("ERROR: unsupported ISO-TP length.\n"); return false; }
  memcpy(p,d+2,6); size_t pos=6; uint8_t seq=1;
  // One CF per block, 5 ms spacing: robust against MCP2515's two RX buffers.
  uint8_t fc[8]={0x30,1,5,0,0,0,0,0};
  while(pos<n) {
    if(!sendFrame(TX_ID,fc) || !frame(d,len,1000)) return false;
    size_t take=min(size_t(7),n-pos);
    if((d[0]>>4)!=2 || (d[0]&15)!=seq || len<take+1) {
      say("ERROR: ISO-TP sequence/length mismatch.\n"); return false;
    }
    memcpy(p+pos,d+1,take); pos+=take; seq=(seq+1)&15;
  }
  return true;
}
static bool request(const uint8_t *req,size_t reqLen,uint8_t *resp,size_t &respLen,
                    uint32_t firstTimeout=RESPONSE_TIMEOUT_MS) {
  lastNrc=0;
  if(!canReady) { say("ERROR: CAN not ready.\n"); return false; }
  discardRx();
  if(!sendPayload(req,reqLen)) { stopDiagnostics(); return false; }
  uint32_t start=millis(); bool pending=false;
  while(millis()-start<REQUEST_DEADLINE_MS) {
    uint32_t timeout=min(uint32_t(pending?5000:firstTimeout),uint32_t(REQUEST_DEADLINE_MS-(millis()-start)));
    if(!receivePayload(resp,respLen,timeout)) { stopDiagnostics(); return false; }
    if(respLen==3 && resp[0]==0x7F && resp[1]==req[0]) {
      if(resp[2]==0x78) { pending=true; continue; } // Wait; never retransmit a write on responsePending.
      lastNrc=resp[2];
      say("ERROR: service %02X NRC %02X (%s). No automatic retry.\n",req[0],resp[2],nrcName(resp[2]));
      stopDiagnostics(); return false;
    }
    if(respLen<1 || resp[0]!=uint8_t(req[0]+0x40)) {
      say("ERROR: unexpected diagnostic response.\n"); stopDiagnostics(); return false;
    }
    lastActivity=millis(); return true;
  }
  say("ERROR: responsePending deadline. Outcome may be unknown; read before retry.\n"); stopDiagnostics(); return false;
}
static bool simple(const uint8_t *req,size_t n,const uint8_t *expect,size_t en) {
  uint8_t r[64]; size_t rn=0;
  if(!request(req,n,r,rn)) return false;
  if(rn<en || memcmp(r,expect,en)) { say("ERROR: response prefix mismatch.\n"); stopDiagnostics(); return false; }
  return true;
}
static bool readDid(uint8_t did,uint8_t *data,size_t *received=nullptr) {
  int i=indexOf(did); if(i<0) return false;
  uint8_t q[]={0x1A,did},r[64]; size_t n=0;
  if(!request(q,2,r,n)) return false;
  if(n<3 || r[1]!=did || !validDidLength(did,n-2)) { say("ERROR: DID %02X response ID/length mismatch.\n",did); stopDiagnostics(); return false; }
  if(n-2!=sizes[i]) say("DID %02X variant: IPC returned %u bytes; catalogue describes %u. Using actual length; no padding.\n",did,unsigned(n-2),sizes[i]);
  memcpy(data,r+2,n-2); if(received) *received=n-2; return true;
}
static bool saveBytes(const char *prefix,uint8_t did,const uint8_t *data,size_t n) {
  char key[12]; snprintf(key,sizeof(key),"%s%02X",prefix,did);
  if(!nvsReady || storage.putBytes(key,data,n)!=n) { say("ERROR: NVS backup failed; write blocked.\n"); return false; }
  return true;
}
static bool readBackup(const char *prefix,uint8_t did,uint8_t *data,size_t &n) {
  char key[12]; snprintf(key,sizeof(key),"%s%02X",prefix,did);
  if(!nvsReady) return false;
  n=storage.getBytesLength(key);
  return validDidLength(did,n) && storage.getBytes(key,data,n)==n;
}
static bool writeDid(uint8_t did,const uint8_t *data,size_t n) {
  int editorIndex=indexOf(did);
  if(editorIndex>=0 && editorIndex<9) editorToken[editorIndex]=0;
  uint8_t q[34]={0x3B,did},r[64]; size_t rn=0; memcpy(q+2,data,n);
  if(!request(q,n+2,r,rn)) return false;
  // Accept service-only 7B acknowledgements. If the IPC echoes a
  // DID, require it to match; commit still verifies the full payload by readback.
  if(rn>1 && r[1]!=did) {
    stopDiagnostics(); say("ERROR: write acknowledgement DID mismatch.\n"); return false;
  }
  return true;
}
static bool hexNumber(const String &s,uint32_t &v,uint32_t maxValue) {
  if(s.length()==0) return false;
  char *end; v=strtoul(s.c_str(),&end,16);
  return *end==0 && end!=s.c_str() && s[0]!='-' && v<=maxValue;
}
static String arg(const String &s,int which) {
  int start=0;
  for(int i=0;i<=which;++i) {
    while(start<int(s.length()) && s[start]==' ') ++start;
    int end=s.indexOf(' ',start); if(end<0) end=s.length();
    if(i==which) return s.substring(start,end);
    start=end;
  }
  return "";
}
static bool validVin(const String &v) {
  if(v.length()!=17) return false;
  for(int i=0;i<17;++i) if(!((v[i]>='0'&&v[i]<='9') || (v[i]>='A'&&v[i]<='Z'&&v[i]!='I'&&v[i]!='O'&&v[i]!='Q'))) return false;
  return true;
}
static void preview() {
  say("Staged DID %02X (not written):\n",stagedDid);
  dump(stagedDid,stagedOld,stagedSize); dump(stagedDid,stagedNew,stagedSize);
}
static bool beginStage(uint8_t did) {
  staged=false; int i=indexOf(did); if(i<0) return false;
  size_t n=0;
  if(!readDid(did,stagedOld,&n)) return false;
  stagedDid=did; stagedSize=n; memcpy(stagedNew,stagedOld,stagedSize); return true;
}
static void help() {
  say("Commands (numbers HEX):\nread 90 | read A3 | backup | saved | previous DID\n"
      "session (tester keepalive) | session-setup (optional 10 02 / 28)\nseed | calc-key | key HHHH | stage-vin VIN\n"
      "unlock (one fresh seed + immediate algorithm 108 key; no retries) | stop\n"
      "options DID | bit DID BYTE BIT 0/1 | mask DID BYTE MASK POSITIONED_VALUE\n"
      "preview | commit CONFIRM | restore DID | reboot CONFIRM\n"
      "finalize-vin CONFIRM | clear-dtcs CONFIRM | wake | trace 0/1\n"
      "can-status | can-retry (resets session and pending edit)\n"
      "Seed/key algorithm: 108, for the CURRENT seed.\n");
}
static void command(String line) {
  line.trim(); String op=arg(line,0); uint32_t a=0,b=0,c=0,d=0;
  if(op=="help" || !op.length()) { help(); return; }
  if(op=="can-status") { canStatus(); return; }
  if(op=="calc-key") { printCalculatedKey(); return; }
  if(op=="can-retry") { initCan(); return; }
  if(op=="stop") {
    invalidateEditor();
    staged=false; stopDiagnostics();
    say("Stopped local keepalive; edit and unlock cleared. No CAN frame sent. An already queued controller frame cannot be recalled.\n"); return;
  }
  if(op=="trace") { traceFrames=arg(line,1)=="1"; say("Trace %s\n",traceFrames?"on":"off"); return; }
  if(op=="preview") { if(staged) preview(); else say("No edit staged.\n"); return; }
  lastUserRequest=millis();
  if(op=="session" || op=="session-setup") {
    revoke(); session=false;
    const uint8_t q1[]={0x3E},e1[]={0x7E},q2[]={0x10,0x02},e2[]={0x50,0x02},q3[]={0x28},e3[]={0x68};
    if(!simple(q1,1,e1,1)) return;
    session=true;
    if(op=="session") { say("Tester present confirmed; 1-second keepalives enabled. No 10 02 or 28 sent.\n"); return; }
    if(!simple(q2,2,e2,2)) {
      if(lastNrc==0x12) say("10 02 is unsupported by this IPC. Skipping 28; keepalive stopped. No alternative session values will be guessed.\n");
      else { session=false; say("Optional session setup failed; tester keepalive stopped.\n"); }
      return;
    }
    if(simple(q3,1,e3,1)) say("Optional diagnostic setup acknowledged; normal communication disabled. Keepalives remain enabled.\n");
    else { session=false; say("Communication-control setup failed; tester keepalive stopped.\n"); }
    return;
  }
  if(op=="seed" || op=="unlock") {
    // Calculate and send the key synchronously, without tester-present
    // or another seed in between. Manual mode retains only explicitly enabled TP.
    bool immediate=op=="unlock";
    if(immediate) session=false;
    revoke(); uint8_t q[]={0x27,0x01},r[64]; size_t n=0;
    if(request(q,2,r,n,2000) && n==4 && r[1]==1) {
      currentSeed=(uint16_t(r[2])<<8)|r[3]; seedAt=millis(); seedPending=true;
      if(currentSeed==0) {
        seedPending=false; unlocked=true;
        say("Security already unlocked (zero seed). No key sent.\n"); return;
      }
      if(immediate) {
        uint16_t key=corsaEKey108(currentSeed);
        seedPending=false;
        uint8_t keyReq[]={0x27,0x02,uint8_t(key>>8),uint8_t(key)};
        // Exactly one key attempt for this fresh seed, before returning to loop.
        bool ok=request(keyReq,sizeof(keyReq),r,n,2000);
        unlocked=ok && n==2 && r[1]==2;
        if(unlocked) say("Security unlocked. SEED=%04X KEY=%04X ALGORITHM=108. No background keepalive enabled.\n",currentSeed,key);
        else { stopDiagnostics(); say("Unlock not confirmed. No retry or background keepalive; check the response above.\n"); }
        return;
      }
      say("SEED=%04X ALGORITHM=108 (0x6C). Submit the calculated key within 30 s.\n",currentSeed);
      printCalculatedKey();
      say("No implicit tester-present. Keepalive=%s; no automatic seed/key retry.\n",session?"explicitly enabled":"off");
    } else if(lastNrc==0x37 || lastNrc==0x36) say("ECU security delay/attempt limit active. Wait for its delay to expire before another seed or key request.\n");
    else { stopDiagnostics(); say("Seed unavailable or unexpected length. Background keepalive stopped.\n"); }
    return;
  }
  if(op=="key") {
    if(!seedPending || millis()-seedAt>30000 || !hexNumber(arg(line,1),a,0xFFFF)) { seedPending=false; say("ERROR: need a fresh seed and a 16-bit hex key.\n"); return; }
    seedPending=false; uint8_t q[]={0x27,0x02,uint8_t(a>>8),uint8_t(a)},e[]={0x67,0x02};
    unlocked=simple(q,4,e,2);
    if(unlocked) say("Security unlocked.\n");
    else if(lastNrc==0x22) say("Unlock refused: conditionsNotCorrect. This does not establish that the calculated key is wrong. Check diagnostic state and seed/session lifetime.\n");
    else if(lastNrc==0x35) say("Unlock refused: invalidKey. Verify algorithm 108 and the most recent seed; respect any ECU security delay.\n");
    else if(lastNrc==0x36 || lastNrc==0x37) say("Unlock refused: ECU delay/attempt limit active. Wait for the ECU delay before another attempt.\n");
    else say("Unlock not confirmed. Check the preceding response; no automatic retry.\n");
    return;
  }
  if(op=="wake") {
    // Exact frame from OpelCorsaECluster::sendGmlan(); no HV wake flag.
    const uint8_t empty[8]={0};
    if(!sendFrame(0x100,empty,0)) return;
    const uint8_t d[]={0xFD,0x02,0x10,0x04,0,0,0,0};
    if(sendFrame(0x101,d)) say("Init frame sent. MCP2515 alone cannot generate SWCAN high-voltage wake.\n");
    return;
  }
  if(op=="backup" || op=="saved") {
    if(op=="backup") { staged=false; storage.putBool("complete",false); }
    bool ok=true; uint8_t data[32];
    for(unsigned i=0;i<sizeof(dids);++i) {
      size_t n=0;
      bool got=op=="backup"?readDid(dids[i],data,&n):readBackup("snap",dids[i],data,n);
      if(got && op=="backup") got=saveBytes("snap",dids[i],data,n);
      if(got) dump(dids[i],data,n); else {
        ok=false; say("Missing DID %02X\n",dids[i]);
        if(op=="backup") { stopDiagnostics(); say("Backup stopped on first failure; no remaining DIDs requested.\n"); break; }
      }
    }
    if(op=="backup" && ok) ok=storage.putBool("complete",true)==1;
    say("%s %s. Save this output to your PC.\n",op.c_str(),ok?"complete":"INCOMPLETE"); return;
  }
  if(op=="stage-vin") {
    String vin=arg(line,1);
    if(!validVin(vin)) { say("ERROR: VIN must be 17 uppercase letters/digits, excluding I/O/Q.\n"); return; }
    if(beginStage(0x90)) { memcpy(stagedNew,vin.c_str(),17); staged=true; vinVerified=false; preview(); } return;
  }
  if(op=="commit") {
    if(arg(line,1)!="CONFIRM" || !staged || !unlocked) { say("ERROR: stage an edit, unlock, then commit CONFIRM.\n"); return; }
    uint8_t live[32]; size_t liveSize=0;
    if(!readDid(stagedDid,live,&liveSize)) { staged=false; return; }
    if(liveSize!=stagedSize || memcmp(live,stagedOld,stagedSize)) { staged=false; say("ERROR: IPC changed since preview. Re-stage from fresh data.\n"); return; }
    if(!memcmp(live,stagedNew,stagedSize)) { staged=false; say("No change needed.\n"); return; }
    if(!saveBytes("prev",stagedDid,live,stagedSize)) return;
    uint8_t did=stagedDid; staged=false; vinVerified=false;
    if(!writeDid(did,stagedNew,stagedSize)) { say("Write unconfirmed. Read DID before any retry.\n"); return; }
    if(!readDid(did,live,&liveSize) || liveSize!=stagedSize || memcmp(live,stagedNew,stagedSize)) { say("ERROR: write verification failed. Stop and read again.\n"); stopDiagnostics(); return; }
    vinVerified=did==0x90;
    say("WRITE VERIFIED for DID %02X. Reboot/cycle ignition, then read again to check persistence.\n",did); dump(did,live,stagedSize); return;
  }
  if(op=="reboot" || op=="clear-dtcs" || op=="finalize-vin") {
    if(arg(line,1)!="CONFIRM") { say("ERROR: append CONFIRM.\n"); return; }
    if(op=="reboot") {
      invalidateEditor();
      uint8_t q[]={0xAE,0xFD,0x40},e[]={0xEE,0xFD};
      bool ok=simple(q,3,e,2); revoke(); session=false; staged=false;
      if(ok) say("Reboot acknowledged. Wait for IPC, then read again.\n");
      return;
    }
    if(op=="clear-dtcs") { uint8_t q[]={0x04},e[]={0x44}; if(simple(q,1,e,1)) say("Diagnostic information cleared.\n"); return; }
    if(!unlocked || !vinVerified) { say("ERROR: finalize only immediately after verified VIN write in this session.\n"); return; }
    vinVerified=false;
    uint8_t zero=0,q1[]={0xAE,0x30},e1[]={0xEE,0x30},q2[]={0xAE,0x28,0x80},e2[]={0xEE,0x28};
    if(writeDid(0xA7,&zero,1)&&simple(q1,2,e1,2)&&simple(q2,3,e2,2)) say("VIN finalization acknowledged. Cycle ignition, let IPC sleep, then re-read VIN.\n");
    return;
  }
  if(!hexNumber(arg(line,1),a,255) || indexOf(a)<0) { say("ERROR: unknown command or DID. Use help.\n"); return; }
  int idx=indexOf(a); uint8_t data[32];
  if(op=="read") { size_t n=0; if(readDid(a,data,&n)) dump(a,data,n); return; }
  if(op=="previous") { size_t n=0; if(readBackup("prev",a,data,n)) dump(a,data,n); else say("No pre-write backup for this DID.\n"); return; }
  if(op=="restore") {
    size_t n=0;
    if(!storage.getBool("complete",false) || !readBackup("snap",a,data,n)) { say("ERROR: no complete snapshot.\n"); return; }
    if(beginStage(a)) {
      if(n!=stagedSize) { say("ERROR: saved and live DID lengths differ. Restore blocked.\n"); return; }
      memcpy(stagedNew,data,stagedSize); staged=true; preview();
    } return;
  }
  if(op=="options") {
    for(const auto &o:OPTIONS) if(o.did==a) say("%02X B%02X.%u %s [%s]%s\n",o.did,o.byte,o.bit,o.name,o.tags,o.flags&1?" UNKNOWN":o.flags&2?" FIELD":"");
    return;
  }
  if(op!="bit" && op!="mask") { say("ERROR: unknown command.\n"); return; }
  if(a==0x90 || !hexNumber(arg(line,2),b,sizes[idx]-1) || !hexNumber(arg(line,3),c,255) || !hexNumber(arg(line,4),d,255)) { say("ERROR: invalid byte/mask/value.\n"); return; }
  if(op=="bit") {
    if(c>7 || d>1) { say("ERROR: bit 0..7, value 0/1.\n"); return; }
    uint8_t mask=1<<c;
    for(const auto &o:OPTIONS) if(o.did==a && o.byte==b && o.bit==c && o.flags) { say("ERROR: unknown bit or multi-bit field; consult docs.\n"); return; }
    c=mask; d=d?mask:0;
  }
  if(!c || (d&~c)) { say("ERROR: value must fit mask (already shifted into position).\n"); return; }
  for(const auto &o:OPTIONS) if(o.did==a && o.byte==b && (c&(1<<o.bit)) && (o.flags&1)) { say("ERROR: unknown bits must remain unchanged.\n"); return; }
  for(const auto &f:FIELDS) if(f.did==a && f.byte==b && (c&f.mask) && (c&f.mask)!=f.mask) {
    say("ERROR: use complete field mask %02X, not a partial field.\n",f.mask); return;
  }
  if(beginStage(a)) {
    if(b>=stagedSize) { say("ERROR: this byte is not present in the IPC's returned DID layout.\n"); return; }
    stagedNew[b]=(stagedNew[b]&~c)|d; staged=true; preview();
  }
}
static bool editorOrigin() {
  String origin=web.header("Origin");
  return !origin.length() || origin=="http://"+WiFi.softAPIP().toString();
}
static String hexBytes(const uint8_t *data,size_t n) {
  String result; result.reserve(n*2);
  for(size_t i=0;i<n;++i) { char h[3]; snprintf(h,sizeof(h),"%02X",data[i]); result+=h; }
  return result;
}
static bool parseHexBytes(const String &text,uint8_t *data,size_t n) {
  if(text.length()!=n*2) return false;
  for(size_t i=0;i<n;++i) {
    char a=text[i*2],b=text[i*2+1];
    auto digit=[](char c)->int { if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; if(c>='A'&&c<='F') return c-'A'+10; return -1; };
    int hi=digit(a),lo=digit(b); if(hi<0||lo<0) return false;
    data[i]=(hi<<4)|lo;
  }
  return true;
}
static void editorError(int code,const char *message) {
  web.send(code,"application/json",String("{\"error\":\"")+message+"\"}");
}
static void serveEditorLoad() {
  if(!editorOrigin()) { editorError(403,"Origin rejected"); return; }
  uint32_t did=0; int idx=-1;
  if(!hexNumber(web.arg("did"),did,255) || (idx=indexOf(did))<0 || idx>=9) { editorError(400,"Invalid configuration DID"); return; }
  editorToken[idx]=0; lastUserRequest=millis();
  say("Graphical editor: reading DID %02X once.\n",did);
  uint8_t data[32]; size_t n=0;
  if(!readDid(did,data,&n)) { editorError(502,"IPC read failed. Check the CAN log; no automatic retry."); return; }
  editorSize[idx]=n;
  memcpy(editorBase[idx],data,n); editorToken[idx]=esp_random()|1;
  dump(did,data,n);
  web.sendHeader("Cache-Control","no-store");
  web.send(200,"application/json",String("{\"did\":\"")+web.arg("did")+"\",\"token\":\""+String(editorToken[idx],HEX)+"\",\"data\":\""+hexBytes(data,n)+"\"}");
}
static void serveEditorSave() {
  if(!editorOrigin()) { editorError(403,"Origin rejected"); return; }
  uint32_t did=0,token=0; int idx=-1;
  if(!hexNumber(web.arg("did"),did,255) || (idx=indexOf(did))<0 || idx>=9 ||
     !hexNumber(web.arg("token"),token,0xFFFFFFFFUL) || !token || token!=editorToken[idx]) {
    editorError(409,"Load the DID again; this edit is no longer current."); return;
  }
  uint8_t next[32],live[32]; size_t n=editorSize[idx],liveSize=0;
  if(web.arg("confirm")!="1" || !parseHexBytes(web.arg("data"),next,n)) { editorError(400,"Confirm the changes and provide the complete DID."); return; }
  if(!validOptionChanges(did,editorBase[idx],next,n)) { editorError(400,"Unknown bits must remain unchanged."); return; }
  if(!memcmp(next,editorBase[idx],n)) { editorError(400,"No options have changed."); return; }
  // Consume before any bus operation. A lost HTTP reply must never replay a write.
  editorToken[idx]=0; lastUserRequest=millis(); session=false; staged=false; revoke();
  say("Graphical editor: check, back up, unlock with algorithm 108, write and verify DID %02X.\n",did);
  if(!readDid(did,live,&liveSize)) { editorError(502,"Live read failed; nothing written. Load the DID again."); return; }
  if(liveSize!=n || memcmp(live,editorBase[idx],n)) { editorError(409,"IPC data or length changed since loading. Nothing written; load again."); return; }
  if(!saveBytes("prev",did,live,n)) { editorError(500,"Could not save the pre-write backup. Nothing written."); return; }
  command("unlock");
  if(!unlocked) { editorError(502,"Unlock failed. Nothing written; no retry. Check the CAN log."); return; }
  if(!writeDid(did,next,n)) { stopDiagnostics(); editorError(502,"Write unconfirmed. Read the DID before any further write."); return; }
  if(!readDid(did,live,&liveSize) || liveSize!=n || memcmp(live,next,n)) { stopDiagnostics(); editorError(502,"Write verification failed. Read the DID before any further write."); return; }
  stopDiagnostics();
  // Permit another local edit based on this verified readback, without another read.
  memcpy(editorBase[idx],live,n); editorToken[idx]=esp_random()|1;
  say("WRITE VERIFIED for graphical DID %02X. Background keepalive remains off.\n",did); dump(did,live,n);
  web.send(200,"application/json",String("{\"verified\":true,\"token\":\"")+String(editorToken[idx],HEX)+"\",\"data\":\""+hexBytes(live,n)+"\"}");
}
static void ipcSetup() {
  pinMode(IPC_CS,OUTPUT); pinMode(CAN_INT,INPUT);
  Serial.begin(SERIAL_BAUD_RATE); delay(1000);
  logBoot=esp_random();
  nvsReady=storage.begin("corsa-ipc",false);
  WiFi.mode(WIFI_AP); WiFi.softAP(AP_NAME,AP_PASSWORD);
  web.on("/",HTTP_GET,[](){ web.send_P(200,"text/html",WEB_UI); });
  web.on("/api/log",HTTP_GET,serveLogs);
  web.on("/api/options/catalogue",HTTP_GET,[](){ web.send_P(200,"application/json",OPTION_CATALOGUE); });
  web.on("/api/options/load",HTTP_POST,serveEditorLoad);
  web.on("/api/options/save",HTTP_POST,serveEditorSave);
  web.on("/api/command",HTTP_POST,[](){
    // Browser requests must come from this AP page; no cross-origin write endpoint.
    String origin=web.header("Origin");
    if(origin.length() && origin!="http://"+WiFi.softAPIP().toString()) { web.send(403,"text/plain","Origin rejected"); return; }
    String cmd=web.arg("cmd");
    if(cmd.length()>180) { web.send(400,"text/plain","Command too long"); return; }
    say("> %s\n",cmd.c_str());
    output=""; capture=true; command(cmd); capture=false; web.send(200,"text/plain",output);
  });
  const char *headers[]={"Origin"}; web.collectHeaders(headers,1);
  web.begin();
  initCan();
  say("Quiet diagnostics: no implicit tester keepalive. Use unlock for one immediate seed/key exchange, stop to allow sleep.\n");
  say("Corsa E IPC. CAN=%s NVS=%s. AP %s password %s; http://%s\n",canReady?"ready":"FAILED",nvsReady?"ready":"FAILED",AP_NAME,AP_PASSWORD,WiFi.softAPIP().toString().c_str()); help();
}
static void ipcLoop() {
  static String line; static bool overflow=false;
  while(Serial.available()) {
    char ch=Serial.read();
    if(ch=='\n') { if(!overflow) { say("> %s\n",line.c_str()); command(line); } else say("ERROR: line too long.\n"); line=""; overflow=false; }
    else if(ch!='\r') { if(line.length()<180) line+=ch; else overflow=true; }
  }
  if(seedPending && millis()-seedAt>30000) seedPending=false;
  // Explicit keepalive must not keep an unattended or nonresponsive IPC awake.
  if(session && millis()-lastUserRequest>=30000) {
    stopDiagnostics(); say("30 s without a diagnostic command: keepalive stopped; local security cleared.\n");
  }
  if(session && millis()-lastActivity>=1000) {
    uint8_t q[]={0x3E},e[]={0x7E};
    if(!simple(q,1,e,1)) { session=false; revoke(); say("Session keepalive failed. Reconnect before writing.\n"); }
  }
  // No request is active here: log unsolicited/late replies without treating them
  // as the response to the next diagnostic request.
  if(canReady) {
    uint8_t data[8],len; unsigned long id;
    if(!digitalRead(CAN_INT))
      if(can.readMsgBuf(&id,&len,data)==CAN_OK) logFrame("RX-IDLE",id,len,data);
  }
  web.handleClient();
}
