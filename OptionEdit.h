#pragma once
#include <stddef.h>
#include "Options.h"

static constexpr bool validDidLength(uint8_t did,size_t n) {
  return did==0x90 ? n==17 : n>0 && n<=32;
}

// Enforce the catalogue on the device, independently of browser controls.
static constexpr bool validOptionChanges(uint8_t did,const uint8_t *before,const uint8_t *after,size_t n) {
  if(n>32) return false;
  uint8_t allowed[32]={};
  for(const auto &o:OPTIONS)
    if(o.did==did && o.byte<n && !(o.flags&1)) allowed[o.byte]|=uint8_t(1<<o.bit);
  for(size_t byte=0;byte<n;++byte) {
    if((before[byte]^after[byte]) & uint8_t(~allowed[byte])) return false;
  }
  return true;
}
