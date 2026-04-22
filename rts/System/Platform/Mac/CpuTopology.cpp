#include "System/Platform/CpuTopology.h"

#include "System/Log/ILog.h"

#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

// macOS does not expose per-core affinity like Linux/Windows do. Apple Silicon
// machines have P/E cores, but the scheduler handles placement; the best we can
// do is report the logical CPU count so spring can size its thread pools.

namespace cpu_topology {

namespace {
int GetLogicalCpuCount()
{
	int count = 0;
	size_t len = sizeof(count);
	if (sysctlbyname("hw.logicalcpu", &count, &len, nullptr, 0) == 0 && count > 0)
		return count;

	const long n = sysconf(_SC_NPROCESSORS_ONLN);
	return (n > 0) ? static_cast<int>(n) : 1;
}

int GetPerformanceCpuCount()
{
	int count = 0;
	size_t len = sizeof(count);
	if (sysctlbyname("hw.perflevel0.logicalcpu", &count, &len, nullptr, 0) == 0 && count > 0)
		return count;
	return GetLogicalCpuCount();
}

uint32_t GetL2CacheSize()
{
	uint64_t size = 0;
	size_t len = sizeof(size);
	if (sysctlbyname("hw.l2cachesize", &size, &len, nullptr, 0) == 0)
		return static_cast<uint32_t>(size);
	return 0;
}

uint32_t GetL3CacheSize()
{
	uint64_t size = 0;
	size_t len = sizeof(size);
	if (sysctlbyname("hw.l3cachesize", &size, &len, nullptr, 0) == 0)
		return static_cast<uint32_t>(size);
	return 0;
}
} // namespace

ThreadPinPolicy GetThreadPinPolicy()
{
	// macOS decides core placement itself; spring should not try to pin.
	return THREAD_PIN_POLICY_NONE;
}

ProcessorMasks GetProcessorMasks()
{
	ProcessorMasks masks;
	const int logical = GetLogicalCpuCount();
	const int perf = std::min(GetPerformanceCpuCount(), logical);

	// Mark the first `perf` cores as performance, the remaining logical as efficiency.
	for (int cpu = 0; cpu < perf && cpu < 32; ++cpu)
		masks.performanceCoreMask |= (1u << cpu);
	for (int cpu = perf; cpu < logical && cpu < 32; ++cpu)
		masks.efficiencyCoreMask |= (1u << cpu);

	LOG("macOS CPU topology: %d logical cores (%d performance).", logical, perf);
	return masks;
}

ProcessorCaches GetProcessorCache()
{
	ProcessorCaches caches;
	const uint32_t l2 = GetL2CacheSize();
	const uint32_t l3 = GetL3CacheSize();
	const int logical = GetLogicalCpuCount();

	ProcessorGroupCaches group;
	group.cacheSizes[1] = l2;
	group.cacheSizes[2] = l3;
	for (int cpu = 0; cpu < logical && cpu < 32; ++cpu)
		group.groupMask |= (1u << cpu);

	caches.groupCaches.push_back(group);
	return caches;
}

} // namespace cpu_topology
