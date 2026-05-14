/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "msg_filter/cpu_sampler.h"

namespace MsgFilter {
namespace {

#ifdef Q_OS_WIN
[[nodiscard]] uint64 ToU64(const FILETIME &ft) {
	return (static_cast<uint64>(ft.dwHighDateTime) << 32)
		| static_cast<uint64>(ft.dwLowDateTime);
}
#endif // Q_OS_WIN

} // namespace

CpuSampler::CpuSampler() = default;

void CpuSampler::sample() {
#ifdef Q_OS_WIN
	FILETIME idle, kernel, user;
	if (!GetSystemTimes(&idle, &kernel, &user)) {
		return;
	}
	FILETIME procCreate, procExit, procKernel, procUser;
	if (!GetProcessTimes(GetCurrentProcess(),
			&procCreate, &procExit, &procKernel, &procUser)) {
		return;
	}

	if (!_primed) {
		_lastIdle = idle;
		_lastKernel = kernel;
		_lastUser = user;
		_lastProcKernel = procKernel;
		_lastProcUser = procUser;
		_primed = true;
		return;
	}

	const auto idleDelta = ToU64(idle) - ToU64(_lastIdle);
	const auto kernelDelta = ToU64(kernel) - ToU64(_lastKernel);
	const auto userDelta = ToU64(user) - ToU64(_lastUser);
	const auto totalSys = kernelDelta + userDelta; // sum across all CPUs.
	if (totalSys > 0) {
		const auto used = (totalSys > idleDelta) ? (totalSys - idleDelta) : 0;
		_systemPercent = double(used) * 100.0 / double(totalSys);
	}

	const auto procKernelDelta = ToU64(procKernel)
		- ToU64(_lastProcKernel);
	const auto procUserDelta = ToU64(procUser) - ToU64(_lastProcUser);
	const auto procTotal = procKernelDelta + procUserDelta;
	if (totalSys > 0) {
		// procTotal and totalSys are both summed across all CPUs, so the
		// ratio gives our share of the *whole machine* (0..100).
		_processPercent = double(procTotal) * 100.0 / double(totalSys);
	}

	_lastIdle = idle;
	_lastKernel = kernel;
	_lastUser = user;
	_lastProcKernel = procKernel;
	_lastProcUser = procUser;
#else
	// TODO: /proc/stat sampling on Linux / Mach host_statistics on macOS.
	_systemPercent = 0.0;
	_processPercent = 0.0;
#endif // Q_OS_WIN
}

} // namespace MsgFilter
