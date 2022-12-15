#pragma once

#include "../../include/atomic_wrapper.hh"
#include "../../include/int64byte.hh"
#include "../../include/rwlock.hh"

#include <algorithm>
#include <bits/stdint-uintn.h>
#include <stdexcept>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <list>
#include <memory>
#include <cassert>
#include <iostream>
#include <vector>

struct Timestamp {
  union {
    uint64_t obj_;
    struct {
      uint32_t thid_;
      uint32_t priority_;
    };
  };
  Timestamp() : obj_(0) {}
  Timestamp(uint32_t thid, uint32_t priority) : thid_(thid), priority_(priority) {}
  bool operator==(const Timestamp &right) const { return obj_ == right.obj_; }
  bool operator!=(const Timestamp &right) const { return !operator==(right); }
  bool operator<(const Timestamp &right) const { return this->obj_ < right.obj_; }
};

enum Operation : uint8_t {
  INVALID,
  SHARED,
  EXCLUSIVE,
  UPGRADE
};

struct WaitDieNode {
  using TS = Timestamp;
  WaitDieNode() {}
  WaitDieNode(TS ts) : ts(ts) {}
  WaitDieNode(TS ts, Operation op, bool waiting)
    : waiting(waiting), ts(ts), op(op) {}
  void init(TS timestamp) {
    ts = timestamp;
  }
  alignas(CACHE_LINE_SIZE) bool waiting;
  char pad[CACHE_LINE_SIZE - sizeof(bool)] = {};
  TS ts;
  Operation op;
  WaitDieNode* next = nullptr;
  WaitDieNode* prev = nullptr;
};

#ifdef GLOBAL_OWNER_DEFINE
#define GLOBAL_OWNER
alignas(CACHE_LINE_SIZE) GLOBAL_OWNER std::vector<WaitDieNode*>** GlobalOwnerPool;
#else
#define GLOBAL_OWNER extern
alignas(CACHE_LINE_SIZE) GLOBAL_OWNER std::vector<WaitDieNode*>** GlobalOwnerPool;
#endif

struct WaitDieList {
 private:
  using TS = Timestamp;
  uint64_t size = 0;
  WaitDieNode* head = nullptr;
  WaitDieNode* tail = nullptr;
 public:
  Operation op = INVALID;
  WaitDieList() { clear(); }
  void clear() {
    size = 0;
    head = nullptr;
    tail = nullptr;
  }
  void insert(TS ts, WaitDieNode* new_item) {
    new_item->ts = ts;
    WaitDieNode* cur;
    for (cur = head; cur != nullptr; cur = cur->next) {
      if (cur->ts == ts) throw std::runtime_error("cannot insert owner: same timestamp");
      if (cur->ts < ts) {
        if (cur == head) {
          cur->prev = new_item;
          new_item->next = cur;
          head = new_item;
          size++;
          return;
        }
        cur->prev->next = new_item;
        new_item->prev = cur->prev;
        new_item->next = cur;
        cur->prev = new_item;
        new_item->next = cur;
        size++;
        if (!new_item->next) tail = new_item;
        return;
      }
    }
    if (!head) {
      head = new_item;
      tail = new_item;
      size++;
      return;
    }
    tail->next = new_item;
    new_item->prev = tail;
    tail = new_item;
    size++;
    return;
  }
  WaitDieNode* remove(TS ts) {
    auto cur = head;
    // if (!cur) return nullptr;
    if (!cur) throw std::runtime_error("cannot remove owner: not found");
    if (ts == cur->ts) {
      if (!cur->next) {
        head = nullptr;
        tail = nullptr;
        size--;
        op = INVALID;
        return cur;
      }
      cur->next->prev = nullptr;
      head = cur->next;
      cur->next = nullptr;
      size--;
      return cur;
    }
    for (cur = cur->next; cur != tail; cur = cur->next) {
      if (cur->ts == ts) {
        cur->prev->next = cur->next;
        cur->next->prev = cur->prev;
        cur->next = nullptr;
        cur->prev = nullptr;
        size--;
        return cur;
      }
    }
    if (ts == cur->ts) {
      cur->prev->next = nullptr;
      tail = cur->prev;
      cur->prev = nullptr;
      size--;
      return cur;
    }
    throw std::runtime_error("cannot remove owner: not found");
  }
  WaitDieNode* front() { return head; }
  void pop() {
    auto cur = head;
    if (!cur) return;
    if (!cur->next) {
      head = nullptr;
      tail = nullptr;
      size--;
      op = INVALID;
      return;
    }
    cur->next->prev = nullptr;
    head = cur->next;
    cur->next = nullptr;
    size--;
    return;
  }
  TS get_min_timestamp() { return tail->ts; }
  uint64_t get_size() { return size; }
  bool empty() { return (get_size() == 0); }
};

