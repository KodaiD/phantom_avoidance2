#include <bits/stdint-uintn.h>
#include <cstdint>
#include <ctype.h>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>

#include "boost/filesystem.hpp"
#include "include/tuple.hh"
#include "include/wait_die_lock.hh"

#define GLOBAL_VALUE_DEFINE

#include "include/atomic_tool.hh"
#include "include/common.hh"
#include "include/result.hh"
#include "include/transaction.hh"
#include "include/util.hh"

#include "../include/atomic_wrapper.hh"
#include "../include/backoff.hh"
#include "../include/cpu.hh"
#include "../include/debug.hh"
#include "../include/fileio.hh"
#include "../include/masstree_wrapper.hh"
#include "../include/random.hh"
#include "../include/result.hh"
#include "../include/tsc.hh"
#include "../include/util.hh"
#include "../include/zipf.hh"

using namespace std;

void worker(int thid, char& ready, const bool& start, const bool& quit, ArrayIndex& array_index) {
  thread_local std::multimap<uint32_t, Tuple*> gc_list;
  Result &myres = std::ref(SiloResult[thid]);
  Xoroshiro128Plus rnd;
  rnd.init();
  TxnExecutor trans(thid, (Result *) &myres, array_index, gc_list);
  FastZipf zipf(&rnd, FLAGS_zipf_skew, FLAGS_tuple_num);
  uint64_t epoch_timer_start, epoch_timer_stop;
#if Linux
  setThreadAffinity(thid);
#endif
  storeRelease(ready, 1);
  while (!loadAcquire(start)) _mm_pause();
  if (thid == 0) epoch_timer_start = rdtscp();

  while (!loadAcquire(quit)) {
    makeProcedure2(trans.pro_set, rnd, zipf, FLAGS_thread_num, thid, FLAGS_tuple_num, \
        FLAGS_max_ope, FLAGS_rratio, FLAGS_range_size, FLAGS_priority, myres);
    trans.ts = Timestamp(thid, 0);

#if PROCEDURE_SORT
    sort(trans.pro_set_.begin(), trans.pro_set_.end());
#endif
RETRY:
    if (thid == 0) leaderWork(epoch_timer_start, epoch_timer_stop);
    if (loadAcquire(quit)) break;

    trans.begin();
    for (auto itr = trans.pro_set.begin(); itr != trans.pro_set.end(); ++itr) {
      switch (itr->ope_) {
        case Ope::READ:
          if (!trans.read(itr->key_)) {
            trans.unlock_node_set();
            trans.abort();
            ++myres.local_abort_counts_;
            ++myres.local_abort_by_read;
            goto RETRY;
          }
          break;
        case Ope::UPSERT:
          if (!trans.upsert(itr->key_)) {
            trans.unlock_node_set();
            trans.abort();
            ++myres.local_abort_counts_;
            ++myres.local_abort_by_upsert;
            goto RETRY;
          }
          break;
        case Ope::DELETE_IF_EXISTS:
          if (!trans.delete_if_exists(itr->key_)) {
            trans.unlock_node_set();
            trans.abort();
            ++myres.local_abort_counts_;
            ++myres.local_abort_by_delete;
            goto RETRY;
          }
          break;
        case Ope::RANGE_SCAN:
          if (!trans.range_scan(itr->key_, itr->key_+FLAGS_range_size)) {
            trans.unlock_node_set();
            trans.abort();
            ++myres.local_abort_counts_;
            ++myres.local_abort_by_range;
            goto RETRY;
          }
          break;
        default:
          ERR;
      }
    }
    if (trans.validation_phase()) {
      trans.write_phase();
      storeRelease(myres.local_commit_counts_, loadAcquire(myres.local_commit_counts_) + 1);
      for (auto itr = trans.pro_set.begin(); itr != trans.pro_set.end(); ++itr) {
        switch (itr->ope_) {
          case Ope::READ:
            ++myres.local_read_commit_counts_;
            break;
          case Ope::UPSERT:
            ++myres.local_upsert_commit_counts_;
            break;
          case Ope::DELETE_IF_EXISTS:
            ++myres.local_delete_commit_counts_;
            break;
          case Ope::RANGE_SCAN:
            ++myres.local_range_commit_counts_;
            break;
        }
      }
    } else {
      trans.abort();
      ++myres.local_abort_counts_;
      switch (trans.pro_set.front().ope_) {
      case Ope::READ:
        ++myres.local_abort_by_read;
        break;
      case Ope::UPSERT:
        ++myres.local_abort_by_upsert;
        break;
      case Ope::DELETE_IF_EXISTS:
        ++myres.local_abort_by_delete;
        break;
      case Ope::RANGE_SCAN:
        ++myres.local_abort_by_range;
        break;
      }
      goto RETRY;
    }
  }
  return;
}

