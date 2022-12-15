#include "include/wait_die_lock.hh"
#include "../include/random.hh"

#include <bits/stdint-uintn.h>
#include <vector>
#include <iostream>
#include <algorithm>
#include <cassert>
#include <iostream>

int main() {
  std::vector<WaitDieNode*> waiters;
  std::vector<WaitDieNode*> owners;
  for (int i = 0; i < 1000; i++) {
    Timestamp ts = Timestamp(i, 0);;  
    waiters.emplace_back(new WaitDieNode(ts, INVALID, false));
    owners.emplace_back(new WaitDieNode(ts));
  } 
  WaitDieList waiter_list;
  WaitDieList owner_list;

  Xoroshiro128Plus rnd;
  rnd.init();

  int rnd_num;
  std::vector<uint32_t> inserted;
  for (int i = 0; i < 1000; ++i) {
     rnd_num = rnd.next() % 1000;
     if (std::find(inserted.begin(), inserted.end(), rnd_num) != inserted.end())
       continue;
     Timestamp ts = Timestamp(rnd_num, 0);
     waiter_list.insert(ts, waiters[rnd_num]);
     owner_list.insert(ts, owners[rnd_num]);
     inserted.emplace_back(rnd_num);
  }
  for (int i = 0; i < inserted.size(); i++) {
    Timestamp ts = Timestamp(inserted[i], 0);
    waiter_list.remove(ts);
    owner_list.remove(ts);
  }

  std::vector<uint32_t> waiters_expected, owners_expected;
  std::vector<uint32_t> check;
  for (auto iter = waiter_list.front(); iter != nullptr; iter = iter->next) {
    waiters_expected.emplace_back(iter->ts.thid_);
    check.emplace_back(iter->ts.thid_);
  }
  std::sort(check.begin(), check.end());
  for (int i = 0; i < waiters_expected.size(); i++) {
    assert(waiters_expected[i] == check[i]);
  }
  std::cout << "PASS: waiter_list test" << std::endl;
  check.clear();
  for (auto iter = owner_list.front(); iter != nullptr; iter = iter->next) {
    owners_expected.emplace_back(iter->ts.thid_);
    check.emplace_back(iter->ts.thid_);
  }
  std::sort(check.begin(), check.end());
  for (int i = 0; i < owners_expected.size(); i++) {
    assert(owners_expected[i] == check[i]);
  }
  std::cout << "PASS: owner_list test" << std::endl;
}
