#ifndef BVH_H
#define BVH_H

#include <cstdint>
#include <functional>
#include <cassert>
#include <chrono>
#include <iostream>

#include "common.h"
#include "float4.h"
#include "bbox.h"

struct Tri {
    float3 v0, v1, v2;

    Tri() {}
    Tri(const float3& v0, const float3& v1, const float3& v2)
        : v0(v0), v1(v1), v2(v2)
    {}

    float3& operator[] (int i) { return i == 0 ? v0 : (i == 1 ? v1 : v2); }
    const float3& operator[] (int i) const { return i == 0 ? v0 : (i == 1 ? v1 : v2); }

    float area() const { return length(cross(v1 - v0, v2 - v0)) / 2; }

    /// Computes the triangle bounding box.
    void compute_bbox(BBox& bb) const {
        bb.min = min(v0, min(v1, v2));
        bb.max = max(v0, max(v1, v2));
    }

    /// Splits the triangle along one axis and returns the resulting two bounding boxes.
    void compute_split(BBox& left_bb, BBox& right_bb, int axis, float split) const {
        left_bb = BBox::empty();
        right_bb = BBox::empty();

        const float3& e0 = v1 - v0;
        const float3& e1 = v2 - v1;
        const float3& e2 = v0 - v2;

        const bool left0 = v0[axis] <= split;
        const bool left1 = v1[axis] <= split;
        const bool left2 = v2[axis] <= split;

        if (left0) left_bb.extend(v0);
        if (left1) left_bb.extend(v1);
        if (left2) left_bb.extend(v2);

        if (!left0) right_bb.extend(v0);
        if (!left1) right_bb.extend(v1);
        if (!left2) right_bb.extend(v2);

        if (left0 ^ left1) {
            const float3& p = clip_edge(axis, split, v0, e0);
            left_bb.extend(p);
            right_bb.extend(p);
        }
        if (left1 ^ left2) {
            const float3& p = clip_edge(axis, split, v1, e1);
            left_bb.extend(p);
            right_bb.extend(p);
        }
        if (left2 ^ left0) {
            const float3& p = clip_edge(axis, split, v2, e2);
            left_bb.extend(p);
            right_bb.extend(p);
        }
    }

private:
    static float3 clip_edge(int axis, float plane, const float3& p, const float3& edge) {
        const float t = (plane - p[axis]) / (edge[axis]);
        return p + t * edge;
    }
};

template <typename Allocator = std::allocator<uint8_t> >
class MemoryPool {
public:
    ~MemoryPool() {
        cleanup();
    }

    template <typename T>
    T* alloc(size_t count) {
        size_t size = count * sizeof(T);
        chunks_.emplace_back(alloc_.allocate(size), size);
        return reinterpret_cast<T*>(chunks_.back().first);
    }

    void cleanup() {
        for (auto chunk: chunks_)
            alloc_.deallocate(chunk.first, chunk.second);
        chunks_.clear();
    }

private:
    typedef std::pair<uint8_t*, size_t> Chunk;

    std::vector<Chunk> chunks_;
    Allocator alloc_;
};

template <typename Node, int N>
struct MultiNode {
    Node nodes[N];
    BBox bbox;
    int count;

    MultiNode(const Node& node) {
        nodes[0] = node;
        bbox = node.bbox;
        count = 1;
    }

    bool is_full() const { return count == N; }
    bool is_leaf() const { return count == 1; }

    void sort_nodes() {
        std::sort(nodes, nodes + count, [] (const Node& a, const Node& b) {
            return a.size() < b.size();
        });
    }

    int next_node() const {
        assert(node_available());
        if (N == 2)
            return 0;
        else {
            float min_cost = FLT_MAX;
            int min_idx = 0;
            for (int i = 0; i < count; i++) {
                if (!nodes[i].tested && min_cost > nodes[i].cost) {
                    min_idx = i;
                    min_cost = nodes[i].cost;
                }
            }
            return min_idx;
        }
    }

    bool node_available() const {
        for (int i = 0; i < count; i++) {
            if (!nodes[i].tested) return true;
        }
        return false;
    }

    void split_node(int i, const Node& left, const Node& right) {
        assert(count < N);
        nodes[i] = left;
        nodes[count++] = right;
    }
};

template <typename T, int N = 128>
struct Stack {
    static constexpr int capacity() { return N; }

