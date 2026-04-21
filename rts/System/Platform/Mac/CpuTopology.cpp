#include "System/Platform/CpuTopology.h"

#include <algorithm>
#include <unistd.h>

namespace cpu_topology {

ThreadPinPolicy GetThreadPinPolicy() {
	return THREAD_PIN_POLICY_NONE;
}

ProcessorMasks GetProcessorMasks() {
	ProcessorMasks masks;

	const long onlineCpuCount = std::max<long>(1, sysconf(_SC_NPROCESSORS_ONLN));
	const uint32_t cpuCount = static_cast<uint32_t>(std::min<long>(onlineCpuCount, 32));
	const uint32_t allCoresMask = (cpuCount >= 32) ? 0xFFFFFFFFu : ((1u << cpuCount) - 1u);

	masks.performanceCoreMask = allCoresMask;
	return masks;
}

ProcessorCaches GetProcessorCache() {
	ProcessorCaches caches;
	const auto masks = GetProcessorMasks();

	if (masks.performanceCoreMask != 0) {
		caches.groupCaches.push_back({
			.groupMask = masks.performanceCoreMask,
			.cacheSizes = {0, 0, 0},
		});
	}

	return caches;
}

} // namespace cpu_topology
