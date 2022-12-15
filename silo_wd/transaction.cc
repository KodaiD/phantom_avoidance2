#include <bits/stdint-uintn.h>
#include <cstring>
#include <map>
#include <stdio.h>
#include <algorithm>
#include <string>

#define GLOBAL_OWNER_DEFINE

#include "include/atomic_tool.hh"
#include "include/log.hh"
#include "include/transaction.hh"

extern void displayDB();

TxnExecutor::TxnExecutor(int thid, Result* sres, ArrayIndex& array_index, std::multimap<uint32_t, Tuple*>& gc_list)
  : thid_(thid), sres_(sres), array_index_(array_index), gc_list_(gc_list) {
  pro_set.reserve(FLAGS_max_ope);
  read_set_.reserve(FLAGS_max_ope);
  write_set_.reserve(FLAGS_max_ope);
  node_set_.reserve(FLAGS_max_ope);
  lock_set_.reserve(FLAGS_max_ope);
  max_rset_.obj_ = 0;
  max_wset_.obj_ = 0;
  genStringRepeatedNumber(write_val_, VAL_SIZE, thid);
}

void TxnExecutor::begin() {
  max_wset_.obj_ = 0;
  max_rset_.obj_ = 0;
  bool exist = true;
  uint64_t reclamation_epoch = atomicLoadRE();
  for (auto iter = gc_list_.begin(); iter != gc_list_.end(); ++iter) {
    if (iter->first <= reclamation_epoch) {
      compareExchange(iter->second->absent, exist, false);
    } else {
      break;
    }
  }
}

void TxnExecutor::abort() {
  read_set_.clear();
  write_set_.clear();
  node_set_.clear();
  lock_set_.clear();
  bool exist = true;
  uint64_t reclamation_epoch = atomicLoadRE();
  for (auto iter = gc_list_.begin(); iter != gc_list_.end(); ++iter) {
    if (iter->first <= reclamation_epoch) {
      compareExchange(iter->second->absent, exist, false);
    } else {
      break;
    }
  }
}

bool TxnExecutor::read(uint64_t key) {
  if (search_write_set(key) || search_read_set(key)) return true;
  Node* node = array_index_.get_node(key);
  Tuple* tuple = node->get_tuple(key);
  if (loadAcquire(tuple->absent)) {
    if (!search_node_set(node)) {
      if (!node->wdlock.try_lock_shared(ts, &ThreadLocalWaiters[thid_], false)) {
        sres_->local_abort_by_node_wait_die++;
        return false;
      }
      node_set_.emplace_back(node, NodeLockMode::READER);
    }
    return true;
  }

  Tidword expected, check;
  expected.obj_ = loadAcquire(tuple->tidword_.obj_);
  while (1) {
    while (expected.lock) {
      expected.obj_ = loadAcquire(tuple->tidword_.obj_);
    }
    memcpy(return_val_, tuple->val_, VAL_SIZE);
    check.obj_ = loadAcquire(tuple->tidword_.obj_);
    if (expected == check) break;
    expected = check;
  }
  read_set_.emplace_back(key, tuple, return_val_, expected);
  return true;
}

bool TxnExecutor::upsert(uint64_t key, std::string_view val) {
  if (auto we = search_write_set(key); we) {
    we->wtype = WriteType::UPSERT;
    return true;
  }
  if (auto re = search_read_set(key); re) {
    write_set_.emplace_back(key, re->rcdptr_, val, WriteType::UPSERT, false);
    return true;
  }
  write_set_.emplace_back(key, nullptr, val, WriteType::UPSERT, true);
  return true;
}

bool TxnExecutor::delete_if_exists(uint64_t key) {
  if (auto we = search_write_set(key); we) {
    we->val_length_ = 0;
    we->wtype = WriteType::DELETE_IF_EXISTS;
    return true;
  }
  if (auto re = search_read_set(key); re) {
    write_set_.emplace_back(key, re->rcdptr_, "", WriteType::DELETE_IF_EXISTS, false);
    return true;
  }
  write_set_.emplace_back(key, nullptr, "", WriteType::DELETE_IF_EXISTS, true);
  return true;
}