    T elems[N];
    int top;

    Stack() : top(-1) {}

    template <typename... Args>
    void push(Args... args) {
        assert(!is_full());
        elems[++top] = T(args...);
    }

    T pop() {
        assert(!is_empty());
        return elems[top--];
    }

    bool is_empty() const { return top < 0; }
    bool is_full() const { return top >= N - 1; }
    int size() const { return top + 1; }
};

/// Builds a SBVH (Spatial split BVH), given the set of triangles and the alpha parameter
/// that controls when to do a spatial split. The tree is built in depth-first order.
/// See  Stich et al., "Spatial Splits in Bounding Volume Hierarchies", 2009
/// http://www.nvidia.com/docs/IO/77714/sbvh.pdf
template <int N, typename CostFn>
class SplitBvhBuilder {
public:
    template <typename NodeWriter, typename LeafWriter>
    void build(const std::vector<Tri>& tris, NodeWriter write_node, LeafWriter write_leaf, int leaf_threshold, float alpha = 1e-5f) {
        assert(leaf_threshold >= 1);

#ifdef STATISTICS
        total_tris_ += tris.size();
        auto time_start = std::chrono::high_resolution_clock::now();
#endif

        const int tri_count = tris.size();

        Ref* initial_refs = mem_pool_.alloc<Ref>(tri_count);
        right_bbs_ = mem_pool_.alloc<BBox>(std::max((int)spatial_bins, tri_count));
        BBox mesh_bb = BBox::empty();
        for (int i = 0; i < tri_count; i++) {
            const Tri& tri = tris[i];
            tri.compute_bbox(initial_refs[i].bb);
            mesh_bb.extend(initial_refs[i].bb);
            initial_refs[i].id = i;
        }

        const float spatial_threshold = mesh_bb.half_area() * alpha;

        Stack<Node> stack;
        stack.push(initial_refs, tri_count, mesh_bb);

        while (!stack.is_empty()) {
            MultiNode<Node, N> multi_node(stack.pop());

            // Iterate over the available split candidates in the multi-node
            while (!multi_node.is_full() && multi_node.node_available()) {
                const int node_id = multi_node.next_node();
                Node node = multi_node.nodes[node_id];
                Ref* refs = node.refs;
                int ref_count = node.ref_count;
                const BBox& parent_bb = node.bbox;
                assert(ref_count != 0);

                if (ref_count <= leaf_threshold) {
                    // This candidate does not have enough triangles
                    multi_node.nodes[node_id].tested = true;
                    continue;
                }

                // Try object splits
                ObjectSplit object_split;
                for (int axis = 0; axis < 3; axis++)
                    find_object_split(object_split, axis, refs, ref_count);

                SpatialSplit spatial_split;
                if (BBox(object_split.left_bb).overlap(object_split.right_bb).half_area() > spatial_threshold) {
                    // Try spatial splits
                    for (int axis = 0; axis < 3; axis++) {
                        if (parent_bb.min[axis] == parent_bb.max[axis])
                            continue;
                        find_spatial_split(spatial_split, parent_bb, tris, axis, refs, ref_count);
                    }
                }

                bool spatial = spatial_split.cost < object_split.cost;
                const float split_cost = spatial ? spatial_split.cost : object_split.cost;

                if (split_cost + CostFn::traversal_cost(parent_bb.half_area()) >= node.cost) {
                    // Split is not beneficial
                    multi_node.nodes[node_id].tested = true;
                    continue;
                }

                if (spatial) {
                    Ref* left_refs, *right_refs;
                    BBox left_bb, right_bb;
                    int left_count, right_count;
                    apply_spatial_split(spatial_split, tris,
                                        refs, ref_count,
                                        left_refs, left_count, left_bb,
                                        right_refs, right_count, right_bb);

                    multi_node.split_node(node_id,
                                          Node(left_refs,  left_count,  left_bb),
                                          Node(right_refs, right_count, right_bb));

#ifdef STATISTICS
                    spatial_splits_++;
#endif
                } else {
                    // Partitioning can be done in-place
                    apply_object_split(object_split, refs, ref_count);

                    const int right_count = ref_count - object_split.left_count;
                    const int left_count = object_split.left_count;

                    Ref *right_refs = refs + object_split.left_count;
                    Ref* left_refs = refs;

                    multi_node.split_node(node_id,
                                          Node(left_refs,  left_count,  object_split.left_bb),
                                          Node(right_refs, right_count, object_split.right_bb));
#ifdef STATISTICS
                    object_splits_++;
#endif
                }
            }

            assert(multi_node.count > 0);
            // Process the smallest nodes first
            multi_node.sort_nodes();

            // The multi-node is ready to be stored
            if (multi_node.is_leaf()) {
                // Store a leaf if it could not be split
                const Node& node = multi_node.nodes[0];
                assert(node.tested);
                make_leaf(node, write_leaf);
            } else {
                // Store a multi-node
                make_node(multi_node, write_node);
                assert(N > 2 || multi_node.count == 2);

                if (stack.size() + multi_node.count < stack.capacity()) {
                    for (int i = multi_node.count - 1; i >= 0; i--) {
                        stack.push(multi_node.nodes[i]);
                    }
                } else {
                    // Insufficient space on the stack, we have to stop recursion here
                    for (int i = 0; i < multi_node.count; i++) {
                        make_leaf(multi_node.nodes[i], write_leaf);
                    }
                }
            }
        }

#ifdef STATISTICS
        auto time_end = std::chrono::high_resolution_clock::now();
        total_time_ += std::chrono::duration_cast<std::chrono::milliseconds>(time_end - time_start).count();
#endif

        mem_pool_.cleanup();
    }

#ifdef STATISTICS
    void print_stats() const {
        std::cout << "BVH built in " << total_time_ << "ms ("
                  << total_nodes_ << " nodes, "
                  << total_leaves_ << " leaves, "
                  << object_splits_ << " object splits, "
                  << spatial_splits_ << " spatial splits, "
                  << "+" << (total_refs_ - total_tris_) * 100  / total_tris_ << "% references)"
                  << std::endl;
    }
#endif

private:
    static constexpr int spatial_bins = 64;
    static constexpr int binning_passes = 2;

