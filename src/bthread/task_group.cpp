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

#include <sys/types.h>
#include <stddef.h>                         // size_t
#include <gflags/gflags.h>
#include "butil/compat.h"                   // OS_MACOSX
#include "butil/macros.h"                   // ARRAY_SIZE
#include "butil/scoped_lock.h"              // BAIDU_SCOPED_LOCK
#include "butil/fast_rand.h"
#include "butil/unique_ptr.h"
#include "butil/third_party/murmurhash3/murmurhash3.h" // fmix64
#include "bthread/errno.h"                  // ESTOP
#include "bthread/butex.h"                  // butex_*
#include "bthread/sys_futex.h"              // futex_wake_private
#include "bthread/processor.h"              // cpu_relax
#include "bthread/task_control.h"
#include "bthread/task_group.h"
#include "bthread/timer_thread.h"
#include "bthread/errno.h"

namespace bthread {

static const bthread_attr_t BTHREAD_ATTR_TASKGROUP = {
    BTHREAD_STACKTYPE_UNKNOWN, 0, NULL };

static bool pass_bool(const char*, bool) { return true; }

DEFINE_bool(show_bthread_creation_in_vars, false, "When this flags is on, The time "
            "from bthread creation to first run will be recorded and shown "
            "in /vars");
const bool ALLOW_UNUSED dummy_show_bthread_creation_in_vars =
    ::GFLAGS_NS::RegisterFlagValidator(&FLAGS_show_bthread_creation_in_vars,
                                    pass_bool);

DEFINE_bool(show_per_worker_usage_in_vars, false,
            "Show per-worker usage in /vars/bthread_per_worker_usage_<tid>");
const bool ALLOW_UNUSED dummy_show_per_worker_usage_in_vars =
    ::GFLAGS_NS::RegisterFlagValidator(&FLAGS_show_per_worker_usage_in_vars,
                                    pass_bool);

__thread TaskGroup* tls_task_group = NULL;
// Sync with TaskMeta::local_storage when a bthread is created or destroyed.
// During running, the two fields may be inconsistent, use tls_bls as the
// groundtruth.
__thread LocalStorage tls_bls = BTHREAD_LOCAL_STORAGE_INITIALIZER;

// defined in bthread/key.cpp
extern void return_keytable(bthread_keytable_pool_t*, KeyTable*);

// [Hacky] This is a special TLS set by bthread-rpc privately... to save
// overhead of creation keytable, may be removed later.
BAIDU_THREAD_LOCAL void* tls_unique_user_ptr = NULL;

const TaskStatistics EMPTY_STAT = { 0, 0 };

const size_t OFFSET_TABLE[] = {
        // 这里面这些值是哪来的？
#include "bthread/offset_inl.list"
};

int TaskGroup::get_attr(bthread_t tid, bthread_attr_t* out) {
    TaskMeta* const m = address_meta(tid);
    if (m != NULL) {
        const uint32_t given_ver = get_version(tid);
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            *out = m->attr;
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

void TaskGroup::set_stopped(bthread_t tid) {
    TaskMeta* const m = address_meta(tid);
    if (m != NULL) {
        const uint32_t given_ver = get_version(tid);
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            m->stop = true;
        }
    }
}

bool TaskGroup::is_stopped(bthread_t tid) {
    TaskMeta* const m = address_meta(tid);
    if (m != NULL) {
        const uint32_t given_ver = get_version(tid);
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            return m->stop;
        }
    }
    // If the tid does not exist or version does not match, it's intuitive
    // to treat the thread as "stopped".
    return true;
}

/**
 * @brief:wait_task()函数负责等待一个bthread，如果当前没有bthread可执行，则pthread会挂起。
 * @param tid
 * @return
 */
bool TaskGroup::wait_task(bthread_t* tid) {

    do {
#ifndef BTHREAD_DONT_SAVE_PARKING_STATE
        if (_last_pl_state.stopped()) {
            return false;
        }
        _pl->wait(_last_pl_state);
        if (steal_task(tid)) {
            return true;
        }
#else
        const ParkingLot::State st = _pl->get_state();
        if (st.stopped()) {
            return false;
        }
        if (steal_task(tid)) {
            return true;
        }
        _pl->wait(st);
#endif
    } while (true);
}

static double get_cumulated_cputime_from_this(void* arg) {
    return static_cast<TaskGroup*>(arg)->cumulated_cputime_ns() / 1000000000.0;
}

void TaskGroup::run_main_task() {
    bvar::PassiveStatus<double> cumulated_cputime(
        get_cumulated_cputime_from_this, this);
    std::unique_ptr<bvar::PerSecond<bvar::PassiveStatus<double> > > usage_bvar;
    
    TaskGroup* dummy = this;
    bthread_t tid;

    while (wait_task(&tid)) {

        TaskGroup::sched_to(&dummy, tid);
        // run_main_task()恢复执行的开始执行点。
        DCHECK_EQ(this, dummy);
        // 这个为啥一定是相等的呢？
        // sched_to 是调度到新的tid上，返回到这的时候，又相当于回到了main bthread上，
        // 所以 _cur_meta->stack 肯定等于 _main_stack
        DCHECK_EQ(_cur_meta->stack, _main_stack);
        // 没想明白，什么时候会出现不相等的情况呢？
        if (_cur_meta->tid != _main_tid) {
            TaskGroup::task_runner(1/*skip remained*/);
        }
        if (FLAGS_show_per_worker_usage_in_vars && !usage_bvar) {
            char name[32];
#if defined(OS_MACOSX)
            snprintf(name, sizeof(name), "bthread_worker_usage_%" PRIu64,
                     pthread_numeric_id());
#else
            snprintf(name, sizeof(name), "bthread_worker_usage_%ld",
                     (long)syscall(SYS_gettid));
#endif
            usage_bvar.reset(new bvar::PerSecond<bvar::PassiveStatus<double> >
                             (name, &cumulated_cputime, 1));
        }
    }
    // stop_main_task() was called.
    // Don't forget to add elapse of last wait_task.
    current_task()->stat.cputime_ns += butil::cpuwide_time_ns() - _last_run_ns;
}

TaskGroup::TaskGroup(TaskControl* c)
    :
#ifndef NDEBUG
    _sched_recursive_guard(0),
#endif
    _cur_meta(NULL)
    , _control(c)
    , _num_nosignal(0)
    , _nsignaled(0)
    , _last_run_ns(butil::cpuwide_time_ns())
    , _cumulated_cputime_ns(0)
    , _nswitch(0)
    , _last_context_remained(NULL)
    , _last_context_remained_arg(NULL)
    , _pl(NULL) 
    , _main_stack(NULL)
    , _main_tid(0)
    , _remote_num_nosignal(0)
    , _remote_nsignaled(0)
{
    _steal_seed = butil::fast_rand();
    _steal_offset = OFFSET_TABLE[_steal_seed % ARRAY_SIZE(OFFSET_TABLE)];
    _pl = &c->_pl[butil::fmix64(pthread_numeric_id()) % TaskControl::PARKING_LOT_NUM];
    CHECK(c);
}

TaskGroup::~TaskGroup() {
    if (_main_tid) {
        TaskMeta* m = address_meta(_main_tid);
        CHECK(_main_stack == m->stack);
        return_stack(m->release_stack());
        return_resource(get_slot(_main_tid));
        _main_tid = 0;
    }
}

int TaskGroup::init(size_t runqueue_capacity) {
    if (_rq.init(runqueue_capacity) != 0) {
        LOG(FATAL) << "Fail to init _rq";
        return -1;
    }
    if (_remote_rq.init(runqueue_capacity / 2) != 0) {
        LOG(FATAL) << "Fail to init _remote_rq";
        return -1;
    }
    ContextualStack* stk = get_stack(STACK_TYPE_MAIN, NULL);
    if (NULL == stk) {
        LOG(FATAL) << "Fail to get main stack container";
        return -1;
    }
    butil::ResourceId<TaskMeta> slot;
    TaskMeta* m = butil::get_resource<TaskMeta>(&slot);
    if (NULL == m) {
        LOG(FATAL) << "Fail to get TaskMeta";
        return -1;
    }
    m->stop = false;
    m->interrupted = false;
    m->about_to_quit = false;
    m->fn = NULL;
    m->arg = NULL;
    m->local_storage = LOCAL_STORAGE_INIT;
    m->cpuwide_start_ns = butil::cpuwide_time_ns();
    m->stat = EMPTY_STAT;
    m->attr = BTHREAD_ATTR_TASKGROUP;
    m->tid = make_tid(*m->version_butex, slot);
    m->set_stack(stk);

    _cur_meta = m;
    _main_tid = m->tid;
    _main_stack = stk;
    _last_run_ns = butil::cpuwide_time_ns();
    return 0;
}

void TaskGroup::task_runner(intptr_t skip_remained) {
    // skip_remained 是在 bthread_jump_fcontext 第3个参数
    // NOTE: tls_task_group is volatile since tasks are moved around
    //       different groups.
    TaskGroup* g = tls_task_group;

    if (!skip_remained) {
        while (g->_last_context_remained) {
            // 这里面的函数就是将上一个bthread入队
            RemainedFn fn = g->_last_context_remained;
            g->_last_context_remained = NULL;
            fn(g->_last_context_remained_arg);
            g = tls_task_group;
        }

#ifndef NDEBUG
        --g->_sched_recursive_guard;
#endif
    }

    do {
        // A task can be stopped before it gets running, in which case
        // we may skip user function, but that may confuse user:
        // Most tasks have variables to remember running result of the task,
        // which is often initialized to values indicating success. If an
        // user function is never called, the variables will be unchanged
        // however they'd better reflect failures because the task is stopped
        // abnormally.
        
        // Meta and identifier of the task is persistent in this run.
        TaskMeta* const m = g->_cur_meta;

        if (FLAGS_show_bthread_creation_in_vars) {
            // NOTE: the thread triggering exposure of pending time may spend
            // considerable time because a single bvar::LatencyRecorder
            // contains many bvar.
            g->_control->exposed_pending_time() <<
                (butil::cpuwide_time_ns() - m->cpuwide_start_ns) / 1000L;
        }
        
        // Not catch exceptions except ExitException which is for implementing
        // bthread_exit(). User code is intended to crash when an exception is 
        // not caught explicitly. This is consistent with other threading 
        // libraries.
        void* thread_return;
        try {
            // 执行应用程序设置的任务函数，在任务函数中可能yield让出cpu，也可能产生新的bthread。
            thread_return = m->fn(m->arg);
        } catch (ExitException& e) {
            thread_return = e.value();
        } 
        
        // Group is probably changed
        g = tls_task_group;

        // TODO: Save thread_return
        (void)thread_return;

        // Logging must be done before returning the keytable, since the logging lib 
        // use bthread local storage internally, or will cause memory leak.
        // FIXME: the time from quiting fn to here is not counted into cputime
        if (m->attr.flags & BTHREAD_LOG_START_AND_FINISH) {
            LOG(INFO) << "Finished bthread " << m->tid << ", cputime="
                      << m->stat.cputime_ns / 1000000.0 << "ms";
        }

        // Clean tls variables, must be done before changing version_butex
        // otherwise another thread just joined this thread may not see side
        // effects of destructing tls variables.
        KeyTable* kt = tls_bls.keytable;
        if (kt != NULL) {
            return_keytable(m->attr.keytable_pool, kt);
            // After deletion: tls may be set during deletion.
            tls_bls.keytable = NULL;
            m->local_storage.keytable = NULL; // optional
        }
        
        // Increase the version and wake up all joiners, if resulting version
        // is 0, change it to 1 to make bthread_t never be 0. Any access
        // or join to the bthread after changing version will be rejected.
        // The spinlock is for visibility of TaskGroup::get_attr.
        {
            BAIDU_SCOPED_LOCK(m->version_lock);
            if (0 == ++*m->version_butex) {
                ++*m->version_butex;
            }
        }
        // 任务函数执行完成后，需要唤起等待该任务函数执行结束的pthread/bthread。
        butex_wake_except(m->version_butex, 0);

        g->_control->_nbthreads << -1;
        g->set_remained(TaskGroup::_release_last_context, m);
        // 将pthread线程执行流转入下一个可执行的bthread（普通bthread或pthread的调度bthread）。
        ending_sched(&g);

    } while (g->_cur_meta->tid != g->_main_tid); // 只要还有其他任务执行，则会一直执行bthread
    
    // Was called from a pthread and we don't have BTHREAD_STACKTYPE_PTHREAD
    // tasks to run, quit for more tasks.
}

void TaskGroup::_release_last_context(void* arg) {
    TaskMeta* m = static_cast<TaskMeta*>(arg);
    if (m->stack_type() != STACK_TYPE_PTHREAD) {
        return_stack(m->release_stack()/*may be NULL*/);
    } else {
        // it's _main_stack, don't return.
        m->set_stack(NULL);
    }
    return_resource(get_slot(m->tid));
}
/**
 * @brief:在前台创建一个任务，立即执行，
 * @param pg
 * @param th
 * @param attr
 * @param fn
 * @param arg
 * @return
 */
int TaskGroup::start_foreground(TaskGroup** pg,
                                bthread_t* __restrict th,
                                const bthread_attr_t* __restrict attr,
                                void * (*fn)(void*),
                                void* __restrict arg) {
    if (__builtin_expect(!fn, 0)) {
        return EINVAL;
    }
    const int64_t start_ns = butil::cpuwide_time_ns();
    const bthread_attr_t using_attr = (attr ? *attr : BTHREAD_ATTR_NORMAL);
    butil::ResourceId<TaskMeta> slot;
    TaskMeta* m = butil::get_resource(&slot);
    if (__builtin_expect(!m, 0)) {
        return ENOMEM;
    }
    // 确保新的任务没有没有等待，那什么时候设置这个值呢？
    // 当当前bthread由于阻塞需要等待的时候
    CHECK(m->current_waiter.load(butil::memory_order_relaxed) == NULL);
    m->stop = false;
    m->interrupted = false;
    m->about_to_quit = false;
    m->fn = fn;
    m->arg = arg;
    CHECK(m->stack == NULL);
    m->attr = using_attr;
    m->local_storage = LOCAL_STORAGE_INIT;
    m->cpuwide_start_ns = start_ns;
    m->stat = EMPTY_STAT;
    m->tid = make_tid(*m->version_butex, slot);
    *th = m->tid;
    if (using_attr.flags & BTHREAD_LOG_START_AND_FINISH) {
        LOG(INFO) << "Started bthread " << m->tid;
    }

    TaskGroup* g = *pg;
    g->_control->_nbthreads << 1;
    // 什么叫 current_pthread_task，即当前任务是直接在thread栈上执行的
    if (g->is_current_pthread_task()) {
        // 刚开始初始化的时候，is_current_pthread_task 为 True，此时我们不应该进行直接切换
        // 只是将要运行的任务放入本地队列中，why?
        // 因为此时切换的话，我们会将 main_tid 也入队，这个是没有必要的
        // never create foreground task in pthread.
        // 不要在pthread上创建前台任务，为什么？
        g->ready_to_run(m->tid, (using_attr.flags & BTHREAD_NOSIGNAL));
    } else {
        // NOSIGNAL affects current task, not the new task.
        RemainedFn fn = NULL;
        if (g->current_task()->about_to_quit) {
            fn = ready_to_run_in_worker_ignoresignal;
        } else {
            fn = ready_to_run_in_worker;
        }
        // 这个是args是在当前函数栈上，传入的是地址，那怎么保证切换后这个地址还在呢？
        ReadyToRunArgs args = {
            g->current_tid(),
            (bool)(using_attr.flags & BTHREAD_NOSIGNAL)
        };
        g->set_remained(fn, &args);
        TaskGroup::sched_to(pg, m->tid);
    }
    return 0;
}

template <bool REMOTE>
int TaskGroup::start_background(bthread_t* __restrict th,
                                const bthread_attr_t* __restrict attr,
                                void * (*fn)(void*),
                                void* __restrict arg) {
    if (__builtin_expect(!fn, 0)) {
        return EINVAL;
    }
    const int64_t start_ns = butil::cpuwide_time_ns();
    const bthread_attr_t using_attr = (attr ? *attr : BTHREAD_ATTR_NORMAL);
    butil::ResourceId<TaskMeta> slot;
    TaskMeta* m = butil::get_resource(&slot);
    if (__builtin_expect(!m, 0)) {
        return ENOMEM;
    }
    CHECK(m->current_waiter.load(butil::memory_order_relaxed) == NULL);
    m->stop = false;
    m->interrupted = false;
    m->about_to_quit = false;
    m->fn = fn;
    m->arg = arg;
    CHECK(m->stack == NULL);
    m->attr = using_attr;
    m->local_storage = LOCAL_STORAGE_INIT;
    m->cpuwide_start_ns = start_ns;
    m->stat = EMPTY_STAT;
    m->tid = make_tid(*m->version_butex, slot);
    *th = m->tid;
    if (using_attr.flags & BTHREAD_LOG_START_AND_FINISH) {
        LOG(INFO) << "Started bthread " << m->tid;
    }
    // 重载操作符为+1
    _control->_nbthreads << 1;
    if (REMOTE) {
        ready_to_run_remote(m->tid, (using_attr.flags & BTHREAD_NOSIGNAL));
    } else {
        ready_to_run(m->tid, (using_attr.flags & BTHREAD_NOSIGNAL));
    }
    return 0;
}

// Explicit instantiations.
template int
TaskGroup::start_background<true>(bthread_t* __restrict th,
                                  const bthread_attr_t* __restrict attr,
                                  void * (*fn)(void*),
                                  void* __restrict arg);
template int
TaskGroup::start_background<false>(bthread_t* __restrict th,
                                   const bthread_attr_t* __restrict attr,
                                   void * (*fn)(void*),
                                   void* __restrict arg);

int TaskGroup::join(bthread_t tid, void** return_value) {
    if (__builtin_expect(!tid, 0)) {  // tid of bthread is never 0.
        return EINVAL;
    }
    TaskMeta* m = address_meta(tid);
    if (__builtin_expect(!m, 0)) {
        // The bthread is not created yet, this join is definitely wrong.
        return EINVAL;
    }
    TaskGroup* g = tls_task_group;
    if (g != NULL && g->current_tid() == tid) {
        // joining self causes indefinite waiting.
        return EINVAL;
    }
    const uint32_t expected_version = get_version(tid);
    while (*m->version_butex == expected_version) {
        if (butex_wait(m->version_butex, expected_version, NULL) < 0 &&
            errno != EWOULDBLOCK && errno != EINTR) {
            return errno;
        }
    }
    if (return_value) {
        *return_value = NULL;
    }
    return 0;
}

bool TaskGroup::exists(bthread_t tid) {
    if (tid != 0) {  // tid of bthread is never 0.
        TaskMeta* m = address_meta(tid);
        if (m != NULL) {
            return (*m->version_butex == get_version(tid));
        }
    }
    return false;
}

TaskStatistics TaskGroup::main_stat() const {
    TaskMeta* m = address_meta(_main_tid);
    return m ? m->stat : EMPTY_STAT;
}
/**
 * @brief: bthread任务函数结束完后会调用ending_sched()，
 * 在ending_sched()内会尝试从本地TaskGroup的任务队列中找出下一个bthread，
 * 或者从其他pthread的TaskGroup上steal一个bthread，
 * 如果没有bthread可用则下一个被执行的就是pthread的“调度bthread”，
 * 通过sched_to()将pthread的执行流转入下一个bthread的任务函数。
 * @param pg
 */
void TaskGroup::ending_sched(TaskGroup** pg) {
    TaskGroup* g = *pg;
    bthread_t next_tid = 0;
    // Find next task to run, if none, switch to idle thread of the group.
#ifndef BTHREAD_FAIR_WSQ
    // When BTHREAD_FAIR_WSQ is defined, profiling shows that cpu cost of
    // WSQ::steal() in example/multi_threaded_echo_c++ changes from 1.9%
    // to 2.9%
    const bool popped = g->_rq.pop(&next_tid);
#else
    const bool popped = g->_rq.steal(&next_tid);
#endif
    if (!popped && !g->steal_task(&next_tid)) {
        // Jump to main task if there's no task to run.
        next_tid = g->_main_tid;
    }

    TaskMeta* const cur_meta = g->_cur_meta;
    TaskMeta* next_meta = address_meta(next_tid);
    if (next_meta->stack == NULL) { // 如果是main_thread, next_meta->stack 是 null
        if (next_meta->stack_type() == cur_meta->stack_type()) {
            // also works with pthread_task scheduling to pthread_task, the
            // transfered stack is just _main_stack.
            // 堆栈还能直接交换。堆栈里面不是存放着bthread的各种寄存器信息嘛？
            // 因为 cur_meta 所对应的 bthread 已经运行结束了，其栈也没用了
            next_meta->set_stack(cur_meta->release_stack());
        } else {
            ContextualStack* stk = get_stack(next_meta->stack_type(), task_runner);
            if (stk) {
                next_meta->set_stack(stk);
            } else {
                // stack_type is BTHREAD_STACKTYPE_PTHREAD or out of memory,
                // In latter case, attr is forced to be BTHREAD_STACKTYPE_PTHREAD.
                // This basically means that if we can't allocate stack, run
                // the task in pthread directly.
                next_meta->attr.stack_type = BTHREAD_STACKTYPE_PTHREAD;
                next_meta->set_stack(g->_main_stack);
            }
        }
    }
    sched_to(pg, next_meta);
}

void TaskGroup::sched(TaskGroup** pg) {
    TaskGroup* g = *pg;
    bthread_t next_tid = 0;
    // Find next task to run, if none, switch to idle thread of the group.
#ifndef BTHREAD_FAIR_WSQ
    const bool popped = g->_rq.pop(&next_tid);
#else
    const bool popped = g->_rq.steal(&next_tid);
#endif
    if (!popped && !g->steal_task(&next_tid)) {
        // Jump to main task if there's no task to run.
        next_tid = g->_main_tid;
    }
    sched_to(pg, next_tid);
}

void TaskGroup::sched_to(TaskGroup** pg, TaskMeta* next_meta) {
    TaskGroup* g = *pg;
#ifndef NDEBUG
    if ((++g->_sched_recursive_guard) > 1) {
        LOG(FATAL) << "Recursively(" << g->_sched_recursive_guard - 1
                   << ") call sched_to(" << g << ")";
    }
#endif
    // Save errno so that errno is bthread-specific.
    // 这两个是bthread相关的，我们在切换前保存，切换到另一个bthread后，再将其恢复
    const int saved_errno = errno;
    void* saved_unique_user_ptr = tls_unique_user_ptr;
    // 此处const表示指针是常量
    TaskMeta* const cur_meta = g->_cur_meta;
    const int64_t now = butil::cpuwide_time_ns();
    const int64_t elp_ns = now - g->_last_run_ns;
    g->_last_run_ns = now;
    cur_meta->stat.cputime_ns += elp_ns;
    if (cur_meta->tid != g->main_tid()) {
        // 如果一个bthread在执行过程中生成了新的bthread，会走到这里
        g->_cumulated_cputime_ns += elp_ns;
    }
    ++cur_meta->stat.nswitch;
    ++ g->_nswitch;
    // Switch to the task
    if (__builtin_expect(next_meta != cur_meta, 1)) {
        // 将_cur_meta指向下一个将要执行的bthread的TaskMeta对象的指针。
        g->_cur_meta = next_meta;
        // Switch tls_bls，记录当前thread的local_storage
        // tls_bls存储的是当前bthread的一些运行期数据（统计量等），执行切换动作前，将tls_bls的内容复制到
        // 当前bthread的私有storage空间中，再将tls_bls重新指向将要执行的bthread的私有storage。
        cur_meta->local_storage = tls_bls;
        // 更新 tls_bls 为要交换的 taskMeta的 local_storage
        tls_bls = next_meta->local_storage;

        // Logging must be done after switching the local storage, since the logging lib 
        // use bthread local storage internally, or will cause memory leak.
        if ((cur_meta->attr.flags & BTHREAD_LOG_CONTEXT_SWITCH) ||
            (next_meta->attr.flags & BTHREAD_LOG_CONTEXT_SWITCH)) {
            LOG(INFO) << "Switch bthread: " << cur_meta->tid << " -> "
                      << next_meta->tid;
        }

        if (cur_meta->stack != NULL) {
            if (next_meta->stack != cur_meta->stack) {
                // 正常情况下都是不相等，相等的情况是都是 pthread_task
                // 进行task切换
                // 这里真正执行bthread的切换。
                // 将执行pthread的cpu的寄存器的当前状态存入cur_meta的context中，并将next_meta的context中
                // 的数据加载到cpu的寄存器中，开始执行next_meta的任务函数。
                jump_stack(cur_meta->stack, next_meta->stack);
                // jump_stack 在切换到新的bthread的同时，也会下一步执行的地址放入到当前bthread的堆栈上
                // 当再次返回时，会执行到下面的语句，此时有可能被调度到了其他pthread上了
                // probably went to another group, need to assign g again.
                // 这是什么情况？因为有可能切换到另一个steal来的bthread上，此时我们要重新
                // 获取下当前thread的 taskGroup
                // 这里是cur_meta代表的bthread的恢复执行点。
                // bthread恢复执行的时候可能被steal到其他pthread上了，需要重置TaskGroup对象的指针g。
                g = tls_task_group;
            }
#ifndef NDEBUG
            else {
                // else pthread_task is switching to another pthread_task, sc
                // can only equal when they're both _main_stack
                CHECK(cur_meta->stack == g->_main_stack);
            }
#endif
        }
        // else because of ending_sched(including pthread_task->pthread_task)
    } else {
        LOG(FATAL) << "bthread=" << g->current_tid() << " sched_to itself!";
    }

    while (g->_last_context_remained) {
        RemainedFn fn = g->_last_context_remained;
        g->_last_context_remained = NULL;
        fn(g->_last_context_remained_arg);
        g = tls_task_group;
    }

    // Restore errno，这个的作用是？
    errno = saved_errno;
    tls_unique_user_ptr = saved_unique_user_ptr;

#ifndef NDEBUG
    --g->_sched_recursive_guard;
#endif
    *pg = g;
}

void TaskGroup::destroy_self() {
    if (_control) {
        _control->_destroy_group(this);
        _control = NULL;
    } else {
        CHECK(false);
    }
}
/**
 * @brief:将tid放入本地队列，根据是否信号触发，唤醒任务
 * @param tid
 * @param nosignal
 */
void TaskGroup::ready_to_run(bthread_t tid, bool nosignal) {
    push_rq(tid);
    if (nosignal) {
        ++_num_nosignal;
    } else {
        const int additional_signal = _num_nosignal;
        _num_nosignal = 0;
        _nsignaled += 1 + additional_signal;
        _control->signal_task(1 + additional_signal);
    }
}

void TaskGroup::flush_nosignal_tasks() {
    const int val = _num_nosignal;
    if (val) {
        _num_nosignal = 0;
        _nsignaled += val;
        _control->signal_task(val);
    }
}

void TaskGroup::ready_to_run_remote(bthread_t tid, bool nosignal) {
    _remote_rq._mutex.lock();
    while (!_remote_rq.push_locked(tid)) {
        flush_nosignal_tasks_remote_locked(_remote_rq._mutex);
        LOG_EVERY_SECOND(ERROR) << "_remote_rq is full, capacity="
                                << _remote_rq.capacity();
        ::usleep(1000);
        _remote_rq._mutex.lock();
    }
    if (nosignal) {
        ++_remote_num_nosignal;
        _remote_rq._mutex.unlock();
    } else {
        const int additional_signal = _remote_num_nosignal;
        _remote_num_nosignal = 0;
        _remote_nsignaled += 1 + additional_signal;
        _remote_rq._mutex.unlock();
        _control->signal_task(1 + additional_signal);
    }
}

void TaskGroup::flush_nosignal_tasks_remote_locked(butil::Mutex& locked_mutex) {
    const int val = _remote_num_nosignal;
    if (!val) {
        locked_mutex.unlock();
        return;
    }
    _remote_num_nosignal = 0;
    _remote_nsignaled += val;
    locked_mutex.unlock();
    _control->signal_task(val);
}

void TaskGroup::ready_to_run_general(bthread_t tid, bool nosignal) {
    if (tls_task_group == this) {
        return ready_to_run(tid, nosignal);
    }
    return ready_to_run_remote(tid, nosignal);
}

void TaskGroup::flush_nosignal_tasks_general() {
    if (tls_task_group == this) {
        return flush_nosignal_tasks();
    }
    return flush_nosignal_tasks_remote();
}

// 当bthread切换后，把前一个bthread重新放入队列中
void TaskGroup::ready_to_run_in_worker(void* args_in) {
    ReadyToRunArgs* args = static_cast<ReadyToRunArgs*>(args_in);
    return tls_task_group->ready_to_run(args->tid, args->nosignal);
}
// 当bthread切换后，把前一个bthread重新放入队列中，当时因为已经要结束了，所以只是入队，不会执行
void TaskGroup::ready_to_run_in_worker_ignoresignal(void* args_in) {
    ReadyToRunArgs* args = static_cast<ReadyToRunArgs*>(args_in);
    return tls_task_group->push_rq(args->tid);
}

struct SleepArgs {
    uint64_t timeout_us;
    bthread_t tid;
    TaskMeta* meta;
    TaskGroup* group;
};

/**
 * @brief: 到期后运行的函数
 * @param arg
 */
static void ready_to_run_from_timer_thread(void* arg) {
    CHECK(tls_task_group == NULL);
    const SleepArgs* e = static_cast<const SleepArgs*>(arg);
    // 此处是从 TimerThread 中运行的，选择一个group，放入remote队列中
    e->group->control()->choose_one_group()->ready_to_run_remote(e->tid);
}

/**
 * @brief: 当bthread主动调用 TaskGroup::usleep 切换到其他bthread上，这是切换后调用的函数
 * @param void_args
 */
void TaskGroup::_add_sleep_event(void* void_args) {
    // Must copy SleepArgs. After calling TimerThread::schedule(), previous
    // thread may be stolen by a worker immediately and the on-stack SleepArgs
    // will be gone.
    // 此处为什么要直接复制出来呢？因为当调用 TimerThread::schedule() 后
    // 可能时间比较短，马上就调用了 ready_to_run_from_timer_thread，
    // 这个函数会直接将选择一个TaskGroup将其运行，运行过程中堆栈就可能发生变化了
    // 那为什么现在复制可以呢，因为他还在睡眠中。堆栈还在那
    SleepArgs e = *static_cast<SleepArgs*>(void_args);
    TaskGroup* g = e.group;
    
    TimerThread::TaskId sleep_id;
    sleep_id = get_global_timer_thread()->schedule(
        ready_to_run_from_timer_thread, void_args,
        butil::microseconds_from_now(e.timeout_us));

    if (!sleep_id) {
        // fail to schedule timer, go back to previous thread.
        // 如果没有睡眠成，则直接重新调度，为什么不放入 remote_rq ?
        g->ready_to_run(e.tid);
        return;
    }
    
    // Set TaskMeta::current_sleep which is for interruption.
    const uint32_t given_ver = get_version(e.tid);
    {
        BAIDU_SCOPED_LOCK(e.meta->version_lock);
        // given_ver == *e.meta->version_butex 这个的作用是？
        if (given_ver == *e.meta->version_butex && !e.meta->interrupted) {
            e.meta->current_sleep = sleep_id;
            return;
        }
    }
    // The thread is stopped or interrupted.
    // interrupt() always sees that current_sleep == 0. It will not schedule
    // the calling thread. The race is between current thread and timer thread.
    if (get_global_timer_thread()->unschedule(sleep_id) == 0) {
        // added to timer, previous thread may be already woken up by timer and
        // even stopped. It's safe to schedule previous thread when unschedule()
        // returns 0 which means "the not-run-yet sleep_id is removed". If the
        // sleep_id is running(returns 1), ready_to_run_in_worker() will
        // schedule previous thread as well. If sleep_id does not exist,
        // previous thread is scheduled by timer thread before and we don't
        // have to do it again.
        g->ready_to_run(e.tid);
    }
}

// To be consistent with sys_usleep, set errno and return -1 on error.
int TaskGroup::usleep(TaskGroup** pg, uint64_t timeout_us) {
    if (0 == timeout_us) {
        // 直接切换
        yield(pg);
        return 0;
    }
    TaskGroup* g = *pg;
    // We have to schedule timer after we switched to next bthread otherwise
    // the timer may wake up(jump to) current still-running context.
    SleepArgs e = { timeout_us, g->current_tid(), g->current_task(), g };
    g->set_remained(_add_sleep_event, &e);
    sched(pg);
    g = *pg;
    e.meta->current_sleep = 0;
    if (e.meta->interrupted) {
        // Race with set and may consume multiple interruptions, which are OK.
        e.meta->interrupted = false;
        // NOTE: setting errno to ESTOP is not necessary from bthread's
        // pespective, however many RPC code expects bthread_usleep to set
        // errno to ESTOP when the thread is stopping, and print FATAL
        // otherwise. To make smooth transitions, ESTOP is still set instead
        // of EINTR when the thread is stopping.
        errno = (e.meta->stop ? ESTOP : EINTR);
        return -1;
    }
    return 0;
}

// Defined in butex.cpp
bool erase_from_butex_because_of_interruption(ButexWaiter* bw);

static int interrupt_and_consume_waiters(
    bthread_t tid, ButexWaiter** pw, uint64_t* sleep_id) {
    TaskMeta* const m = TaskGroup::address_meta(tid);
    if (m == NULL) {
        return EINVAL;
    }
    const uint32_t given_ver = get_version(tid);
    BAIDU_SCOPED_LOCK(m->version_lock);
    if (given_ver == *m->version_butex) {
        *pw = m->current_waiter.exchange(NULL, butil::memory_order_acquire);
        *sleep_id = m->current_sleep;
        m->current_sleep = 0;  // only one stopper gets the sleep_id
        m->interrupted = true;
        return 0;
    }
    return EINVAL;
}

static int set_butex_waiter(bthread_t tid, ButexWaiter* w) {
    TaskMeta* const m = TaskGroup::address_meta(tid);
    if (m != NULL) {
        const uint32_t given_ver = get_version(tid);
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            // Release fence makes m->interrupted visible to butex_wait
            m->current_waiter.store(w, butil::memory_order_release);
            return 0;
        }
    }
    return EINVAL;
}

