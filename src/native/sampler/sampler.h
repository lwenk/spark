#ifndef ENDSTONE_SPARK_SAMPLER_H
#define ENDSTONE_SPARK_SAMPLER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <concurrentqueue.h>

#include "native/python/python_attribution.h"
#include "native/sampler/call_tree.h"
#include "native/sampler/heartbeat.h"
#include "native/sampler/recovery_sink.h"
#include "native/sampler/thread_selector.h"
#include "native/sampler/types.h"
#include "core/profiler/profiling_window.h"

namespace spark {

struct SamplerTestAccess;
struct ProfilerTestAccess;

struct SamplerConfig {
    int interval_us = 4000;
    bool ignore_sleeping = false;
    bool all_threads = false;
    bool regex_threads = false;
    std::vector<std::string> thread_patterns;
    std::int64_t only_ticks_over_ms = 0;  // 0 = disabled (record every tick)
};

// Per-window tick accounting, used to build the viewer's timeline overlay.
struct WindowTickStats {
    int ticks = 0;
    double mspt_sum = 0.0;
    double mspt_max = 0.0;
};

struct ThreadCallTree {
    std::uint64_t thread_id = 0;
    std::string thread_name;
    CallTree tree;
};

// Wall-clock sampling profiler. Captures stacks at a fixed interval into a lock-free
// queue; the aggregator thread builds per-thread call trees. No Endstone API access.
class Sampler {
public:
    ~Sampler();

    bool start(const SamplerConfig &config);  // arms capture + spawns threads
    bool stop();                              // stops + joins; safe to call once
    void requestStop() noexcept;              // stops the producer without joining or disarming

    // Temporarily stop both service threads without clearing accumulated data
    // or disarming the capture backend. Allows safe concurrent reads from
    // exportData() during a live viewer window rotate.
    void pauseForExport();
    bool resumeAfterExport();

    bool running() const { return running_.load(); }
    bool failure(std::string &error) const;

    void setTarget(std::uint64_t tid, std::string name = "Server thread")
    {
        target_tid_.store(tid);
        target_name_ = std::move(name);
    }

    // The provider owns only fixed atomic shadow stacks on the sampling path.
    // The sampler never calls into CPython and never acquires the GIL.
    void setPythonStackProvider(PythonStackProvider *provider) noexcept
    {
        python_stack_provider_.store(provider, std::memory_order_release);
    }
    PythonStackProvider *pythonStackProvider() const noexcept
    {
        return python_stack_provider_.load(std::memory_order_acquire);
    }

    // Sets the recovery sink for crash-safe journaling. Must be called before start().
    // All RecoverySink methods are non-blocking.
    void setRecoverySink(RecoverySink *sink) { recovery_sink_ = sink; }

    // Called once per server tick from the main thread: `mspt_ms` is the duration
    // of the tick that just finished.
    void onTick(double mspt_ms);

