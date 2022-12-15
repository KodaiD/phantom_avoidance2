#pragma once

#include <bits/stdint-uintn.h>
#include <iostream>
#include <set>
#include <string_view>
#include <vector>
#include <map>

#include "../../include/fileio.hh"
#include "../../include/procedure.hh"
#include "../../include/result.hh"
#include "../../include/string.hh"
#include "common.hh"
#include "log.hh"
#include "silo_op_element.hh"
#include "tuple.hh"
#include "array_index.hh"

class TxnExecutor {
 public:
  TxnExecutor(int thid, Result* sres, ArrayIndex& array_index, std::multimap<uint32_t, Tuple*>& gc_list);
  void begin();
  void abort();
  void read(uint64_t key);
  bool upsert(uint64_t key, std::string_view val = "");
  void delete_if_exists(uint64_t key);
  bool range_scan(uint64_t lkey, uint64_t rkey);
  bool validation_phase();
  void write_phase();

  std::vector<Procedure> pro_set;

 private:
  bool before_lock_phase();
  void lock_write_set();
  void unlock_write_set();
  WriteElement<Tuple>* search_write_set(uint64_t key);
  ReadElement<Tuple>* search_read_set(uint64_t key);
  
  std::vector<ReadElement<Tuple>> read_set_;
  std::vector<WriteElement<Tuple>> write_set_;
  NodeSet node_set_;
  ArrayIndex &array_index_;
  std::multimap<uint32_t, Tuple*>& gc_list_;
  uint32_t thid_;
  Result* sres_;
  Tidword mrctid_, max_rset_, max_wset_;
  char write_val_[VAL_SIZE];
  char return_val_[VAL_SIZE]; // used by fast approach for benchmark
};