class WaitDieLock {
 private:
   using TS = Timestamp;
   alignas(CACHE_LINE_SIZE) RWLock latch;
   alignas(CACHE_LINE_SIZE) RWLock lock;
   WaitDieList waiter_list;
   WaitDieList owner_list;
 public:
  WaitDieLock() { latch.init(); }

  bool try_lock_shared(Timestamp ts, WaitDieNode* wn, bool have_no_lock) {
    wn->ts = ts;
    wn->op = SHARED;
    wn->waiting = true;
    while (!latch.w_trylock()) usleep(1);
    bool no_waiter = waiter_list.empty();
    Operation op = owner_list.op;
    if ((op == INVALID || op == SHARED) && no_waiter) {
      auto on = get_owner(ts);
      on->ts = ts;
      owner_list.insert(ts, on);
      owner_list.op = SHARED;
      latch.w_unlock();
      return true;
    }
    if (have_no_lock) {
      waiter_list.insert(ts, wn);
      latch.w_unlock();
      while (loadAcquire(wn->waiting))
        ;
      return true;
    }
    assert(op != INVALID);
    // pattern A ==============================================
    if (((op == SHARED) && !no_waiter) || op == EXCLUSIVE) {
      if (ts < owner_list.get_min_timestamp()) {
        waiter_list.insert(ts, wn);
        latch.w_unlock();
        while (loadAcquire(wn->waiting))
          ;
        return true;
      } else {
        latch.w_unlock();
        // usleep(1);
        return false;
      }
    }
    // ========================================================
    // pattern B Bug!! ========================================
    // if ((op == SHARED) && !no_waiter) {
    //   waiter_list.insert(ts, wn);
    //   latch.w_unlock();
    //   while (loadAcquire(wn->waiting))
    //     ;
    //   return true;
    // }
    // if (op == EXCLUSIVE) {
    //   if (ts < owner_list.get_min_timestamp()) {
    //     waiter_list.insert(ts, wn);
    //     latch.w_unlock();
    //     while (loadAcquire(wn->waiting))
    //       ;
    //     return true;
    //   } else {
    //     latch.w_unlock();
    //     // usleep(1);
    //     return false;
    //   }
    // }
    // ========================================================
    // pattern C ==============================================
    // if ((op == SHARED) && !no_waiter) {
    //   auto on = get_owner(ts);
    //   on->ts = ts;
    //   owner_list.insert(ts, on);
    //   owner_list.op = SHARED;
    //   latch.w_unlock();
    //   return true;
    // }
    // if (op == EXCLUSIVE) {
    //   if (ts < owner_list.get_min_timestamp()) {
    //     waiter_list.insert(ts, wn);
    //     latch.w_unlock();
    //     while (loadAcquire(wn->waiting))
    //       ;
    //     return true;
    //   } else {
    //     latch.w_unlock();
    //     // usleep(1);
    //     return false;
    //   }
    // }
    // ========================================================
    throw std::runtime_error("fail to lock shared");
  }

  bool try_lock(Timestamp ts, WaitDieNode* wn, bool have_no_lock) {
    wn->ts = ts;
    wn->op = EXCLUSIVE;
    wn->waiting = true;
    while (!latch.w_trylock()) usleep(1);
    bool no_waiter = waiter_list.empty();
    Operation op = owner_list.op;
    if (op == INVALID && no_waiter) {
      auto on = get_owner(ts);
      owner_list.insert(ts, on);
      owner_list.op = EXCLUSIVE;
      latch.w_unlock();
      return true;
    }
    if (have_no_lock) {
      waiter_list.insert(ts, wn);
      latch.w_unlock();
      while (loadAcquire(wn->waiting))
        ;
      return true;
    }
    assert(op != INVALID);
    if (op == SHARED || op == EXCLUSIVE) {
      if (ts < owner_list.get_min_timestamp()) {
        waiter_list.insert(ts, wn);
        latch.w_unlock();
        while (loadAcquire(wn->waiting))
          ;
        return true;
      } else {
        latch.w_unlock();
        // usleep(1);
        return false;
      }
    }
    throw std::runtime_error("fail to lock");
  }

