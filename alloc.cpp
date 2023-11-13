#include <iostream>
#include <vector>
#include <atomic>
#include <thread>
#include <stdexcept>
#include <chrono>

class SpinlockGuard
{
public:
    /**
     * @brief Construct a new Spinlock Guard object and acquire the lock if the calling thread doesn't already hold it.
     *
     * The constructor attempts to acquire the lock by setting the atomic flag. If the current thread already owns the lock,
     * as indicated by the owner atomic, the lock acquisition is bypassed, preventing reentrancy issues.
     *
     * @param flag Reference to the atomic flag used for the lock.
     * @param owner Reference to the atomic holding the ID of the owning thread.
     */
    explicit SpinlockGuard(std::atomic_flag &flag, std::atomic<std::thread::id> &owner)
        : flag_(flag), owner_(owner), owns_lock_(false)
    {
        std::thread::id this_id = std::this_thread::get_id();
        if (owner_.load() != this_id)
        {
            // Attempt to acquire the lock only if the current thread does not own it.
            while (flag_.test_and_set(std::memory_order_acquire))
            {
                // Spin-wait loop for the lock to be released.
            }
            owner_.store(this_id);
            owns_lock_ = true;
        }
    }

    /**
     * @brief Destroy the Spinlock Guard object.
     *
     * The destructor releases the lock if the current thread owns it by clearing the atomic flag
     * and resetting the owner atomic to a default thread ID.
     */
    ~SpinlockGuard()
    {
        if (owns_lock_)
        {
            // Only release the lock if it was acquired by this guard.
            owner_.store(std::thread::id());
            flag_.clear(std::memory_order_release);
        }
    }

    // Non-copyable and non-movable.
    SpinlockGuard(const SpinlockGuard &) = delete;
    SpinlockGuard &operator=(const SpinlockGuard &) = delete;

private:
    std::atomic_flag &flag_;              ///< The atomic flag used for the lock.
    std::atomic<std::thread::id> &owner_; ///< The atomic holding the ID of the owning thread.
    bool owns_lock_;                      ///< Flag indicating whether this guard owns the lock.
};

class MemoryAllocator
{
public:
    MemoryAllocator(size_t blockSize, size_t totalPoolSizeInBytes)
        : blockSize_(blockSize), totalPoolSizeInBytes_(totalPoolSizeInBytes), lockFlag_(ATOMIC_FLAG_INIT), owner_(std::thread::id())
    {
        validateParameters();
        initializePool();
    }

    ~MemoryAllocator()
    {
        operator delete[](memoryPool_);
    }

    void *allocate(size_t size)
    {
        SpinlockGuard guard(lockFlag_, owner_);
        size_t blocksNeeded = (size + blockSize_ - 1) / blockSize_;
        std::cout << "Allocating size: " << size << ", blocks needed: " << blocksNeeded << std::endl;

        if (blocksNeeded > blockPointers_.size())
        {
            std::cerr << "Allocation error: Not enough blocks in the pool" << std::endl;
            return nullptr;
        }

        // First, try to find contiguous blocks
        for (size_t i = 0; i <= blockPointers_.size() - blocksNeeded; ++i)
        {
            if (isFree(i, blocksNeeded))
            {
                markAsUsed(i, blocksNeeded);
                return blockPointers_[i];
            }
        }

        // Fallback: If contiguous allocation is not possible, handle the error or alternative logic
        std::cerr << "Allocation error: Not enough contiguous blocks, consider implementing non-contiguous allocation" << std::endl;
        return nullptr;
    }

    void deallocate(void *block, size_t size)
    {
        SpinlockGuard guard(lockFlag_, owner_);
        size_t blockIndex = (static_cast<char *>(block) - static_cast<char *>(memoryPool_)) / blockSize_;
        size_t blocksToFree = (size + blockSize_ - 1) / blockSize_;
        std::cout << "Deallocating from index: " << blockIndex << ", blocks to free: " << blocksToFree << std::endl;

        markAsFree(blockIndex, blocksToFree);
        mergeFreeBlocks();
    }

private:
    size_t blockSize_;
    size_t totalPoolSizeInBytes_;
    void *memoryPool_;
    std::vector<void *> blockPointers_;
    std::vector<bool> blockStatus_;
    std::atomic_flag lockFlag_;
    std::atomic<std::thread::id> owner_;

