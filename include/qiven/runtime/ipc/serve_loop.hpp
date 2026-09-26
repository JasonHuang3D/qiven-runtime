#pragma once

// ============================================================================
// ipc/serve_loop.hpp — the production LISTEN-POOL serve loop
// (host-server redesign LL-1/LL-3; docs/design/mvp4-host-server.md §5)
//
// ServeLoop owns the host's accept topology: several concurrently-ARMED
// pipe instances (one per arm thread — a single armed instance admits one
// concurrent connect and races ERROR_PIPE_BUSY into a false 120, the
// redesign's named defect), one serve thread per accepted connection
// bounded by a cap (over-cap receives the typed 125 busy frame + linger +
// close), a NEVER-FATAL accept policy (vanish storms and creation failures
// degrade loud and keep retrying — the process never ends because of a
// client), per-connection fault containment (an escaping exception closes
// one connection, never the host), and a PHASED stop (arms stop
// accepting; in-flight requests complete within the grace; remaining
// serve threads observe the stop inside one read slice; the serve-thread
// countdown releases the drain). An authenticated Shutdown request
// observed through the handle hook stops the loop after its ack rides the
// connection.
// ============================================================================

#include <qiven/result.hpp>
#include <qiven/runtime/ipc/framing.hpp>
#include <qiven/runtime/ipc/named_pipe_server.hpp>
#include <qiven/runtime/ipc/pipe_service.hpp>
#include <qiven/types.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace qiven::runtime::ipc
{
class ServeLoop
{
public:
    struct Options
    {
        u64 listen_arms       = 4;     // concurrently armed instances
        u64 max_connections   = 8;     // serve-thread cap (busy beyond)
        u64 idle_timeout_ms   = 30000; // per-connection frame idle (hygiene)
        u64 max_frames        = 64;    // per-connection frame budget
        u64 write_deadline_ms = 5000;  // per-write hygiene bound
        u64 stop_grace_ms     = 5000;  // in-flight completion grace
        u64 connect_slice_ms  = 100;   // accept-wait quantization (stop)
        i32 busy_code         = err_server_busy;
    };

    struct Stats
    {
        std::atomic<u64> connections_served { 0 };
        std::atomic<u64> busy_rejected { 0 };
        std::atomic<u64> accept_recreates { 0 };
        std::atomic<u64> serve_thread_faults { 0 };
        std::atomic<u64> busy_occupancy_last { 0 };
        std::atomic<bool> degraded_listener { false };
    };

    struct Hooks
    {
        AdmitFn admit;                             // per connection (image check)
        HandleFn handle;                           // per request
        std::function<void(std::string_view)> log; // observable line
    };

    // Creates the FIRST pipe instance with FILE_FLAG_FIRST_PIPE_INSTANCE
    // (the OS-level singleton half; the named root mutex fires first).
    [[nodiscard]] static qiven::Result<std::unique_ptr<ServeLoop>> create(
        std::string_view install_id, const FrameCodec& codec, Options options, Hooks hooks);

    // Serves until request_stop() (or an authenticated Shutdown request
    // observed through the handle hook), then drains phased and returns.
    // The caller's thread coordinates the stop; arms and serve threads are
    // owned internally.
    void run();

    // Thread-safe stop request (console handler, Shutdown hook, tests).
    void request_stop() noexcept;

    [[nodiscard]] bool stop_requested() const noexcept
    {
        return m_stop.load(std::memory_order_acquire);
    }

    [[nodiscard]] const Stats& stats() const noexcept
    {
        return m_stats;
    }

    ~ServeLoop();

    ServeLoop(const ServeLoop&)            = delete;
    ServeLoop& operator=(const ServeLoop&) = delete;

private:
    ServeLoop(std::wstring pipe, void* first_instance, FrameCodec codec, Options options,
              Hooks hooks);
    void arm_thread_body(usize arm_index);
    void dispatch_connection(PipeConnection connection);
    void serve_thread_body(PipeConnection connection);
    void linger_for_peer_read(PipeConnection& connection);

    const std::wstring m_pipe;
    const FrameCodec m_codec; // copied: the loop outlives caller temporaries
    const Options m_options;
    Hooks m_hooks;
    Stats m_stats;
    std::atomic<bool> m_stop { false };
    std::mutex m_serve_mutex; // serve-thread registry + CV
    std::condition_variable m_serve_cv;
    u64 m_live_serve_threads = 0;
    std::vector<std::thread> m_arms;
    void* m_first_instance = nullptr; // armed by create(); arm 0 inherits it
};
} // namespace qiven::runtime::ipc