  bool try_lock_upgrade(Timestamp ts, WaitDieNode* wn) {
    wn->ts = ts;
    wn->op = UPGRADE;
    wn->waiting = true;
    while (!latch.w_trylock()) usleep(1);
    Operation op = owner_list.op;
    if (op == SHARED) {
      TS min_ts = owner_list.get_min_timestamp();
      uint64_t num_owners = owner_list.get_size();
      if (min_ts == ts && num_owners > 1) {
        waiter_list.insert(ts, wn);
        latch.w_unlock();
        while (loadAcquire(wn->waiting))
          ;
        return true;
      } else if (min_ts == ts && num_owners == 1) {
        owner_list.op = EXCLUSIVE;
        latch.w_unlock();
        return true;
      } else {
        latch.w_unlock();
        return false;
      }
    } else {
      throw std::runtime_error("cannot upgrade: wrong lock mode");
    }
  }

  void unlock_shared(Timestamp ts) {
    while (!latch.w_trylock()) usleep(1);
    Operation op = owner_list.op;
    if (op == SHARED) {
      auto on = owner_list.remove(ts);
      return_owner(ts, on);
      promote_waiters();
      latch.w_unlock();
      return;
    }
    throw std::runtime_error("cannot unlock shared: wrong lock mode");
  }

  void unlock(Timestamp ts) {
    while (!latch.w_trylock()) usleep(1);
    Operation op = owner_list.op;
    if (op == EXCLUSIVE) {
      auto on = owner_list.remove(ts);
      return_owner(ts, on);
      promote_waiters();
      latch.w_unlock();
      return;
    }
    throw std::runtime_error("cannot unlock: wrong lock mode");
  }

  WaitDieNode* get_owner(TS ts) {
    auto local_owner_pool = GlobalOwnerPool[ts.thid_];
    for (int i = 0; i < local_owner_pool->size(); i++) {
      if ((*local_owner_pool)[i] != nullptr) {
        auto owner = (*local_owner_pool)[i];
        (*local_owner_pool)[i] = nullptr;
        owner->ts = ts;
        return owner;
      }
    }
    std::runtime_error("ran out of WaitDieNode");
    return nullptr;
  }

  void return_owner(TS ts, WaitDieNode* on) {
    auto local_owner_pool = GlobalOwnerPool[ts.thid_];
    for (int i = 0; i < local_owner_pool->size(); i++) {
      if ((*local_owner_pool)[i] == nullptr) {
        (*local_owner_pool)[i] = on;
        return;
      }
    }
    std::runtime_error("full with WaitDieNode");
  }

  void promote_waiters() {
    Operation o_op;
    Operation w_op;
    uint64_t num_owners;
    while (true) {
      if (waiter_list.get_size() == 0) return;

      o_op = owner_list.op;
      num_owners = owner_list.get_size();
      w_op = waiter_list.front()->op;

      if (w_op == SHARED && o_op == EXCLUSIVE) return;
      if (w_op == EXCLUSIVE && (o_op == SHARED || o_op == EXCLUSIVE)) return;
      if (w_op == UPGRADE && o_op == SHARED && num_owners > 1) return;
      if (w_op == UPGRADE && (o_op == INVALID || o_op == EXCLUSIVE)) {
        throw std::runtime_error("fail to promote");
      }

      if (w_op == SHARED && (o_op == INVALID || o_op == SHARED)) {
        auto waiter = waiter_list.front();
        TS ts = waiter->ts;
        auto on = get_owner(ts);
        owner_list.insert(ts, on);
        owner_list.op = SHARED;
        waiter_list.pop();
        storeRelease(waiter->waiting, false);
        continue;
      } else if (w_op == EXCLUSIVE && o_op == INVALID) {
        auto waiter = waiter_list.front();
        TS ts = waiter->ts;
        auto on = get_owner(ts);
        owner_list.insert(ts, on);
        owner_list.op = EXCLUSIVE;
        waiter_list.pop();
        storeRelease(waiter->waiting, false);
        continue;
      } else if (w_op == UPGRADE && o_op == SHARED && num_owners == 1) {
        auto waiter = waiter_list.front();
        assert(owner_list.get_min_timestamp() == waiter->ts);
        owner_list.op = EXCLUSIVE;
        waiter_list.pop();
        storeRelease(waiter->waiting, false);
        continue;
      }
      throw std::runtime_error("cannot promote waiters: unhandled state");
    }
  }
};
