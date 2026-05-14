/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_msg_id.h"
#include "data/data_peer_id.h"
#include "msg_filter/cpu_sampler.h"

#include <rpl/event_stream.h>
#include <rpl/producer.h>

#include <QtCore/QString>
#include <QtCore/QTimer>

#include <deque>
#include <unordered_set>
#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace MsgFilter {

// Scheduled background ad-detective task (3.x in the spec).
//
//   * Runs once every 24h, with the last-run timestamp persisted in
//     tdata/msg_filter_state.json so the cycle survives restarts.
//   * Walks every channel / group on every account, snapshotting the
//     newest 10000 message ids loaded in memory per peer.
//   * Slices the work in small batches that yield back to the Qt event
//     loop, sampling CPU between slices and sleeping when the system is
//     busy (3.1, 3.2).
//   * Per item, it asks the HashFilter to match against the BackGround
//     stash; on a hit, the matched signature is promoted into RealTime so
//     the regular real-time path takes over from then on.
class BackgroundScanner final {
public:
	[[nodiscard]] static BackgroundScanner &Instance();

	void start();
	void stop();
	void runNow();
	// Wipes per-peer scan progress before kicking a run, so every
	// channel/group gets re-enumerated even if its newest message id
	// hasn't advanced since the last scan. Useful as a manual "rescan
	// everything" trigger from the UI.
	void runFullNow();

	// Live progress surface for UI consumers (e.g. the dialogs list,
	// which draws a "scanning" badge plus a dot on the row whose peer
	// is currently being inspected).
	[[nodiscard]] bool running() const { return _running; }
	[[nodiscard]] PeerId currentPeer() const { return _currentPeer; }
	[[nodiscard]] int peersTotal() const { return _peersTotal; }
	[[nodiscard]] int processedTotal() const { return _processedTotal; }
	[[nodiscard]] int matchedTotal() const { return _matchedTotal; }
	[[nodiscard]] rpl::producer<bool> runningChanges() const {
		return _runningChanges.events();
	}
	[[nodiscard]] rpl::producer<PeerId> currentPeerChanges() const {
		return _currentPeerChanges.events();
	}

	BackgroundScanner(const BackgroundScanner &) = delete;
	BackgroundScanner &operator=(const BackgroundScanner &) = delete;

private:
	struct ScanQueue {
		Main::Session *session = nullptr;
		PeerId peer;
		QString peerName;
		std::vector<FullMsgId> ids; // newest first
		std::size_t cursor = 0;
		int64 newestMsgId = 0; // max bare msg id seen on this peer.
	};

	BackgroundScanner();

	void scheduleNextRun();
	void runOnce();
	void enumerateAll();
	void processNextSlice();
	void finishRun();

	void loadState();
	void saveState();

	[[nodiscard]] QString stateFilePath() const;

	void setCurrentPeer(PeerId peer);

	QTimer _runTimer;
	QTimer _sliceTimer;
	QTimer _flushTimer;
	std::deque<ScanQueue> _queues;
	bool _running = false;
	int64 _lastRunUnix = 0;
	int _processedTotal = 0;
	int _matchedTotal = 0;
	int _peersTotal = 0;
	PeerId _currentPeer;
	rpl::event_stream<bool> _runningChanges;
	rpl::event_stream<PeerId> _currentPeerChanges;
	CpuSampler _cpuSampler;
	// Peers with at least one promoted match in this run; we postpone
	// the (expensive) `ReapplyToHistory` to finishRun and only do it
	// once per peer to avoid hammering the main thread during the sweep.
	std::unordered_set<uint64> _pendingReapply;
	std::deque<uint64> _reapplyQueue;
};

} // namespace MsgFilter