    void validateParameters()
    {
        if (blockSize_ == 0 || totalPoolSizeInBytes_ == 0 || blockSize_ > totalPoolSizeInBytes_)
        {
            throw std::invalid_argument("Invalid block size or total pool size");
        }
    }

    void initializePool()
    {
        SpinlockGuard guard(lockFlag_, owner_);
        std::cout << "initializePool size: " << totalPoolSizeInBytes_ << ", blocks size: " << blockSize_ << std::endl;
        size_t numberOfBlocks = totalPoolSizeInBytes_ / blockSize_;
        std::cout << "numberOfBlocks size: " << numberOfBlocks << std::endl;
        memoryPool_ = operator new[](totalPoolSizeInBytes_);
        blockPointers_.reserve(numberOfBlocks);
        blockStatus_.resize(numberOfBlocks, false);

        for (size_t i = 0; i < numberOfBlocks; ++i)
        {
            blockPointers_.push_back(static_cast<char *>(memoryPool_) + i * blockSize_);
        }
    }

    void markAsFree(size_t startIndex, size_t blocksNeeded)
    {
        std::cout << "Marking blocks as free from index: " << startIndex << ", number of blocks: " << blocksNeeded << std::endl;
        std::fill(blockStatus_.begin() + startIndex, blockStatus_.begin() + startIndex + blocksNeeded, false);
    }

    void mergeFreeBlocks()
    {
        size_t startIndex = 0;
        while (startIndex < blockStatus_.size())
        {
            if (blockStatus_[startIndex])
            {
                startIndex++;
                continue;
            }

            size_t endIndex = startIndex;
            while (endIndex < blockStatus_.size() && !blockStatus_[endIndex])
            {
                endIndex++;
            }

            startIndex = endIndex;
        }
    }

    bool isFree(size_t startIndex, size_t blocksNeeded)
    {
        if (startIndex + blocksNeeded > blockStatus_.size())
        {
            return false; // Out of bounds
        }
        for (size_t i = startIndex; i < startIndex + blocksNeeded; ++i)
        {
            if (blockStatus_[i])
            {
                return false;
            }
        }
        return true;
    }

    void markAsUsed(size_t startIndex, size_t blocksNeeded)
    {
        std::fill(blockStatus_.begin() + startIndex, blockStatus_.begin() + startIndex + blocksNeeded, true);
    }

    // Disallow copying and assignment.
    MemoryAllocator(const MemoryAllocator &) = delete;
    MemoryAllocator &operator=(const MemoryAllocator &) = delete;
};

// The main function remains the same

void allocatorThread(MemoryAllocator &allocator, size_t allocationsPerThread)
{
    for (size_t i = 0; i < allocationsPerThread; ++i)
    {
        // Example allocation size
        size_t size = (i % 10 + 1) * 100;

        // Allocate and immediately deallocate
        void *memory = allocator.allocate(size);
        if (memory)
        {
            allocator.deallocate(memory, size);
        }
    }
}

void testCustomAllocator(size_t blockSize, size_t totalPoolSize, size_t allocationsCount)
{
    MemoryAllocator allocator(blockSize, totalPoolSize);

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < allocationsCount; ++i)
    {
        size_t size = (i % 10 + 1) * 100;
        void *memory = allocator.allocate(size);
        allocator.deallocate(memory, size);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;
    std::cout << "Custom Allocator Time: " << duration.count() << " seconds." << std::endl;
}

void testStandardAllocator(size_t allocationsCount)
{
    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < allocationsCount; ++i)
    {
        size_t size = (i % 10 + 1) * 100;
        void *memory = new char[size];
        delete[] static_cast<char *>(memory);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;
    std::cout << "Standard Allocator Time: " << duration.count() << " seconds." << std::endl;
}

int main()
{
    size_t blockSize = 64;
    size_t totalPoolSize = 1024 * 1024; // 1MB
    size_t allocationsCount = 10000;

    std::cout << "Testing Custom Allocator..." << std::endl;
    testCustomAllocator(blockSize, totalPoolSize, allocationsCount);

    std::cout << "Testing Standard Allocator..." << std::endl;
    testStandardAllocator(allocationsCount);

    return 0;
}
