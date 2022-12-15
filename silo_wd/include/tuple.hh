#pragma once

#include <bits/stdint-uintn.h>
#include <memory>
#include <pthread.h>
#include <string.h>

#include <cstdint>

#include "../../include/atomic_wrapper.hh"
#include "../../include/cache_line_size.hh"
#include "wait_die_lock.hh"

struct Tidword {
  union {
    uint64_t obj_;
    struct {
      bool lock: 1;
      bool latest: 1;
      bool absent: 1;
      uint64_t tid: 29;
      uint64_t epoch: 32;
    };
  };

  Tidword() : obj_(0) {};
  bool operator==(const Tidword &right) const { return obj_ == right.obj_; }
  bool operator!=(const Tidword &right) const { return !operator==(right); }
  bool operator<(const Tidword &right) const { return this->obj_ < right.obj_; }
};

class Tuple {
 private:
  void promote_waiters() {
    if (waiters->empty()) return;
    auto waiter = waiters->front();
    owner = waiter->ts;
    no_owner = false;
    Tidword desired = tidword_;
    desired.lock = 1;
    storeRelease(tidword_.obj_, desired.obj_);
    waiters->pop();
    storeRelease(waiter->waiting, false);
  }

  Timestamp owner;
  bool no_owner;
  WaitDieList* waiters;
  RWLock latch;

 public:
  alignas(CACHE_LINE_SIZE) Tidword tidword_;
  alignas(CACHE_LINE_SIZE) bool absent = true;
  char val_[VAL_SIZE];

  void init() {
    owner = Timestamp(UINT32_MAX, 0);
    no_owner = true;
    waiters = new WaitDieList;
    latch.init();
  }

  bool try_lock(Timestamp ts, WaitDieNode* wn, bool have_no_lock) {
    wn->ts = ts;
    wn->waiting = true;
    while (!latch.w_trylock()) usleep(1);
    if (no_owner && waiters->empty()) {
      owner = ts;
      no_owner = false;
      Tidword tw = tidword_;
      tw.lock = 1;
      storeRelease(tidword_.obj_, tw.obj_);
      latch.w_unlock();
      return true;
    }
    if (have_no_lock) {
      waiters->insert(ts, wn);
      latch.w_unlock();
      while (loadAcquire(wn->waiting))
        ;
      return true;
    }
    if (ts < owner) {
      waiters->insert(ts, wn);
      latch.w_unlock();
      while (loadAcquire(wn->waiting))
        ;
      return true;
    }
    latch.w_unlock();
    return false;
  }

  void unlock() {
    while (!latch.w_trylock()) usleep(1);
    no_owner = true;
    Tidword desired = tidword_;
    desired.lock = 0;
    storeRelease(tidword_.obj_, desired.obj_);
    promote_waiters();
    latch.w_unlock();
  }
};
