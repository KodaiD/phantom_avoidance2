#include "../../include/atomic_wrapper.hh"
#include "tuple.hh"
#include <bits/stdint-uintn.h>
#include <sys/types.h>
#include <unordered_map>

#define NUM_ARRAY_NODES 15

union version_t {
  uint16_t obj_;
  struct {
    bool locked : 1;
    uint16_t vinsert : 15;
  };
  version_t() : locked(false), vinsert(0) {}
  bool operator==(const version_t &right) const { return obj_ == right.obj_; }
  bool operator!=(const version_t &right) const { return obj_ != right.obj_; } 
};


struct Node {
  Node() {
    for (int i = 0; i < NUM_ARRAY_NODES; i++) {
      records[i] = nullptr;
    }
  }
  
  Tuple* get_tuple(uint64_t key) {
    int slot_no = key % NUM_ARRAY_NODES;
    return records[slot_no];
  }

  void lock() {
    version_t expected, desired;
    expected.obj_ = loadAcquire(version.obj_);
    while (1) {
      if (!expected.locked) {
        desired = expected;
        desired.locked = 1;
        if (compareExchange(version.obj_, expected.obj_, desired.obj_))
          break;
      }
      expected.obj_ = loadAcquire(version.obj_);
    }
  }

  void unlock() {
    version_t desired;
    desired.obj_ = loadAcquire(version.obj_);
    desired.locked = 0;
    storeRelease(version.obj_, desired.obj_);
  }

  version_t increment_and_unlock() {
    version_t desired;
    desired.obj_ = loadAcquire(version.obj_);
    desired.locked = 0;
    desired.vinsert++;
    storeRelease(version.obj_, desired.obj_);
    return desired;
  }

  Tuple* records[NUM_ARRAY_NODES];
  alignas(CACHE_LINE_SIZE) version_t version;
};


struct NodeSet {
  bool find(Node* node) {
    return data.find(node) != data.end();
  }

  version_t get_version(Node* node) {
    return data[node];
  }

  void add_entry(Node* node, version_t ver) {
    data[node] = ver;
  }

  void update_entry(Node* node, version_t new_ver) {
    auto iter = data.find(node);
    if (iter != data.end()) {
      data[node] = new_ver;
    }
  }

  bool validation() {
    for (auto iter = data.begin(); iter != data.end(); ++iter) {
      Node* node = iter->first;
      version_t expected = iter->second;
      if (loadAcquire(node->version.obj_) != expected.obj_) return false;
    }
    return true;
  }

  void reserve(size_t size) {
    data.reserve(size);
  }

  void clear() {
    data.clear();
  }

  std::unordered_map<Node*, version_t> data;
};


struct ArrayIndex {
  ArrayIndex(uint64_t num_tuples) {
    int num_nodes;
    if (num_tuples % NUM_ARRAY_NODES != 0)
      num_nodes = int(num_tuples / NUM_ARRAY_NODES) + 1;
    else
      num_nodes = int(num_tuples / NUM_ARRAY_NODES);
    nodes = new Node[num_nodes];
  }

  Node* get_node(uint64_t key, version_t& ver) {
    int node_no = int(key / NUM_ARRAY_NODES);
    Node* node = &nodes[node_no];
    version_t expected;
    expected.obj_ = loadAcquire(node->version.obj_);
    while (expected.locked) {
      expected.obj_ = loadAcquire(node->version.obj_);
    }
    ver.obj_ = expected.obj_;
    return node;
  }

  Node* nodes;
};
