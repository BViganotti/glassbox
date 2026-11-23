// Copyright (c) 2026 Bruno Viganotti
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "glassbox/analysis.hpp"

namespace gbx {
namespace {

constexpr uint32_t kInvalid = std::numeric_limits<uint32_t>::max();
constexpr int kMaxIterations = 1000;

struct Graph {
  size_t node_count = 0;
  size_t root = 0;  // synthetic root index == node_count
  std::vector<std::vector<uint32_t>> succ;
  std::vector<std::vector<uint32_t>> pred;
  std::vector<uint32_t> postorder;      // node -> postorder number
  std::vector<uint32_t> rpo;            // root-first
};

/// Iterative DFS producing reverse postorder. Nodes unreachable from the
/// synthetic root are attached as roots so that every node is analyzed.
void dfs_collect(const Graph& graph, uint32_t start, std::vector<uint8_t>& visited,
                 std::vector<uint32_t>& post) {
  std::vector<std::pair<uint32_t, size_t>> stack;
  stack.emplace_back(start, 0);
  visited[start] = 1;
  while (!stack.empty()) {
    auto& entry = stack.back();
    const uint32_t node = entry.first;
    if (entry.second < graph.succ[node].size()) {
      const uint32_t child = graph.succ[node][entry.second++];
      if (!visited[child]) {
        visited[child] = 1;
        stack.emplace_back(child, 0);
      }
    } else {
      post.push_back(node);
      stack.pop_back();
    }
  }
}

void build_reachability(Graph& graph) {
  const size_t n = graph.node_count + 1;
  const uint32_t root = static_cast<uint32_t>(graph.node_count);
  std::vector<uint8_t> visited(n, 0);
  std::vector<uint32_t> scratch;
  dfs_collect(graph, root, visited, scratch);
  bool attached = false;
  for (uint32_t node = 0; node < graph.node_count; ++node) {
    if (visited[node]) continue;
    graph.succ[root].push_back(node);
    graph.pred[node].push_back(root);
    attached = true;
  }
  std::vector<uint32_t> post;
  if (attached) {
    std::fill(visited.begin(), visited.end(), 0);
    dfs_collect(graph, root, visited, post);
  } else {
    post = std::move(scratch);
  }
  graph.postorder.assign(n, 0);
  for (size_t i = 0; i < post.size(); ++i) {
    graph.postorder[post[i]] = static_cast<uint32_t>(i);
  }
  graph.rpo.assign(post.rbegin(), post.rend());
}

uint32_t intersect(const Graph& graph, const std::vector<uint32_t>& idom,
                   uint32_t a, uint32_t b) {
  uint32_t guard = 0;
  while (a != b && guard++ < graph.node_count + 2) {
    while (graph.postorder[a] < graph.postorder[b]) {
      const uint32_t next = idom[a];
      if (next == kInvalid || next == a) break;
      a = next;
    }
    while (graph.postorder[b] < graph.postorder[a]) {
      const uint32_t next = idom[b];
      if (next == kInvalid || next == b) break;
      b = next;
    }
    if (idom[a] == kInvalid || idom[b] == kInvalid) break;
  }
  return a;
}

}  // namespace

HeapGraph build_heap_graph(const Trace& trace, uint64_t at,
                           const HeapGraphOptions& options) {
  HeapGraph result;
  const size_t max_nodes = std::max<size_t>(options.max_nodes, 1);
  auto live = live_allocations(trace, at, max_nodes + 1);
  if (live.size() > max_nodes) {
    result.truncated = true;
    live.resize(max_nodes);
  }
  const size_t count = live.size();
  result.nodes.reserve(count);
  std::unordered_map<uint64_t, uint32_t> index_of;
  index_of.reserve(count * 2);
  for (size_t i = 0; i < count; ++i) {
    HeapGraphNode node;
    node.ptr = live[i].ptr;
    node.size = live[i].size;
    node.owner = live[i].owner;
    node.site = live[i].site;
    node.klass = live[i].klass;
    node.retained = live[i].size;
    result.total_bytes += live[i].size;
    index_of.emplace(node.ptr, static_cast<uint32_t>(i));
    result.nodes.push_back(std::move(node));
  }

  Graph graph;
  graph.node_count = count;
  graph.root = count;
  graph.succ.assign(count + 1, {});
  graph.pred.assign(count + 1, {});
  const size_t max_edges = std::max<size_t>(options.max_edges, 1);
  size_t edge_count = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint64_t owner = live[i].owner;
    auto it = owner == 0 ? index_of.end() : index_of.find(owner);
    if (it == index_of.end()) {
      if (edge_count >= max_edges) {
        result.truncated = true;
        continue;
      }
      graph.succ[graph.root].push_back(static_cast<uint32_t>(i));
      graph.pred[i].push_back(static_cast<uint32_t>(graph.root));
      ++edge_count;
    } else if (it->second != i) {
      if (edge_count >= max_edges) {
        result.truncated = true;
        continue;
      }
      graph.succ[it->second].push_back(static_cast<uint32_t>(i));
      graph.pred[i].push_back(it->second);
      ++edge_count;
    }
  }

  if (count == 0) {
    result.converged = true;
    return result;
  }

  build_reachability(graph);
  std::vector<uint32_t> idom(count + 1, kInvalid);
  idom[graph.root] = graph.root;
  bool changed = true;
  int iterations = 0;
  while (changed && iterations < kMaxIterations) {
    changed = false;
    ++iterations;
    for (size_t i = 1; i < graph.rpo.size(); ++i) {
      const uint32_t node = graph.rpo[i];
      uint32_t new_idom = kInvalid;
      for (uint32_t pred : graph.pred[node]) {
        if (idom[pred] == kInvalid) continue;
        new_idom =
            new_idom == kInvalid ? pred : intersect(graph, idom, pred, new_idom);
      }
      if (new_idom != kInvalid && idom[node] != new_idom) {
        idom[node] = new_idom;
        changed = true;
      }
    }
  }
  if (changed) {
    result.converged = false;
  }

  // Map dominators back to result nodes and accumulate retained sizes bottom-up
  // in reverse reverse-postorder (children before parents in the dom tree).
  for (size_t i = 0; i < count; ++i) {
    const uint32_t parent = idom[i];
    if (parent == kInvalid || parent == graph.root) {
      result.nodes[i].idom = -1;
    } else {
      result.nodes[i].idom = static_cast<int32_t>(parent);
    }
  }
  result.roots = 0;
  for (size_t i = 0; i < count; ++i) {
    const int32_t parent = result.nodes[i].idom;
    if (parent >= 0) {
      result.nodes[static_cast<size_t>(parent)].children.push_back(
          static_cast<uint32_t>(i));
    } else {
      ++result.roots;
    }
  }
  for (auto it = graph.rpo.rbegin(); it != graph.rpo.rend(); ++it) {
    const uint32_t node = *it;
    if (node == graph.root) continue;
    const uint32_t parent = idom[node];
    if (parent == kInvalid || parent == graph.root || parent == node) continue;
    result.nodes[parent].retained += result.nodes[node].retained;
  }
  return result;
}

}  // namespace gbx
