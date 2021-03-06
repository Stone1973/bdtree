/*
 * (C) Copyright 2015 ETH Zurich Systems Group (http://www.systems.ethz.ch/) and others.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Contributors:
 *     Markus Pilman <mpilman@inf.ethz.ch>
 *     Simon Loesing <sloesing@inf.ethz.ch>
 *     Thomas Etter <etterth@gmail.com>
 *     Kevin Bocksrocker <kevin.bocksrocker@gmail.com>
 *     Lucas Braun <braunl@inf.ethz.ch>
 */
#pragma once

#include <bdtree/config.h>

#include "logical_table_cache.h"
#include "search_operation.h"
#include "split_operation.h"
#include "merge_operation.h"

namespace bdtree {

template<typename Key, typename Value>
struct leaf_operation_base {
    bool consolidated = false;

    template <typename NodeTable>
    void cleanup(NodeTable& node_table, const std::vector<physical_pointer>& ptrs) {
        if (!consolidated) return;
        for (physical_pointer ptr : ptrs) {
            node_table.remove(ptr);
        }
    }
};

template<typename Key, typename Value, typename Compare = key_compare<Key, Value> >
struct insert_operation : public leaf_operation_base<Key, Value> {
    const Key& key;
    const Value& value;
    Compare comp;

    insert_operation(const Key& key, const Value& value, Compare comp)
        : key(key), value(value), comp(comp)
    {}

    bool has_conflicts(leaf_node<Key, Value>* leafp) {
        return std::binary_search(leafp->array_.begin(), leafp->array_.end(), key, comp);
    }

    std::vector<uint8_t> operator() (const node_pointer<Key, Value>* nptr, leaf_node<Key, Value>& ln, physical_pointer pptr) {
        auto iter = std::lower_bound(ln.array_.begin(), ln.array_.end(), key, comp);
        //prevent the exact same entry from being inserted twice, just rewrite the same record
        if (iter == ln.array_.end()
                || iter->second != value
                || iter->first != key) {
            ln.array_.insert(iter, std::make_pair(key, value));
        }
        auto leafp = nptr->as_leaf();
        auto deltasize = leafp->deltas_.size();
        if (deltasize + 1 >= CONSOLIDATE_AT) {
            ln.deltas_.clear();
            ln.leaf_pptr_ = pptr;
            this->consolidated = true;
            return ln.serialize();
        } else {
            this->consolidated = false;
            insert_delta<Key, Value> ins_delta;
            ins_delta.value = std::make_pair(key, value);
            ins_delta.next = nptr->ptr_;
            ln.deltas_.insert(ln.deltas_.begin(), pptr);
            return ins_delta.serialize();
        }
    }
};

template<typename Key, typename Value, typename Compare = key_compare<Key, Value> >
struct delete_operation : public leaf_operation_base<Key, Value> {
    const Key& key;
    Compare comp;

    delete_operation(const Key& key, Compare comp)
        : key(key), comp(comp)
    {}

    bool has_conflicts(leaf_node<Key, Value>* leafp) {
        return !std::binary_search(leafp->array_.begin(), leafp->array_.end(), key, comp);
    }

    std::vector<uint8_t> operator() (const node_pointer<Key, Value>* nptr, leaf_node<Key, Value>& ln, physical_pointer pptr) {
        auto iter = std::lower_bound(ln.array_.begin(), ln.array_.end(), key, comp);
        assert(iter->first == key);
        ln.array_.erase(iter);
        auto leafp = nptr->as_leaf();
        if (leafp->deltas_.size() + 1 >= CONSOLIDATE_AT) {
            this->consolidated = true;
            ln.deltas_.clear();
            ln.leaf_pptr_ = pptr;
            return ln.serialize();
        } else {
            this->consolidated = false;
            delete_delta<Key, Value> del_delta;
            del_delta.key = key;
            del_delta.next = nptr->ptr_;
            ln.deltas_.insert(ln.deltas_.begin(), pptr);
            return del_delta.serialize();
        }
    }
};

template<typename Key, typename Value, typename Backend, typename Operation>
bool exec_leaf_operation(const Key& key, Backend& backend, logical_table_cache<Key, Value, Backend>& cache, uint64_t tx_id,
        Operation op) {
    // find the insert/erase candidate
    auto leaf = lower_node_bound(key, backend, cache, tx_id);
    std::size_t nsize = leaf.first->as_leaf()->serialized_size();
    if (nsize >= MAX_NODE_SIZE) {
        split_operation<Key, Value, Backend>::split(leaf.first, leaf.second);
        return exec_leaf_operation(key, backend, cache, tx_id, op);
    } else if (nsize < MIN_NODE_SIZE
               && !(leaf.first->as_leaf()->low_key_ == null_key<Key>::value() && !leaf.first->as_leaf()->high_key_)) {
        merge_operation<Key, Value, Backend>::merge(leaf.first, leaf.second);
        return exec_leaf_operation(key, backend, cache, tx_id, op);
    }
    auto context = std::move(leaf.second);
    auto& node_table = context.get_node_table();
    auto& ptr_table = context.get_ptr_table();
    // create new leaf node for cache (and may be for consolidation)
    for (;;) {
        leaf_node<Key, Value>* leafp = leaf.first->as_leaf();
        if (op.has_conflicts(leafp)) {
            return false;
        }

        auto pptr = node_table.get_next_ptr();
        std::unique_ptr<leaf_node<Key, Value>> lnptr(new leaf_node<Key, Value>(pptr));
        *lnptr = *leafp;

        // create and get the serialized delta node or consolidated node
        std::vector<uint8_t> data = op(leaf.first, *lnptr, pptr);
        node_table.insert(pptr, reinterpret_cast<const char*>(data.data()), uint32_t(data.size()));

        // do the compare and swap
        std::error_code ec;
        auto new_version = ptr_table.update(leaf.first->lptr_, pptr, leaf.first->rc_version_, ec);
        if (!ec) {
            node_pointer<Key, Value>* nnp = new node_pointer<Key, Value>(leaf.first->lptr_, pptr, new_version);
            nnp->node_ = lnptr.release();
            if (!cache.add_entry(nnp, tx_id)) {
                delete nnp;
            }
            std::vector<physical_pointer> ptrs = leafp->deltas_;
            ptrs.push_back(leafp->leaf_pptr_);
            // do the cleanup if consolidated
            op.cleanup(node_table, ptrs);
            return true;
        } else if (ec == error::object_doesnt_exist) {
            context.cache.invalidate(leaf.first->lptr_);
        } else if (ec != error::wrong_version) {
            throw std::system_error(ec);
        }
        node_table.remove(pptr);
        leaf.first = lower_bound_node_with_context(key, context, search_bound::LAST_SMALLER_EQUAL, cache_use::None);
    }
    assert(false);
    return true;
}

}