    struct Ref {
        uint32_t id;
        BBox bb;

        Ref() {}
        Ref(uint32_t id, const BBox& bb) : id(id), bb(bb) {}
    };

    struct Bin {
        BBox bb;
        int entry;
        int exit;
    };

    struct ObjectSplit {
        int axis;
        float cost;
        BBox left_bb, right_bb;
        int left_count;

        ObjectSplit() : cost (FLT_MAX) {}
    };

    struct SpatialSplit {
        int axis;
        float cost;
        float position;

        SpatialSplit() : cost (FLT_MAX) {}
    };

    struct Node {
        Ref* refs;
        int ref_count;
        BBox bbox;
        float cost;
        bool tested;

        Node() {}
        Node(Ref* refs, int ref_count, const BBox& bbox)
            : refs(refs), ref_count(ref_count), bbox(bbox)
            , cost(CostFn::leaf_cost(ref_count, bbox.half_area()))
            , tested(false)
        {}

        int size() const { return ref_count; }
    };

    template <typename NodeWriter>
    void make_node(const MultiNode<Node, N>& multi_node, NodeWriter write_node) {
        write_node(multi_node.bbox, multi_node.count, [&] (int i) {
            return multi_node.nodes[i].bbox;
        });
#ifdef STATISTICS
        total_nodes_++;
#endif
    }

    template <typename LeafWriter>
    void make_leaf(const Node& node, LeafWriter write_leaf) {
        write_leaf(node.bbox, node.ref_count, [&] (int i) {
            return node.refs[i].id;
        });
#ifdef STATISTICS
        total_leaves_++;
        total_refs_ += node.ref_count;
#endif
    }

    void sort_refs(int axis, Ref* refs, int ref_count) {
        // Sort the primitives based on their centroids
        std::sort(refs, refs + ref_count, [axis] (const Ref& a, const Ref& b) {
            const float ca = a.bb.min[axis] + a.bb.max[axis];
            const float cb = b.bb.min[axis] + b.bb.max[axis];
            return (ca < cb) || (ca == cb && a.id < b.id);
        });
    }

