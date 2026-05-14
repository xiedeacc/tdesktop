/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "msg_filter/background_scanner.h"

#include "core/application.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "msg_filter/msg_filter.h"

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTextStream>

namespace MsgFilter {
namespace {

// 3.3: every 24 hours.
constexpr auto kIntervalSec = qint64(24) * 60 * 60;
// 3.4: at most 10000 newest messages per peer.
constexpr auto kPerPeerCap = std::size_t(10'000);
// Number of items processed per Qt event-loop slice. Kept small so the
// UI stays smooth — the slice is the unit of UI starvation, so a low
// number plus a generous idle gap is what keeps the chat list painting
// while a sweep runs.
constexpr auto kSliceItems = 2;
// Idle delay between back-to-back slices when the system is calm.
constexpr auto kSliceIdleMs = 60;
// Sleep when we throttle for CPU pressure.
constexpr auto kThrottleMs = 2'000;
// 3.1: when system CPU >= 40%, throttle.
constexpr auto kSystemBusyPercent = 40.0;
// 3.2: keep total system CPU below 80%.
constexpr auto kSystemCeilingPercent = 80.0;
// Gap between consecutive per-peer `ReapplyToHistory` calls when the
// sweep finishes. Each call walks every cached message of one history
// and updates UI flags, so we space them out instead of running them
// back-to-back at scan end.
constexpr auto kFlushPeerSpacingMs = 120;
// Wait this long after launch before the very first run kicks in. Lets the
// session log in and load history blocks.
constexpr auto kStartupDelaySec = 60;

[[nodiscard]] QString StatePath() {
	return cWorkingDir() + u"tdata/msg_filter_state.json"_q;
}

[[nodiscard]] QString DebugLogPath() {
	return cWorkingDir() + u"tdata/msg_filter_debug.log"_q;
}

void DebugLog(const QString &msg) {
	auto file = QFile(DebugLogPath());
	if (!file.open(QIODevice::WriteOnly
			| QIODevice::Append
			| QIODevice::Text)) {
		return;
	}
	auto stream = QTextStream(&file);
	stream << QDateTime::currentDateTime().toString(u"hh:mm:ss.zzz"_q)
		<< u" "_q << msg << u"\n"_q;
}

[[nodiscard]] bool ShouldScanPeer(not_null<PeerData*> peer) {
	if (peer->isSelf()) {
		return false;
	}
	// Channels (broadcast + megagroup) and legacy basic chats.
	return peer->isChat() || peer->isChannel();
}

} // namespace

BackgroundScanner &BackgroundScanner::Instance() {
	static auto instance = BackgroundScanner();
	return instance;
}

BackgroundScanner::BackgroundScanner() {
	_runTimer.setSingleShot(true);
	QObject::connect(&_runTimer, &QTimer::timeout, [this] {
		runOnce();
	});
	_sliceTimer.setSingleShot(true);
	QObject::connect(&_sliceTimer, &QTimer::timeout, [this] {
		processNextSlice();
	});
	_flushTimer.setSingleShot(true);
	QObject::connect(&_flushTimer, &QTimer::timeout, [this] {
		if (_reapplyQueue.empty()) {
			return;
		}
		const auto peer = PeerId(_reapplyQueue.front());
		_reapplyQueue.pop_front();
		DebugLog(u"[SCANNER] flush reapply peer=%1 (%2 left)"_q
			.arg(peer.value)
			.arg(_reapplyQueue.size()));
		HashFilter::Instance().reapplyToPeerHistory(peer);
		if (!_reapplyQueue.empty()) {
			_flushTimer.start(kFlushPeerSpacingMs);
		}
	});
}

QString BackgroundScanner::stateFilePath() const {
	return StatePath();
}

void BackgroundScanner::start() {
	loadState();
	scheduleNextRun();
	DebugLog(u"[SCANNER] start: lastRunUnix=%1"_q.arg(_lastRunUnix));
}

void BackgroundScanner::stop() {
	_runTimer.stop();
	_sliceTimer.stop();
	if (_running) {
		_running = false;
		_runningChanges.fire_copy(false);
	}
	_queues.clear();
	setCurrentPeer(PeerId());
}

void BackgroundScanner::setCurrentPeer(PeerId peer) {
	if (_currentPeer == peer) {
		return;
	}
	_currentPeer = peer;
	_currentPeerChanges.fire_copy(_currentPeer);
}

void BackgroundScanner::runNow() {
	if (_running) {
		return;
	}
	_runTimer.stop();
	runOnce();
}

void BackgroundScanner::runFullNow() {
	if (_running) {
		return;
	}
	HashFilter::Instance().clearAllPeerScanRecords();
	// Drop our own last-run timestamp too so finishRun's
	// scheduleNextRun does not bias the schedule into the past.
	_lastRunUnix = 0;
	saveState();
	runNow();
}

void BackgroundScanner::scheduleNextRun() {
	const auto now = QDateTime::currentSecsSinceEpoch();
	auto delaySec = qint64(0);
	if (_lastRunUnix <= 0) {
		delaySec = kStartupDelaySec;
	} else {
		const auto due = _lastRunUnix + kIntervalSec;
		delaySec = (due > now) ? (due - now) : qint64(kStartupDelaySec);
	}
	if (delaySec > kIntervalSec) {
		delaySec = kIntervalSec;
	}
	const auto ms = int(std::min<qint64>(delaySec * 1000,
		std::numeric_limits<int>::max()));
	_runTimer.start(ms);
	DebugLog(u"[SCANNER] next run in %1s"_q.arg(delaySec));
}

void BackgroundScanner::runOnce() {
	if (_running) {
		return;
	}
	_running = true;
	_runningChanges.fire_copy(true);
	_processedTotal = 0;
	_matchedTotal = 0;
	_peersTotal = 0;
	_queues.clear();
	_pendingReapply.clear();
	_reapplyQueue.clear();
	_flushTimer.stop();
	_cpuSampler.sample(); // prime baseline
	enumerateAll();
	DebugLog(u"[SCANNER] runOnce begin: peers=%1"_q.arg(_peersTotal));
	if (_queues.empty()) {
		finishRun();
		return;
	}
	setCurrentPeer(_queues.front().peer);
	processNextSlice();
}

void BackgroundScanner::enumerateAll() {
	auto &filter = HashFilter::Instance();
	const auto &accounts = Core::App().domain().accounts();
	for (const auto &[index, account] : accounts) {
		const auto session = account->maybeSession();
		if (!session) {
			continue;
		}
		const auto list = session->data().chatsList(nullptr);
		if (!list || !list->indexed()) {
			continue;
		}
		for (const auto &row : list->indexed()->all()) {
			const auto history = row->history();
			if (!history) {
				continue;
			}
			const auto peer = history->peer;
			if (!ShouldScanPeer(peer)) {
				continue;
			}
			auto queue = ScanQueue();
			queue.session = session;
			queue.peer = peer->id;
			queue.peerName = peer->name();
			queue.ids.reserve(std::min<std::size_t>(
				kPerPeerCap,
				history->blocks.empty() ? 0 : history->blocks.size() * 32));
			// Walk newest -> oldest.
			for (auto blockIt = history->blocks.crbegin();
					blockIt != history->blocks.crend()
						&& queue.ids.size() < kPerPeerCap;
					++blockIt) {
				const auto &messages = (*blockIt)->messages;
				for (auto msgIt = messages.crbegin();
						msgIt != messages.crend()
							&& queue.ids.size() < kPerPeerCap;
						++msgIt) {
					if (const auto item = (*msgIt)->data()) {
						const auto fullId = item->fullId();
						queue.ids.push_back(fullId);
						if (fullId.msg.bare > queue.newestMsgId) {
							queue.newestMsgId = fullId.msg.bare;
						}
					}
				}
			}
			if (queue.ids.empty()) {
				continue;
			}
			// 3.x optimisation: if this peer has not received a new
			// message id since the last completed scan, skip it
			// entirely and keep our prior progress mark.
			const auto prior = filter.peerScanRecord(queue.peer);
			if (queue.newestMsgId > 0
				&& prior.lastScannedMsgId >= queue.newestMsgId) {
				DebugLog(u"[SCANNER] skip name=\"%1\" peer=%2 newest=%3 lastSeen=%4"_q
					.arg(queue.peerName)
					.arg(queue.peer.value)
					.arg(queue.newestMsgId)
					.arg(prior.lastScannedMsgId));
				continue;
			}
			++_peersTotal;
			_queues.push_back(std::move(queue));
		}
	}
}

void BackgroundScanner::processNextSlice() {
	if (!_running) {
		return;
	}
	_cpuSampler.sample();
	const auto sysPct = _cpuSampler.systemPercent();
	const auto procPct = _cpuSampler.processPercent();

	const auto systemBusy = sysPct >= kSystemBusyPercent;
	const auto wouldOverflow = (sysPct + procPct) >= kSystemCeilingPercent;
	if (systemBusy || wouldOverflow) {
		DebugLog(u"[SCANNER] throttle sys=%1%% proc=%2%% reason=%3"_q
			.arg(QString::number(sysPct, 'f', 1))
			.arg(QString::number(procPct, 'f', 1))
			.arg(systemBusy ? u"busy"_q : u"ceiling"_q));
		_sliceTimer.start(kThrottleMs);
		return;
	}

	auto &filter = HashFilter::Instance();
	auto count = 0;
	while (count < kSliceItems && !_queues.empty()) {
		auto &queue = _queues.front();
		if (queue.cursor >= queue.ids.size()) {
			DebugLog(u"[SCANNER] peer done name=\"%1\" peer=%2 scanned=%3 newest=%4"_q
				.arg(queue.peerName)
				.arg(queue.peer.value)
				.arg(queue.cursor)
				.arg(queue.newestMsgId));
			filter.updatePeerScanRecord(queue.peer, {
				.lastFinishUnix = QDateTime::currentSecsSinceEpoch(),
				.lastScannedMsgId = queue.newestMsgId,
			});
			_queues.pop_front();
			setCurrentPeer(
				_queues.empty() ? PeerId() : _queues.front().peer);
			continue;
		}
		if (queue.peer != _currentPeer) {
			setCurrentPeer(queue.peer);
		}
		const auto fullId = queue.ids[queue.cursor++];
		++count;
		++_processedTotal;
		const auto session = queue.session;
		if (!session) {
			continue;
		}
		const auto item = session->data().message(fullId);
		if (!item) {
			continue;
		}
		const auto match = filter.matchInBackground(item);
		if (match.hide) {
			++_matchedTotal;
			const auto peerId = item->history()->peer->id;
			DebugLog(u"[SCANNER] match peer=%1 msgId=%2 reason=%3"_q
				.arg(peerId.value)
				.arg(item->fullId().msg.bare)
				.arg(match.reason));
			// Deferred reapply: stage the per-peer UI sweep so we don't
			// walk every cached message and trigger UI resize per match.
			filter.promoteToRealtime(item, match, /*reapplyImmediately=*/false);
			_pendingReapply.insert(peerId.value);
		}
	}

	if (_queues.empty()) {
		finishRun();
		return;
	}
	_sliceTimer.start(kSliceIdleMs);
}

void BackgroundScanner::finishRun() {
	DebugLog(u"[SCANNER] runOnce done processed=%1 matched=%2 peers=%3 reapplyPeers=%4"_q
		.arg(_processedTotal)
		.arg(_matchedTotal)
		.arg(_peersTotal)
		.arg(_pendingReapply.size()));
	_lastRunUnix = QDateTime::currentSecsSinceEpoch();
	saveState();
	_running = false;
	_runningChanges.fire_copy(false);
	setCurrentPeer(PeerId());
	// Persist the new RealTime entries once for the whole sweep instead
	// of once per match.
	HashFilter::Instance().saveStateToDefault();
	// Stage the per-peer UI sweeps. Each tick of `_flushTimer` walks one
	// peer's cached history; spacing them avoids back-to-back layout
	// thrash when there are many matched peers.
	_reapplyQueue.clear();
	for (const auto value : _pendingReapply) {
		_reapplyQueue.push_back(value);
	}
	_pendingReapply.clear();
	if (!_reapplyQueue.empty()) {
		_flushTimer.start(kFlushPeerSpacingMs);
	}
	scheduleNextRun();
}

void BackgroundScanner::loadState() {
	auto file = QFile(stateFilePath());
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return;
	}
	const auto bytes = file.readAll();
	file.close();
	auto error = QJsonParseError();
	const auto doc = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError || !doc.isObject()) {
		return;
	}
	const auto obj = doc.object();
	const auto v = obj.value(u"lastRunUnix"_q);
	if (v.isDouble()) {
		_lastRunUnix = qint64(v.toDouble());
	} else if (v.isString()) {
		_lastRunUnix = v.toString().toLongLong();
	}
}

void BackgroundScanner::saveState() {
	auto obj = QJsonObject();
	obj.insert(u"lastRunUnix"_q, QString::number(_lastRunUnix));
	auto file = QFile(stateFilePath());
	if (!file.open(QIODevice::WriteOnly
			| QIODevice::Truncate
			| QIODevice::Text)) {
		return;
	}
	file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
}

} // namespace MsgFilter
