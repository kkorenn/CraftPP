// java.util.HashSet<BlockPos> with Java's iteration order (HashMap: power-of-two table, hash ^ (hash >>> 16),
// tail-appended bucket chains, order-preserving lo/hi split on resize, resize at 0.75 load). Vanilla trees
// iterate HashSets (leaf distance flood fill, decorator lists), so the order is part of the output.
// ponytail: no treeified bins (>= 8 collisions in one bucket of a >= 64 table); warns once if it would happen.
#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>

namespace worldgen {

template <class K, class Hash>  // Hash(k) -> Java hashCode()
class JavaHashSet {
 public:
  size_t size() const { return count; }
  bool empty() const { return count == 0; }
  bool contains(const K& k) const {
    if (table.empty()) return false;
    for (int i = table[index(spread(Hash()(k)))]; i >= 0; i = nodes[size_t(i)].next)
      if (nodes[size_t(i)].key == k) return true;
    return false;
  }
  bool insert(const K& k) {
    if (table.empty()) resize();
    uint32_t h = spread(Hash()(k));
    size_t bin = index(h);
    int prev = -1, chain = 0;
    for (int i = table[bin]; i >= 0; i = nodes[size_t(i)].next) {
      if (nodes[size_t(i)].key == k) return false;
      prev = i, chain++;
    }
    int n = alloc(k, h);  // may reallocate nodes
    (prev < 0 ? table[bin] : nodes[size_t(prev)].next) = n;
    if (chain >= 8) {  // binCount >= TREEIFY_THRESHOLD - 1: resize small tables, treeify large ones
      if (table.size() < 64) resize();
      else warn();
    }
    if (++count > table.size() * 3 / 4) resize();
    return true;
  }
  bool erase(const K& k) {
    if (table.empty()) return false;
    for (int* link = &table[index(spread(Hash()(k)))]; *link >= 0; link = &nodes[size_t(*link)].next)
      if (nodes[size_t(*link)].key == k) {
        int dead = *link;
        *link = nodes[size_t(dead)].next;
        nodes[size_t(dead)].next = free_list, free_list = dead;
        count--;
        return true;
      }
    return false;
  }
  // Iteration order snapshot.
  std::vector<K> items() const {
    std::vector<K> out;
    out.reserve(count);
    for (int head : table)
      for (int i = head; i >= 0; i = nodes[size_t(i)].next) out.push_back(nodes[size_t(i)].key);
    return out;
  }
  // iterator().next() + remove(): the first element in iteration order.
  K pop_first() {
    for (int& head : table)
      if (head >= 0) {
        K k = nodes[size_t(head)].key;
        int dead = head;
        head = nodes[size_t(dead)].next;
        nodes[size_t(dead)].next = free_list, free_list = dead;
        count--;
        return k;
      }
    return K{};
  }

 private:
  struct Node {
    K key;
    uint32_t hash;
    int next;
  };
  std::vector<Node> nodes;
  std::vector<int> table;
  size_t count = 0;
  int free_list = -1;

  static uint32_t spread(int32_t h) { return uint32_t(h) ^ (uint32_t(h) >> 16); }
  size_t index(uint32_t h) const { return h & (table.size() - 1); }
  int alloc(const K& k, uint32_t h) {
    if (free_list >= 0) {
      int i = free_list;
      free_list = nodes[size_t(i)].next;
      nodes[size_t(i)] = {k, h, -1};
      return i;
    }
    nodes.push_back({k, h, -1});
    return int(nodes.size() - 1);
  }
  void resize() {
    size_t old = table.size(), cap = old ? old * 2 : 16;
    std::vector<int> t(cap, -1);
    for (size_t j = 0; j < old; j++) {
      int loH = -1, loT = -1, hiH = -1, hiT = -1;
      for (int i = table[j]; i >= 0;) {
        int next = nodes[size_t(i)].next;
        nodes[size_t(i)].next = -1;
        bool hi = nodes[size_t(i)].hash & old;
        int& h = hi ? hiH : loH;
        int& tl = hi ? hiT : loT;
        if (tl < 0) h = i;
        else nodes[size_t(tl)].next = i;
        tl = i;
        i = next;
      }
      t[j] = loH;
      t[j + old] = hiH;
    }
    table.swap(t);
  }
  static void warn() {
    static bool once = false;
    if (!once) once = true, fprintf(stderr, "worldgen: JavaHashSet bin would treeify; iteration order may differ\n");
  }
};

}  // namespace worldgen
