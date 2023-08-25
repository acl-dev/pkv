#pragma once
#include <atomic>

namespace pkv {

class spinlock {
public:
	spinlock() = default;
	~spinlock() = default;

	void lock() {
		while (flag.test_and_set(std::memory_order_acquire));
	}

	void unlock() {
		flag.clear(std::memory_order_release);
	}

private:
	std::atomic_flag flag = ATOMIC_FLAG_INIT;
};

} // namespace pkv
