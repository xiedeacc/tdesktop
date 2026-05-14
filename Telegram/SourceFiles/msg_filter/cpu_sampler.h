/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

namespace MsgFilter {

// Cheap process+system CPU sampler used by the background detective task to
// honour 3.1 (sleep when system > 40%) and 3.2 (keep total <= 80%).
//
// Call sample() to refresh; deltas are measured against the previous call,
// so the very first call after construction returns 0 / 0 and just primes
// the baseline.
//
// Linux / macOS implementations are deliberately stubbed to 0% so the
// scanner keeps running there. Adding /proc/stat support later is local.
class CpuSampler final {
public:
	CpuSampler();

	void sample();

	[[nodiscard]] double systemPercent() const {
		return _systemPercent;
	}
	[[nodiscard]] double processPercent() const {
		return _processPercent;
	}

private:
#ifdef Q_OS_WIN
	FILETIME _lastIdle{};
	FILETIME _lastKernel{};
	FILETIME _lastUser{};
	FILETIME _lastProcKernel{};
	FILETIME _lastProcUser{};
	bool _primed = false;
#endif // Q_OS_WIN
	double _systemPercent = 0.0;
	double _processPercent = 0.0;
};

} // namespace MsgFilter
