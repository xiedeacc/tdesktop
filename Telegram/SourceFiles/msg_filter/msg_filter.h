/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_map.h"
#include "base/flat_set.h"
#include "data/data_msg_id.h"
#include "data/data_peer_id.h"

class HistoryItem;
class History;

namespace Data {
class PhotoMedia;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace MsgFilter {

class HashFilter final {
public:
	[[nodiscard]] static HashFilter &Instance();

	struct UrlEntry {
		QString text;
		QString uri;

		friend bool operator<(const UrlEntry &a, const UrlEntry &b) {
			return std::tie(a.text, a.uri)
				< std::tie(b.text, b.uri);
		}
		friend bool operator==(const UrlEntry &a, const UrlEntry &b) {
			return a.text == b.text && a.uri == b.uri;
		}
	};

	struct MatchResult {
		bool hide = false;
		QString reason;
		uint64 photoId = 0;
		uint64 videoId = 0;
		uint64 audioId = 0;
		uint64 docId = 0;
		std::vector<UrlEntry> urls;
	};

	// Per-peer background scan progress, persisted under BackGround so
	// the next scan can skip peers that didn't get any newer messages.
	struct PeerScanRecord {
		int64 lastFinishUnix = 0;
		int64 lastScannedMsgId = 0;
	};

	void loadFromDefaultLocation();

	static void applyToItem(not_null<HistoryItem*> item);

	void appendToFilter(not_null<HistoryItem*> item);

	// Stage 2 background detective task surface.
	[[nodiscard]] MatchResult matchInBackground(
		not_null<HistoryItem*> item) const;
	// `reapplyImmediately = false` lets the background scanner queue up
	// many promotions across one sweep and apply the visible result via
	// a single per-peer `reapplyToPeerHistory()` at the end, instead of
	// thrashing the UI thread by walking every cached message on every
	// match.
	void promoteToRealtime(
		not_null<HistoryItem*> item,
		const MatchResult &result,
		bool reapplyImmediately = true);
	// Walks every loaded history of the given peer and re-evaluates each
	// cached message against the current RealTime entry. Called by the
	// background scanner once per affected peer at the end of a sweep.
	void reapplyToPeerHistory(PeerId peer);

	// Convenience: persist the current state to the default filter
	// location. Used by the background scanner to flush all of one
	// sweep's promoted matches in a single write at the end of the run.
	void saveStateToDefault();

	// Per-peer "disable ad filter" toggle. When set, every message of
	// that peer is forced visible (any prior hide is undone) and no new
	// matches are applied for that peer.
	[[nodiscard]] bool isDisabledFor(PeerId peer) const;
	void setDisabledFor(PeerId peer, bool disabled);

	// Per-peer dedup toggle
	[[nodiscard]] bool isDedupEnabledFor(PeerId peer) const;
	void setDedupEnabledFor(PeerId peer, bool enabled);

	// Per-peer background-scan progress accessors used by the scanner.
	[[nodiscard]] PeerScanRecord peerScanRecord(PeerId peer) const;
	void updatePeerScanRecord(PeerId peer, PeerScanRecord record);
	// Wipes every PeerScans entry. Used by the manual "Force full ad
	// scan" trigger so the next run re-enumerates every peer regardless
	// of prior progress.
	void clearAllPeerScanRecords();

private:
	// RealTime entry: per-peer rules used while displaying messages.
	// Keeps the categories from the previous schema and adds Url.
	struct PerPeer {
		base::flat_set<QString> rawTexts;
		base::flat_set<uint64> photoIds;
		base::flat_set<uint64> videoIds;
		base::flat_set<uint64> audioIds;
		base::flat_set<uint64> docIds;
		base::flat_set<UrlEntry> urls;
		// User-toggled per-peer kill switch ("Disable ad filter").
		// When true, applyPerPeer is bypassed and the per-peer entry is
		// kept around even with no other content so the toggle survives
		// reloads.
		bool disabled = false;

		[[nodiscard]] bool emptyContent() const {
			return rawTexts.empty()
				&& photoIds.empty()
				&& videoIds.empty()
				&& audioIds.empty()
				&& docIds.empty()
				&& urls.empty();
		}

		[[nodiscard]] bool empty() const {
			return emptyContent() && !disabled;
		}
	};

	// BackGround stash: raw signals collected from "Mark as Garbage" calls
	// across all peers. Consumed by the (Stage 2) background detective task.
	//
	// Domain is a flat list of normalized hostnames; perceptual photo
	// fingerprints live independently in PerpetualPhotoHash, mapping every
	// known dHash to the set of exact-bytes blake3 fingerprints that share
	// (or are close to sharing) it. The scanner cascade is:
	//   any msg domain in `domains`
	//     AND msg's photo dHash within Hamming<=6 of some key in
	//       `phashToBlake3`
	//     AND msg's photo blake3 contained in that key's value set.
	struct BackGround {
		base::flat_set<QString> rawTexts;
		base::flat_set<uint64> photoIds;
		base::flat_set<uint64> videoIds;
		base::flat_set<uint64> audioIds;
		base::flat_set<uint64> docIds;
		base::flat_set<UrlEntry> urls;
		base::flat_set<QString> domains;
		base::flat_map<uint64, base::flat_set<QByteArray>> phashToBlake3;
		// Per-peer scanner progress so the next scheduled run can skip
		// peers whose newest in-memory message id is still <=
		// lastScannedMsgId.
		base::flat_map<PeerId, PeerScanRecord> peerScans;

		[[nodiscard]] bool empty() const {
			return rawTexts.empty()
				&& photoIds.empty()
				&& videoIds.empty()
				&& audioIds.empty()
				&& docIds.empty()
				&& urls.empty()
				&& domains.empty()
				&& phashToBlake3.empty()
				&& peerScans.empty();
		}
	};

	struct DedupPeer {
		bool enabled = false;
		base::flat_map<uint64, std::vector<MsgId>> mediaToMsgIds;

		[[nodiscard]] bool empty() const {
			return !enabled && mediaToMsgIds.empty();
		}
	};

	struct Pending {
		FullMsgId itemId;
		std::shared_ptr<Data::PhotoMedia> mediaView;
	};

	[[nodiscard]] bool empty() const;
	[[nodiscard]] const PerPeer *findFor(PeerId peer) const;
	[[nodiscard]] const DedupPeer *findDedupFor(PeerId peer) const;

	void loadFromJsonFile(const QString &path);
	void saveToJsonFile(const QString &path) const;

	bool applyPerPeer(
		not_null<HistoryItem*> item,
		const PerPeer *entry);
		
	bool applyDedup(
		not_null<HistoryItem*> item,
		DedupPeer *entry);

	void schedulePending(
		not_null<HistoryItem*> item,
		std::shared_ptr<Data::PhotoMedia> mediaView);
	void processPending();
	void watchSession(not_null<Main::Session*> session);

	base::flat_map<PeerId, PerPeer> _realtime;
	base::flat_map<PeerId, DedupPeer> _dedup;
	BackGround _background;
	std::vector<Pending> _pending;
	base::flat_set<Main::Session*> _watchedSessions;
	bool _loadedLegacy = false;
};

} // namespace MsgFilter
