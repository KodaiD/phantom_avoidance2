#pragma once

#include "../../include/atomic_wrapper.hh"
#include "tuple.hh"
#include "wait_die_lock.hh"
#include <bits/stdint-uintn.h>
#include <sys/types.h>
#include <unordered_map>

#define NUM_ARRAY_NODES 15

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

  Tuple* records[NUM_ARRAY_NODES];
  WaitDieLock wdlock;
};

enum NodeLockMode : uint8_t {
  READER,
  WRITER,
};

struct NodeSetElement {
  NodeSetElement(Node *node, NodeLockMode mode) : node(node), mode(mode) {}
  Node *node;
  NodeLockMode mode;
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

  ~ArrayIndex() {
    delete[] nodes;
  }

  Node* get_node(uint64_t key) {
    int node_no = int(key / NUM_ARRAY_NODES);
    return &nodes[node_no];
  }

  Tuple* get_tuple(uint64_t key) {
    int node_no = int(key / NUM_ARRAY_NODES);
    int slot_no = key % NUM_ARRAY_NODES;
    return nodes[node_no].records[slot_no];
  }

  Node* nodes;
};