    // Valid after stop(): the aggregated data.
    const CallTree &tree() const { return tree_; }
    const ModuleTable &modules() const { return modules_; }
    const std::map<std::uint64_t, ThreadCallTree> &threadTrees() const { return thread_trees_; }
    const std::map<std::int32_t, WindowTickStats> &windowTicks() const { return window_ticks_; }
    std::uint64_t numberOfTicks() const { return current_tick_.load(); }
    std::uint64_t sampleCount() const { return sample_count_.load(std::memory_order_relaxed); }
    std::uint64_t droppedSamples() const { return dropped_samples_.load(std::memory_order_relaxed); }
    std::uint64_t droppedQueueSamples() const { return dropped_queue_samples_.load(std::memory_order_relaxed); }
    std::uint64_t droppedPendingSamples() const { return dropped_pending_samples_.load(std::memory_order_relaxed); }
    std::uint64_t droppedProfileSamples() const { return dropped_profile_samples_.load(std::memory_order_relaxed); }
    std::uint64_t droppedTickEvents() const { return dropped_tick_events_.load(std::memory_order_relaxed); }
    std::uint64_t terminalInFlightTickSamplesDiscarded() const
    {
        return terminal_in_flight_tick_samples_discarded_.load(std::memory_order_relaxed);
    }
    static constexpr std::size_t sampleQueueCapacity() { return kSampleQueueCapacity; }
    static constexpr std::size_t tickQueueCapacity() { return kTickQueueCapacity; }
    static constexpr std::size_t moduleCapacity() { return kModuleCapacity; }
    static constexpr std::size_t threadRootCapacity() { return kThreadRootCapacity; }
    static constexpr std::size_t pendingSampleCapacity() { return kMaxPendingSamples; }
    static constexpr std::size_t profileNodeCapacity() { return kProfileNodeCapacity; }
    static constexpr std::size_t profileTimeEntryCapacity() { return kProfileTimeEntryCapacity; }
    std::uint64_t moduleOverflowFrames() const { return module_overflow_frames_.load(std::memory_order_relaxed); }
    std::uint64_t overflowThreadSamples() const { return overflow_thread_samples_.load(std::memory_order_relaxed); }
    std::uint64_t historySamplesPruned() const { return history_samples_pruned_.load(std::memory_order_relaxed); }
    std::size_t retainedHistoryWindows() const { return tree_.root().times.size(); }
    bool historyTruncated() const { return historySamplesPruned() != 0; }
    bool profileStorageExhausted() const { return profile_storage_exhausted_.load(std::memory_order_relaxed); }
    bool dataIncomplete() const { return droppedSamples() != 0 || droppedTickEvents() != 0; }
    const std::string &lastError() const { return last_error_; }

    // Heartbeats updated by the sampler and aggregator threads.
    const Heartbeat &samplerHeartbeat() const { return sampler_heartbeat_; }
    const Heartbeat &aggregatorHeartbeat() const { return aggregator_heartbeat_; }

private:
    friend struct SamplerTestAccess;
    friend struct ProfilerTestAccess;

    static constexpr std::size_t kSampleQueueCapacity = 4096;
    static constexpr std::size_t kTickQueueCapacity = 4096;
    static constexpr std::size_t kModuleCapacity = 512;
    static constexpr std::size_t kMaxThreadIdentities = 256;
    static constexpr std::size_t kThreadRootCapacity = kMaxThreadIdentities + 1;
    static constexpr std::size_t kProfileNodeCapacity = 131072;
    static constexpr std::size_t kProfileTimeEntryCapacity = 2 * 1024 * 1024;
    static constexpr std::size_t kMaxPendingSamples = 32768;

    struct QueueTraits : moodycamel::ConcurrentQueueDefaultTraits {
        static constexpr std::size_t MAX_SUBQUEUE_SIZE = kSampleQueueCapacity;
    };

    struct TickEvent {
        std::uint64_t tick_id;
        double mspt_ms;
    };

    void samplerLoop();
    void aggregatorLoop();
    void drainQueues();
    bool acceptSample(const Sample &sample);
    void journalModuleDefinitions(const Sample &sample);
    void flushOrDrop(std::uint64_t tick_id, bool keep);
    void resetSession();
    static std::int32_t currentWindow();
    void maybePruneHistory(std::int32_t current_window);
    void maybePruneTickHistory(std::int32_t current_window);
    void recordTickDecision(std::uint64_t tick_id, bool keep);
    bool enqueueSample(Sample sample) noexcept;
    void dropPendingSamples(std::size_t count) noexcept;
    void finishPending(std::uint64_t terminal_tick);
    void markWorkerFailure() noexcept;
    bool startServiceThreads();