// The interruption is "persistent" compared to the ones caused by signals,
// namely if a bthread is interrupted when it's not blocked, the interruption
// is still remembered and will be checked at next blocking. This designing
// choice simplifies the implementation and reduces notification loss caused
// by race conditions.
// TODO: bthreads created by BTHREAD_ATTR_PTHREAD blocking on bthread_usleep()
// can't be interrupted.
int TaskGroup::interrupt(bthread_t tid, TaskControl* c) {
    // Consume current_waiter in the TaskMeta, wake it up then set it back.
    ButexWaiter* w = NULL;
    uint64_t sleep_id = 0;
    int rc = interrupt_and_consume_waiters(tid, &w, &sleep_id);
    if (rc) {
        return rc;
    }
    // a bthread cannot wait on a butex and be sleepy at the same time.
    CHECK(!sleep_id || !w);
    if (w != NULL) {
        erase_from_butex_because_of_interruption(w);
        // If butex_wait() already wakes up before we set current_waiter back,
        // the function will spin until current_waiter becomes non-NULL.
        rc = set_butex_waiter(tid, w);
        if (rc) {
            LOG(FATAL) << "butex_wait should spin until setting back waiter";
            return rc;
        }
    } else if (sleep_id != 0) {
        if (get_global_timer_thread()->unschedule(sleep_id) == 0) {
            bthread::TaskGroup* g = bthread::tls_task_group;
            if (g) {
                g->ready_to_run(tid);
            } else {
                if (!c) {
                    return EINVAL;
                }
                c->choose_one_group()->ready_to_run_remote(tid);
            }
        }
    }
    return 0;
}
/**
 * @brief: 一个bthread在自己的任务函数执行过程中想要挂起时，
 * 调用TaskGroup::yield(TaskGroup** pg)，yield()内部会调用TaskGroup::sched(TaskGroup** pg)，
 * sched()也是负责将pthread的执行流转入下一个bthread（普通bthread或调度bthread）的任务函数。
 * 挂起的bthread在适当的时候会被其他bthread唤醒，
 * 即某个bthread会负责将挂起的bthread的tid重新加入TaskGroup的任务队列。
 * @param pg
 */