bool TxnExecutor::range_scan(uint64_t lkey, uint64_t rkey) {
  assert(lkey < 0 || rkey > FLAGS_tuple_num || lkey >= rkey);
  int node_no = int(lkey / NUM_ARRAY_NODES);
  int slot_no = lkey % NUM_ARRAY_NODES;

  Tuple *cur_tuple;
  Tidword expected, check;
  Node* cur_node;
  uint64_t cur_key;

  while (true) {
    cur_node = &array_index_.nodes[node_no];
    if (!search_node_set(cur_node)) {
      if (!cur_node->wdlock.try_lock_shared(ts, &ThreadLocalWaiters[thid_], false)) {
        sres_->local_abort_by_node_wait_die++;
        return false;
      }
      node_set_.emplace_back(cur_node, NodeLockMode::READER);
    }
    while (slot_no < NUM_ARRAY_NODES) {
      cur_key = node_no * NUM_ARRAY_NODES + slot_no;
      if (cur_key >= rkey) break;
      // tuple read =====================================================
      if (search_write_set(cur_key) || search_read_set(cur_key)) {
        slot_no++;
        continue;
      }
      cur_tuple = cur_node->get_tuple(cur_key);
      if (cur_tuple->absent) {
        slot_no++;
        continue;
      }
      expected.obj_ = loadAcquire(cur_tuple->tidword_.obj_);
      while (1) {
        while (expected.lock) {
          expected.obj_ = loadAcquire(cur_tuple->tidword_.obj_);
        }
        memcpy(return_val_, cur_tuple->val_, VAL_SIZE);
        check.obj_ = loadAcquire(cur_tuple->tidword_.obj_);
        if (expected == check) break;
        expected = check;
      }
      // ================================================================
      read_set_.emplace_back(cur_key, cur_tuple, return_val_, expected);
      slot_no++;
    }
    node_no++;
    slot_no = 0;
    if (cur_key >= rkey) break;
  }
  return true;
}

bool TxnExecutor::validation_phase() {
  // printf("%u: validation\n", thid_);
  if (!lock_node_set()) {
    unlock_node_set();
    return false;
  }
  if (!lock_write_set()) {
    unlock_node_set();
    unlock_tuples();
    sres_->local_abort_by_tuple_wait_die++;
    return false;
  }
  // serialization point
  asm volatile("":: : "memory");
  atomicStoreThLocalEpoch(thid_, atomicLoadGE());
  asm volatile("":: : "memory");
  // node reader unlock
  unlock_node_set();
  // read-set validation
  Tidword check;
  for (auto itr = read_set_.begin(); itr != read_set_.end(); ++itr) {
    check.obj_ = loadAcquire(itr->rcdptr_->tidword_.obj_);
    if (itr->get_tidword().epoch != check.epoch ||
        itr->get_tidword().tid != check.tid) {
      // unlock_node_set();
      unlock_tuples();
      sres_->local_abort_by_read_validation++;
      return false;
    }
    if (check.lock && !search_write_set(itr->key_)) {
      // unlock_node_set();
      unlock_tuples();
      sres_->local_abort_by_read_validation++;
      return false;
    }
    max_rset_ = std::max(max_rset_, check);
  }
  return true;
}

void TxnExecutor::write_phase() {
  Tidword tid_a, tid_b, tid_c;
  tid_a = std::max(max_wset_, max_rset_);
  tid_a.tid++;
  tid_b = mrctid_;
  tid_b.tid++;
  tid_c.epoch = ThLocalEpoch[thid_].obj_;
  Tidword maxtid = std::max({tid_a, tid_b, tid_c});
  maxtid.lock = 1; // unlock は後で行う
  maxtid.latest = 1;
  mrctid_ = maxtid;

  for (auto itr = write_set_.begin(); itr != write_set_.end(); ++itr) {
    if (itr->wtype == WriteType::DELETE_IF_EXISTS) {
      if (!itr->rcdptr_) continue;
      maxtid.latest = 0;
      maxtid.absent = 1;
      gc_list_.emplace(ThLocalEpoch[thid_].obj_, itr->rcdptr_);
      storeRelease(itr->rcdptr_->tidword_.obj_, maxtid.obj_);
      continue;
    }
    if (itr->get_val_length() == 0) {
      memcpy(itr->rcdptr_->val_, write_val_, VAL_SIZE); // fast approach for benchmark
    } else {
      memcpy(itr->rcdptr_->val_, itr->get_val_ptr(), itr->get_val_length());
    }
    storeRelease(itr->rcdptr_->tidword_.obj_, maxtid.obj_);
  }

  unlock_tuples();
  read_set_.clear();
  write_set_.clear();
  node_set_.clear();
  lock_set_.clear();
}

