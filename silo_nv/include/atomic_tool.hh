#pragma once

#include "common.hh"

#include "../../include/inline.hh"
#include <bits/stdint-uintn.h>

INLINE uint64_t atomicLoadGE();

INLINE uint64_t atomicAddGE() {
  uint64_t expected, desired;

  expected = atomicLoadGE();
  for (;;) {
    desired = expected + 1;
    if (__atomic_compare_exchange_n(&(GlobalEpoch.obj_), &expected, desired,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      break;
  }
  return desired;
}

INLINE uint64_t atomicLoadGE() {
  uint64_t_64byte result =
          __atomic_load_n(&(GlobalEpoch.obj_), __ATOMIC_ACQUIRE);
  return result.obj_;
}

INLINE uint64_t atomicLoadRE() {
  uint64_t_64byte result =
          __atomic_load_n(&(ReclamationEpoch.obj_), __ATOMIC_ACQUIRE);
  return result.obj_;
}

INLINE void atomicStoreThLocalEpoch(unsigned int thid, uint64_t newval) {
  __atomic_store_n(&(ThLocalEpoch[thid].obj_), newval, __ATOMIC_RELEASE);
}

INLINE void atomicStoreReclamationEpoch(uint64_t newval) {
  __atomic_store_n(&(ReclamationEpoch.obj_), newval, __ATOMIC_RELEASE);
}