int main(int argc, char *argv[]) try {
  gflags::SetUsageMessage("Silo benchmark.");
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  chkArg();
  makeDB();
  Xoroshiro128Plus rnd;
  rnd.init();

  Result final_result;

  for (int n = 0; n < FLAGS_num_execution; n++) {
    ThreadLocalWaiters = new WaitDieNode[FLAGS_thread_num];
    ThreadLocalTupleWaiters = new WaitDieNode[FLAGS_thread_num];
    ThreadLocalOwners = new std::vector<WaitDieNode*>[FLAGS_thread_num];
    GlobalOwnerPool = new std::vector<WaitDieNode*>*[FLAGS_thread_num];

    for (int i = 0; i < FLAGS_thread_num; i++) {
      for (int j = 0; j < FLAGS_max_ope * FLAGS_range_size; j++) {
        ThreadLocalOwners[i].emplace_back(new WaitDieNode());
      }
      GlobalOwnerPool[i] = &(ThreadLocalOwners[i]);
    }

    ArrayIndex array_index = ArrayIndex(FLAGS_tuple_num);
    for (int i = 0; i < FLAGS_tuple_num; ++i) {
      Tuple *tmp;
      tmp = &Table[i];
      tmp->tidword_.epoch = 1;
      if (rnd.next() % 100 < 50) {
        tmp->tidword_.absent = 1;
      } else {
        tmp->tidword_.absent = 0;
      }
      tmp->tidword_.latest = 1;
      tmp->tidword_.lock = 0;
      if (rnd.next() % 100 < 50) {
        tmp->absent = true;
      } else {
        tmp->absent = false;
      }
      tmp->val_[0] = 'a';
      tmp->val_[1] = '\0';
      tmp->init();
      int node_no = int(i / NUM_ARRAY_NODES);
      int slot_no = i % NUM_ARRAY_NODES;
      array_index.nodes[node_no].records[slot_no] = tmp;
    }

    alignas(CACHE_LINE_SIZE) bool start = false;
    alignas(CACHE_LINE_SIZE) bool quit = false;
    initResult();
    std::vector<char> readys(FLAGS_thread_num);
    std::vector<std::thread> thv;
    for (size_t i = 0; i < FLAGS_thread_num; ++i)
      thv.emplace_back(worker, i, std::ref(readys[i]), std::ref(start),
                       std::ref(quit), std::ref(array_index));
    waitForReady(readys);
    storeRelease(start, true);
    for (size_t i = 0; i < FLAGS_extime; ++i) {
      sleepMs(1000);
    }
    storeRelease(quit, true);
    for (auto &th : thv) th.join();

    // std::cout << atomicLoadGE() << std::endl;

    for (unsigned int i = 0; i < FLAGS_thread_num; ++i) {
      SiloResult[0].addLocalAllResult(SiloResult[i]);
    }

    final_result.total_upsert_commit_counts_ += SiloResult[0].total_upsert_commit_counts_;
    final_result.total_delete_commit_counts_ += SiloResult[0].total_delete_commit_counts_;
    final_result.total_range_commit_counts_ += SiloResult[0].total_range_commit_counts_;
    final_result.total_commit_counts_ += SiloResult[0].total_commit_counts_;
    final_result.total_upsert_abort_counts_ += SiloResult[0].total_upsert_abort_counts_;
    final_result.total_delete_abort_counts_ += SiloResult[0].total_delete_abort_counts_;
    final_result.total_range_abort_counts_ += SiloResult[0].total_range_abort_counts_;
    final_result.total_abort_counts_ += SiloResult[0].total_abort_counts_;

    // ShowOptParameters();
    // SiloResult[0].displayAllResult(FLAGS_clocks_per_us, FLAGS_extime,
    //                                FLAGS_thread_num);
    // SiloResult[0].displayTpsAndAbortRatioPerOps(FLAGS_extime, FLAGS_thread_num);
    // SiloResult[0].displayCommitAbortCount(FLAGS_thread_num);
    // SiloResult[0].displayCommitCountPerOps();
    // SiloResult[0].displayCauseOfAbort();

    delete[] ThreadLocalWaiters;
    delete[] ThreadLocalTupleWaiters;
    delete[] ThreadLocalOwners;
    delete[] GlobalOwnerPool;
  }

  final_result.total_upsert_commit_counts_ /= FLAGS_num_execution;
  final_result.total_delete_commit_counts_ /= FLAGS_num_execution;
  final_result.total_range_commit_counts_ /= FLAGS_num_execution;
  final_result.total_commit_counts_ /= FLAGS_num_execution;
  final_result.total_upsert_abort_counts_ /= FLAGS_num_execution;
  final_result.total_delete_abort_counts_ /= FLAGS_num_execution;
  final_result.total_range_abort_counts_ /= FLAGS_num_execution;
  final_result.total_abort_counts_ /= FLAGS_num_execution;

  final_result.displayTpsAndAbortRatioPerOps(FLAGS_extime, FLAGS_thread_num);
  return 0;
} catch (bad_alloc) {
  ERR;
}