    SamplerConfig config_;
    ThreadSelector thread_selector_;
    std::string last_error_;
    std::atomic<bool> running_{false};      // sampler (producer) thread
    std::atomic<bool> agg_running_{false};  // aggregator (consumer) thread
    std::atomic<std::uint64_t> target_tid_{0};
    std::atomic<std::uint64_t> current_tick_{0};
    std::atomic<std::uint64_t> sample_count_{0};
    std::atomic<std::uint64_t> dropped_samples_{0};
    std::atomic<std::uint64_t> dropped_queue_samples_{0};
    std::atomic<std::uint64_t> dropped_pending_samples_{0};
    std::atomic<std::uint64_t> dropped_profile_samples_{0};
    std::atomic<std::uint64_t> dropped_tick_events_{0};
    std::atomic<std::uint64_t> terminal_in_flight_tick_samples_discarded_{0};
    std::atomic<std::uint64_t> module_overflow_frames_{0};
    std::atomic<std::uint64_t> overflow_thread_samples_{0};
    std::atomic<std::uint64_t> history_samples_pruned_{0};
    std::atomic<bool> profile_storage_exhausted_{false};
    std::atomic<std::uint64_t> sampler_tid_{0};
    std::atomic<std::uint64_t> aggregator_tid_{0};
    std::atomic<bool> worker_failed_{false};
    std::atomic<bool> tick_admission_open_{true};
    std::atomic<bool> finalize_pending_{false};
    std::atomic<bool> pending_finalized_{false};
    std::atomic<std::uint64_t> terminal_tick_{0};
    std::atomic<std::uint64_t> service_start_count_{0};
    std::atomic<PythonStackProvider *> python_stack_provider_{nullptr};
    std::string target_name_ = "Server thread";

    std::thread sampler_thread_;
    std::thread aggregator_thread_;
    std::mutex lifecycle_mutex_;
    std::mutex tick_mutex_;
    std::condition_variable wait_cv_;
    std::mutex wait_mutex_;

    moodycamel::ConcurrentQueue<Sample, QueueTraits> samples_{kSampleQueueCapacity};
    moodycamel::ProducerToken sample_producer_{samples_};
    moodycamel::ConcurrentQueue<TickEvent, QueueTraits> ticks_{kTickQueueCapacity};
    moodycamel::ProducerToken tick_producer_{ticks_};

    // aggregator-thread state
    CallTree tree_;
    std::map<std::uint64_t, ThreadCallTree> thread_trees_;
    std::unordered_map<std::uint64_t, std::vector<Sample>> buckets_;
    std::size_t pending_sample_count_ = 0;
    std::deque<std::uint8_t> tick_decisions_;  // 0 pending, 1 drop, 2 keep
    std::uint64_t tick_decision_base_ = 0;
    std::map<std::int32_t, std::uint64_t> window_sample_counts_;
    std::int64_t next_history_prune_window_ = profiling_window::kHistorySize;
    std::size_t profile_nodes_remaining_ = kProfileNodeCapacity;
    std::size_t profile_time_entries_remaining_ = kProfileTimeEntryCapacity;

    // sampler-thread state
    ModuleTable modules_{kModuleCapacity};
    std::vector<RecoveryModuleDefinition> pending_recovery_module_definitions_;

    // main-thread state (written by onTick, read at export after join)
    std::map<std::int32_t, WindowTickStats> window_ticks_;
    std::int64_t next_tick_history_prune_window_ = profiling_window::kHistorySize;

    // Heartbeats for stall-watchdog diagnostics (updated by service threads).
    Heartbeat sampler_heartbeat_;
    Heartbeat aggregator_heartbeat_;

    // Recovery journal sink (nullptr = no journaling).  Used by the
    // aggregator thread only.
    RecoverySink *recovery_sink_ = nullptr;
    std::unordered_map<std::uint64_t, std::uint64_t> thread_identities_;
    std::unordered_set<std::uint64_t> journaled_threads_;
    std::function<void()> sampler_thread_hook_;
    std::function<void()> aggregator_thread_hook_;

    static constexpr std::int32_t kHistoryPruneIntervalWindows = 1;
    static constexpr std::size_t kTickDecisionCapacity = 60 * 60 * 20 + 1;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_SAMPLER_H
