// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// bthread - A M:N threading library to make applications more concurrent.

// Date: Tue Jul 10 17:40:58 CST 2012

#ifndef BTHREAD_WORK_STEALING_QUEUE_H
#define BTHREAD_WORK_STEALING_QUEUE_H

#include "butil/macros.h"
#include "butil/atomicops.h"
#include "butil/logging.h"

namespace bthread {

template <typename T>
class WorkStealingQueue {
public:
    WorkStealingQueue()
        : _bottom(1)
        , _capacity(0)
        , _buffer(NULL)
        , _top(1) {
    }

    ~WorkStealingQueue() {
        delete [] _buffer;
        _buffer = NULL;
    }

    int init(size_t capacity) {
        if (_capacity != 0) {
            LOG(ERROR) << "Already initialized";
            return -1;
        }
        if (capacity == 0) {
            LOG(ERROR) << "Invalid capacity=" << capacity;
            return -1;
        }
        if (capacity & (capacity - 1)) {
            LOG(ERROR) << "Invalid capacity=" << capacity
                       << " which must be power of 2";
            return -1;
        }
        _buffer = new(std::nothrow) T[capacity];
        if (NULL == _buffer) {
            return -1;
        }
        _capacity = capacity;
        return 0;
    }

    // Push an item into the queue.
    // Returns true on pushed.
    // May run in parallel with steal().
    // Never run in parallel with pop() or another push().
    // 本地工作线程往队列**尾部**push和pop队列数据
    bool push(const T& x) {
        const size_t b = _bottom.load(butil::memory_order_relaxed);
        // steal函数会修改_top值，此处使用memory_order_acquire，是为了保证肯定能获取到steal中的修改
        const size_t t = _top.load(butil::memory_order_acquire);
        if (b >= t + _capacity) { // Full queue.
            return false;
        }
        // 相当于取余，第一个元素放在了下标为1的位置
        _buffer[b & (_capacity - 1)] = x;
        // store和load的默认级别是：memory_order_release 和 memory_order_acquire
        // bottom值只有push/pop自己这个线程会修改，其他线程steal时都只是读而已
        _bottom.store(b + 1, butil::memory_order_release);
        return true;
    }

    // Pop an item from the queue.
    // Returns true on popped and the item is written to `val'.
    // May run in parallel with steal().
    // Never run in parallel with push() or another pop().
    bool pop(T* val) {
        const size_t b = _bottom.load(butil::memory_order_relaxed);
        size_t t = _top.load(butil::memory_order_relaxed);
        if (t >= b) {
            // fast check since we call pop() in each sched.
            // Stale _top which is smaller should not enter this branch.
            return false;
        }
        const size_t newb = b - 1;
        _bottom.store(newb, butil::memory_order_relaxed);
        // memory_order_seq_cst 保证操作顺讯，不会重排，steal会去修改top，所以我们要保证下面的top获取到的是最新值
        butil::atomic_thread_fence(butil::memory_order_seq_cst);
        t = _top.load(butil::memory_order_relaxed);
        if (t > newb) {
            // 说明steal已经从top将元素取出，此处不能pop
            _bottom.store(b, butil::memory_order_relaxed);
            return false;
        }
        // 取出元素
        *val = _buffer[newb & (_capacity - 1)];
        // 若相等则存在单个元素竞争的情况，要进行下一步判断
        // 若不等则至少有两个元素，最多又只有一个steal，所以肯定可以return true
        if (t != newb) {
            return true;
        }
        // 此处 t == newb，即只剩下一个元素了，我们要防止steal偷取，怎么办呢？
        // 我们直接让top加一，可以保证 top > newb，偷取不了
        // Single last element, compete with steal()
        const bool popped = _top.compare_exchange_strong(
            t, t + 1, butil::memory_order_seq_cst, butil::memory_order_relaxed);
        // 不管上面poped成不成功，我们都需要调整_bottom
        _bottom.store(b, butil::memory_order_relaxed);
        return popped;
    }

    // Steal one item from the queue.
    // Returns true on stolen.
    // May run in parallel with push() pop() or another steal().
    // steal不是工作线程调用，而是其他线程调用，会和push和pop存在竞争
    // 但是steal是调整的top，push和pop调整bottom，相对来说竞争小
    bool steal(T* val) {
        // 此处 memory_order_acquire 确保，push和pop操作可见性
        size_t t = _top.load(butil::memory_order_acquire);
        size_t b = _bottom.load(butil::memory_order_acquire);
        if (t >= b) {
            // Permit false negative for performance considerations.
            return false;
        }
        do {
            butil::atomic_thread_fence(butil::memory_order_seq_cst);
            b = _bottom.load(butil::memory_order_acquire);
            // 此处判断的时候，有可能还是 t = b - 1,但是马上pop的时候，b就减1了，此时就出现
            // t == b，然后我们此处steal要和pop争抢最后一个元素
            if (t >= b) {
                return false;
            }
            *val = _buffer[t & (_capacity - 1)];
        } while (!_top.compare_exchange_strong(t, t + 1,
                                               butil::memory_order_seq_cst,
                                               butil::memory_order_relaxed));
        return true;
    }

    size_t volatile_size() const {
        const size_t b = _bottom.load(butil::memory_order_relaxed);
        const size_t t = _top.load(butil::memory_order_relaxed);
        return (b <= t ? 0 : (b - t));
    }

    size_t capacity() const { return _capacity; }

private:
    // Copying a concurrent structure makes no sense.
    DISALLOW_COPY_AND_ASSIGN(WorkStealingQueue);

    butil::atomic<size_t> _bottom;
    size_t _capacity;
    T* _buffer;
    butil::atomic<size_t> BAIDU_CACHELINE_ALIGNMENT _top;
};

}  // namespace bthread

#endif  // BTHREAD_WORK_STEALING_QUEUE_H