    void find_object_split(ObjectSplit& split, int axis, Ref* refs, int ref_count) {
        assert(ref_count > 0);

        sort_refs(axis, refs, ref_count);

        // Sweep from the right and accumulate the bounding boxes
        BBox cur_bb = BBox::empty();
        for (int i = ref_count - 1; i > 0; i--) {
            cur_bb.extend(refs[i].bb);
            right_bbs_[i - 1] = cur_bb;
        }

        // Sweep from the left and compute the SAH cost
        cur_bb = BBox::empty();
        for (int i = 0; i < ref_count - 1; i++) {
            cur_bb.extend(refs[i].bb);
            const float cost = CostFn::leaf_cost(i + 1, cur_bb.half_area()) + CostFn::leaf_cost(ref_count - i - 1, right_bbs_[i].half_area());
            if (cost < split.cost) {
                split.axis = axis;
                split.cost = cost;
                split.left_count = i + 1;
                split.left_bb = cur_bb;
                split.right_bb = right_bbs_[i];
            }
        }

        assert(split.left_count != 0 && split.left_count != ref_count);
    }

    void apply_object_split(const ObjectSplit& split, Ref* refs, int ref_count) {
        sort_refs(split.axis, refs, ref_count);
    }

    int spatial_binning(Bin* bins, int num_bins, SpatialSplit& split,
                        const std::vector<Tri>& tris, int axis,
                        Ref* refs, int ref_count,
                        float axis_min, float axis_max) {
        // Initialize bins
        for (int i = 0; i < num_bins; i++) {
            bins[i].entry = 0;
            bins[i].exit = 0;
            bins[i].bb = BBox::empty();
        }

        // Put the primitives in the bins
        const float bin_size = (axis_max - axis_min) / num_bins;
        const float inv_size = 1.0f / bin_size;
        for (int i = 0; i < ref_count; i++) {
            const Ref& ref = refs[i];

            const int first_bin = clamp(int(inv_size * (ref.bb.min[axis] - axis_min)), 0, num_bins - 1);
            const int last_bin  = clamp(int(inv_size * (ref.bb.max[axis] - axis_min)), 0, num_bins - 1);

            BBox cur_bb = ref.bb;
            for (int j = first_bin; j < last_bin; j++) {
                BBox left_bb, right_bb;
                tris[ref.id].compute_split(left_bb, right_bb, axis, j < num_bins - 1 ? axis_min + (j + 1) * bin_size : axis_max);
                bins[j].bb.extend(left_bb.overlap(cur_bb));
                cur_bb.overlap(right_bb);
            }

            bins[last_bin].bb.extend(cur_bb);
            bins[first_bin].entry++;
            bins[last_bin].exit++;
        }

        // Sweep from the right and accumulate the bounding boxes
        BBox cur_bb = BBox::empty();
        for (int i = num_bins - 1; i > 0; i--) {
            cur_bb.extend(bins[i].bb);
            right_bbs_[i - 1] = cur_bb;
        }

        // Sweep from the left and compute the SAH cost
        int left_count = 0, right_count = ref_count;
        cur_bb = BBox::empty();

        int split_index = -1;
        for (int i = 0; i < num_bins - 1; i++) {
            left_count += bins[i].entry;
            right_count -= bins[i].exit;
            cur_bb.extend(bins[i].bb);

            const float cost = CostFn::leaf_cost(left_count, cur_bb.half_area()) + CostFn::leaf_cost(right_count, right_bbs_[i].half_area());
            if (cost < split.cost) {
                split.axis = axis;
                split.cost = cost;
                split.position = axis_min + (i + 1) * bin_size;
                split_index = i;
            }
        }
        return split_index;
    }

    void find_spatial_split(SpatialSplit& split, const BBox& parent_bb,
                            const std::vector<Tri>& tris, int axis,
                            Ref* refs, int ref_count) {
        float axis_min = parent_bb.min[axis];
        float axis_max = parent_bb.max[axis];
        assert(axis_max > axis_min);
        Bin bins[spatial_bins];
        int n = 0;

        do {
            if (axis_max <= axis_min) break;

            int split_index = spatial_binning(bins, spatial_bins, split, tris, axis, refs, ref_count, axis_min, axis_max);
            if (split_index < 0) break;

            float bin_size = (axis_max - axis_min) / spatial_bins;
            axis_min = split.position - bin_size;
            axis_max = split.position + bin_size;
            n++;
        } while (n < binning_passes);
    }