bool TxnExecutor::lock_node_set() {
  for (auto itr = write_set_.begin(); itr != write_set_.end(); ++itr) {
    if (!itr->check) continue;
    Node* node = array_index_.get_node(itr->key_);
    Tuple* tuple = node->get_tuple(itr->key_);
    if (itr->wtype == WriteType::UPSERT) {
      if (loadAcquire(tuple->absent)) {
        auto ne = search_node_set(node);
        if (!ne) {
          if (!node->wdlock.try_lock(ts, &ThreadLocalWaiters[thid_], false)) {
            sres_->local_abort_by_node_wait_die++;
            return false;
          }
        } else if (ne->mode == NodeLockMode::READER) {
          if (!node->wdlock.try_lock_upgrade(ts, &ThreadLocalWaiters[thid_])) {
            sres_->local_abort_by_node_wait_die++;
            return false;
          }
          ne->mode = NodeLockMode::WRITER;
        }
        bool exist = true;
        compareExchange(tuple->absent, exist, false);
        // TODO: upgrade の場合は unlock しない
        node->wdlock.unlock(ts);
      }
      itr->rcdptr_ = tuple;
      continue;
    }
    if (itr->wtype == WriteType::DELETE_IF_EXISTS) {
      if (loadAcquire(tuple->absent)) {
        if (!search_node_set(node)) {
          if (!node->wdlock.try_lock_shared(ts, &ThreadLocalWaiters[thid_], false)) {
            sres_->local_abort_by_node_wait_die++;
            return false;
          }
          node_set_.emplace_back(node, NodeLockMode::READER);
        }
        continue;
      }
      itr->rcdptr_ = tuple;
      continue;
    }
  } 
  return true;
}

bool TxnExecutor::lock_write_set() {
  Tidword expected, desired;
  for (auto itr = write_set_.begin(); itr != write_set_.end(); ++itr) {
    if (!itr->rcdptr_ && itr->wtype == WriteType::DELETE_IF_EXISTS) continue;
    if (!search_lock_set(itr->rcdptr_)) {
      if (!itr->rcdptr_->try_lock(ts, &ThreadLocalTupleWaiters[thid_], false)) {
        return false;
      }
      lock_set_.emplace_back(itr->rcdptr_);
    }
    max_wset_ = std::max(max_wset_, expected);
  }
  return true;
}

void TxnExecutor::unlock_tuples() {
  Tidword expected, desired;
  for (auto itr = lock_set_.begin(); itr != lock_set_.end(); ++itr) {
    (*itr)->unlock();
  }
}

void TxnExecutor::unlock_node_set() {
  for (auto itr = node_set_.begin(); itr != node_set_.end(); ++itr) {
    itr->node->wdlock.unlock_shared(ts);
  }
}

ReadElement<Tuple> *TxnExecutor::search_read_set(uint64_t key) {
  for (auto itr = read_set_.begin(); itr != read_set_.end(); ++itr) {
    if ((*itr).key_ == key) return &(*itr);
  }
  return nullptr;
}

WriteElement<Tuple> *TxnExecutor::search_write_set(uint64_t key) {
  for (auto itr = write_set_.begin(); itr != write_set_.end(); ++itr) {
    if ((*itr).key_ == key) return &(*itr);
  }
  return nullptr;
}

NodeSetElement *TxnExecutor::search_node_set(Node* node) {
  for (auto itr = node_set_.begin(); itr != node_set_.end(); ++itr) {
    if (itr->node == node) return &(*itr);
  }
  return nullptr;
}

bool TxnExecutor::search_lock_set(Tuple* tuple) {
  for (auto itr = lock_set_.begin(); itr != lock_set_.end(); ++itr) {
    if ((*itr) == tuple) return true;
  }
  return false;
}
