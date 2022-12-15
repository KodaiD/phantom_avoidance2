#pragma once

#include <atomic>
#include <mutex>

#include "../../include/cache_line_size.hh"
#include "../../include/inline.hh"
#include "wait_die_lock.hh"

using namespace std;

class Tuple {
public:
  WaitDieLock* lock_;
  char val_[VAL_SIZE];
};
