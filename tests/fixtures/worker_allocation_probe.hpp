#pragma once
#include <atomic>
#include <thread>
namespace worker_allocation_probe {
inline std::atomic<bool> enabled{};
inline std::atomic<std::size_t> count{};
inline std::thread::id owner;
inline void Record() noexcept {
  if(enabled.load(std::memory_order_acquire)&&std::this_thread::get_id()!=owner)
    count.fetch_add(1,std::memory_order_relaxed);
}
}