void TaskGroup::yield(TaskGroup** pg) {
    TaskGroup* g = *pg;
    ReadyToRunArgs args = { g->current_tid(), false };
    g->set_remained(ready_to_run_in_worker, &args);
    sched(pg);
}

void print_task(std::ostream& os, bthread_t tid) {
    TaskMeta* const m = TaskGroup::address_meta(tid);
    if (m == NULL) {
        os << "bthread=" << tid << " : never existed";
        return;
    }
    const uint32_t given_ver = get_version(tid);
    bool matched = false;
    bool stop = false;
    bool interrupted = false;
    bool about_to_quit = false;
    void* (*fn)(void*) = NULL;
    void* arg = NULL;
    bthread_attr_t attr = BTHREAD_ATTR_NORMAL;
    bool has_tls = false;
    int64_t cpuwide_start_ns = 0;
    TaskStatistics stat = {0, 0};
    {
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            matched = true;
            stop = m->stop;
            interrupted = m->interrupted;
            about_to_quit = m->about_to_quit;
            fn = m->fn;
            arg = m->arg;
            attr = m->attr;
            has_tls = m->local_storage.keytable;
            cpuwide_start_ns = m->cpuwide_start_ns;
            stat = m->stat;
        }
    }
    if (!matched) {
        os << "bthread=" << tid << " : not exist now";
    } else {
        os << "bthread=" << tid << " :\nstop=" << stop
           << "\ninterrupted=" << interrupted
           << "\nabout_to_quit=" << about_to_quit
           << "\nfn=" << (void*)fn
           << "\narg=" << (void*)arg
           << "\nattr={stack_type=" << attr.stack_type
           << " flags=" << attr.flags
           << " keytable_pool=" << attr.keytable_pool 
           << "}\nhas_tls=" << has_tls
           << "\nuptime_ns=" << butil::cpuwide_time_ns() - cpuwide_start_ns
           << "\ncputime_ns=" << stat.cputime_ns
           << "\nnswitch=" << stat.nswitch;
    }
}

}  // namespace bthread
