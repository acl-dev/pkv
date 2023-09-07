#pragma once
#include <atomic>

namespace pkv {

// A spinlock class that provides a simple mutual exclusion mechanism.
class spinlock {
public:
	// Default constructor and destructor.
	spinlock() = default;
	~spinlock() = default;

	// Locks the spinlock by spinning until the lock is acquired.
	void lock() {
		while (flag.test_and_set(std::memory_order_acquire));
	}

	// Unlocks the spinlock by clearing the lock flag.
	void unlock() {
		flag.clear(std::memory_order_release);
	}

private:
	// The atomic flag used to implement the spinlock.
	std::atomic_flag flag = ATOMIC_FLAG_INIT;
};


} // namespace pkv