    void apply_spatial_split(const SpatialSplit& split,
                             const std::vector<Tri>& tris,
                             Ref* refs, int ref_count,
                             Ref*& left_refs, int& left_count, BBox& left_bb,
                             Ref*& right_refs, int& right_count, BBox& right_bb) {
        // Split the reference array in three parts:
        // [0.. left_count[ : references that are completely on the left
        // [left_count.. first_right[ : references that lie in between
        // [first_right.. ref_count[ : references that are completely on the right
        int first_right = ref_count;
        int cur_ref = 0;

        left_count = 0;
        left_bb = BBox::empty();
        right_bb = BBox::empty();

        while (cur_ref < first_right) {
            if (refs[cur_ref].bb.max[split.axis] <= split.position) {
                left_bb.extend(refs[cur_ref].bb);
                std::swap(refs[cur_ref++], refs[left_count++]);
            } else if (refs[cur_ref].bb.min[split.axis] >= split.position) {
                right_bb.extend(refs[cur_ref].bb);
                std::swap(refs[cur_ref], refs[--first_right]);
            } else {
                cur_ref++;
            }
        }

        right_count = ref_count - first_right;

        // Handle straddling references
        std::vector<Ref> dup_refs;
        while (left_count < first_right) {
            const Ref& ref = refs[left_count];
            BBox left_split_bb, right_split_bb;
            tris[ref.id].compute_split(left_split_bb, right_split_bb, split.axis, split.position);
            left_split_bb.overlap(ref.bb);
            right_split_bb.overlap(ref.bb);

            const BBox left_unsplit_bb  = BBox(ref.bb).extend(left_bb);
            const BBox right_unsplit_bb = BBox(ref.bb).extend(right_bb);
            const BBox left_dup_bb  = BBox(left_split_bb).extend(left_bb);
            const BBox right_dup_bb = BBox(right_split_bb).extend(right_bb);

            const float left_unsplit_area  = left_unsplit_bb.half_area();
            const float right_unsplit_area = right_unsplit_bb.half_area();
            const float left_dup_area  = left_dup_bb.half_area();
            const float right_dup_area = right_dup_bb.half_area();

            // Compute the cost of unsplitting to the left and the right
            const float unsplit_left_cost  = CostFn::leaf_cost(left_count + 1, left_unsplit_area)   + CostFn::leaf_cost(right_count,     right_bb.half_area());
            const float unsplit_right_cost = CostFn::leaf_cost(left_count,     left_bb.half_area()) + CostFn::leaf_cost(right_count + 1, right_unsplit_area);
            const float dup_cost           = CostFn::leaf_cost(left_count + 1, left_dup_area)       + CostFn::leaf_cost(right_count + 1, right_dup_area);

            const float min_cost = std::min(dup_cost, std::min(unsplit_left_cost, unsplit_right_cost));

            if (min_cost == unsplit_left_cost) {
                // Unsplit to the left
                left_bb = left_unsplit_bb;
                left_count++;
            } else if (min_cost == unsplit_right_cost) {
                // Unsplit to the right
                right_bb = right_unsplit_bb;
                std::swap(refs[--first_right], refs[left_count]);
                right_count++;
            } else {
                // Duplicate
                left_bb = left_dup_bb;
                right_bb = right_dup_bb;
                refs[left_count].bb = left_split_bb;
                dup_refs.emplace_back(refs[left_count].id, right_split_bb);
                left_count++;
                right_count++;
            }
        }

        if (dup_refs.size() == 0) {
            // We can reuse the original arrays
            left_refs = refs;
            right_refs = refs + left_count;
        } else {
            // We need to reallocate a new array for the right child
            left_refs = refs;
            right_refs = mem_pool_.alloc<Ref>(right_count);
            std::copy(refs + first_right, refs + ref_count, right_refs + dup_refs.size());
            std::copy(dup_refs.begin(), dup_refs.end(), right_refs);
        }

        assert(left_count != 0 && right_count != 0);
        assert(!left_bb.is_empty() && !right_bb.is_empty());
    }


#ifdef STATISTICS
    long total_time_ = 0;
    int total_nodes_ = 0;
    int total_leaves_ = 0;
    int total_refs_ = 0;
    int total_tris_ = 0;
    int spatial_splits_ = 0;
    int object_splits_ = 0;
#endif

    BBox* right_bbs_;
    MemoryPool<> mem_pool_;
};

#endif // BVH_H
