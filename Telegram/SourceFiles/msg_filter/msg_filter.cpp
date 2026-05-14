/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "msg_filter/msg_filter.h"

#include "msg_filter/blake3.h"
#include "core/application.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_groups.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "ui/text/text_entity.h"

#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonParseError>
#include <QtCore/QUrl>
#include <QtGui/QImage>

namespace MsgFilter {
namespace {

constexpr auto kRealTimeKey = "RealTime";
constexpr auto kBackGroundKey = "BackGround";
constexpr auto kDedupKey = "DEDUP";

// The filter only acts on group / channel conversations. 1-on-1 user
// chats (including bots) are exempt: we never hide their messages and we
// refuse to record signatures from them.
[[nodiscard]] bool IsFilterablePeer(not_null<PeerData*> peer) {
	return peer->isChat() || peer->isChannel();
}

QString DefaultFilterPath() {
	return cWorkingDir() + u"tdata/msg_filter.json"_q;
}

QString DebugLogPath() {
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

void HideItem(not_null<HistoryItem*> item) {
	if (item->isHiddenByFilter()) {
		return;
	}
	item->setHiddenByFilter(true);
	item->history()->owner().requestItemResize(item);
}

void UnhideItem(not_null<HistoryItem*> item) {
	if (!item->isHiddenByFilter()) {
		return;
	}
	item->setHiddenByFilter(false);
	item->history()->owner().requestItemResize(item);
}

QString PeerKey(PeerId peer) {
	return QString::number(peer.value);
}

PeerId PeerFromKey(const QString &key) {
	auto ok = false;
	const auto raw = key.toULongLong(&ok);
	if (!ok || !raw) {
		return PeerId();
	}
	auto result = PeerId();
	result.value = raw;
	return result;
}

int LoadStringsArray(
		const QJsonValue &value,
		base::flat_set<QString> &out) {
	if (!value.isArray()) {
		return 0;
	}
	const auto array = value.toArray();
	auto added = 0;
	for (const auto &element : array) {
		if (element.isString()) {
			const auto text = element.toString();
			if (!text.isEmpty() && out.emplace(text).second) {
				++added;
			}
		}
	}
	return added;
}

int LoadIdsArray(
		const QJsonValue &value,
		base::flat_set<uint64> &out) {
	if (!value.isArray()) {
		return 0;
	}
	const auto array = value.toArray();
	auto added = 0;
	for (const auto &element : array) {
		auto ok = false;
		auto id = uint64(0);
		if (element.isString()) {
			id = element.toString().toULongLong(&ok);
		} else if (element.isDouble()) {
			id = uint64(element.toDouble());
			ok = (id != 0);
		}
		if (ok && id && out.emplace(id).second) {
			++added;
		}
	}
	return added;
}

QJsonArray IdsToArray(const base::flat_set<uint64> &ids) {
	auto array = QJsonArray();
	for (const auto id : ids) {
		array.append(QString::number(id));
	}
	return array;
}

QJsonArray TextsToArray(const base::flat_set<QString> &texts) {
	auto array = QJsonArray();
	for (const auto &text : texts) {
		array.append(text);
	}
	return array;
}

QJsonArray UrlsToArray(
		const base::flat_set<HashFilter::UrlEntry> &urls) {
	auto array = QJsonArray();
	for (const auto &u : urls) {
		auto obj = QJsonObject();
		obj.insert(u"text"_q, u.text);
		obj.insert(u"uri"_q, u.uri);
		array.append(obj);
	}
	return array;
}

int LoadUrls(
		const QJsonValue &value,
		base::flat_set<HashFilter::UrlEntry> &out) {
	if (!value.isArray()) {
		return 0;
	}
	const auto array = value.toArray();
	auto added = 0;
	for (const auto &element : array) {
		if (!element.isObject()) {
			continue;
		}
		const auto obj = element.toObject();
		auto entry = HashFilter::UrlEntry{
			.text = obj.value(u"text"_q).toString(),
			.uri = obj.value(u"uri"_q).toString(),
		};
		if (entry.text.isEmpty() && entry.uri.isEmpty()) {
			continue;
		}
		if (out.emplace(std::move(entry)).second) {
			++added;
		}
	}
	return added;
}

enum class DocKind {
	Video,
	Audio,
	Other,
};

[[nodiscard]] DocKind ClassifyDocument(not_null<DocumentData*> document) {
	if (document->isVideoFile()
		|| document->isVideoMessage()
		|| document->isAnimation()
		|| document->isGifv()) {
		return DocKind::Video;
	}
	if (document->isVoiceMessage()
		|| document->isSong()
		|| document->isAudioFile()) {
		return DocKind::Audio;
	}
	return DocKind::Other;
}

[[nodiscard]] QString DocKindLabel(DocKind kind) {
	switch (kind) {
	case DocKind::Video: return u"video"_q;
	case DocKind::Audio: return u"audio"_q;
	case DocKind::Other: return u"document"_q;
	}
	return u"document"_q;
}

void ReapplyToHistory(not_null<History*> history) {
	for (const auto &block : history->blocks) {
		for (const auto &view : block->messages) {
			HashFilter::applyToItem(view->data());
		}
	}
}

void UnhideAllInHistory(not_null<History*> history) {
	for (const auto &block : history->blocks) {
		for (const auto &view : block->messages) {
			UnhideItem(view->data());
		}
	}
}

// --- url / domain extraction --------------------------------------------

[[nodiscard]] QString NormalizeDomain(QString rawUrl) {
	if (rawUrl.isEmpty()) {
		return QString();
	}
	if (!rawUrl.contains(u"://"_q)) {
		rawUrl = u"http://"_q + rawUrl;
	}
	auto url = QUrl::fromUserInput(rawUrl);
	auto host = url.host(QUrl::FullyDecoded).toLower();
	if (host.isEmpty()) {
		return QString();
	}
	if (host.startsWith(u"www."_q)) {
		host = host.mid(4);
	}
	// Treat each Telegram channel as its own "domain" so that t.me
	// links to distinct channels never get conflated under a single
	// "t.me" bucket.
	if (host == u"t.me"_q || host == u"telegram.me"_q) {
		const auto path = url.path();
		const auto segments = path.split('/', Qt::SkipEmptyParts);
		if (!segments.isEmpty()) {
			return u"t.me/"_q + segments.first().toLower();
		}
	}
	return host;
}

[[nodiscard]] std::vector<HashFilter::UrlEntry> ExtractUrls(
		not_null<HistoryItem*> item) {
	auto result = std::vector<HashFilter::UrlEntry>();
	const auto &textData = item->originalText();
	const auto &text = textData.text;
	for (const auto &entity : textData.entities) {
		const auto type = entity.type();
		auto rawText = QString();
		auto rawUri = QString();
		if (type == EntityType::Url) {
			rawText = text.mid(entity.offset(), entity.length());
			rawUri = rawText;
		} else if (type == EntityType::CustomUrl) {
			rawText = text.mid(entity.offset(), entity.length());
			rawUri = entity.data();
		} else {
			continue;
		}
		if (rawUri.isEmpty()) {
			continue;
		}
		result.push_back(HashFilter::UrlEntry{
			.text = rawText,
			.uri = rawUri,
		});
	}
	return result;
}

[[nodiscard]] std::vector<QString> ExtractDomains(
		const std::vector<HashFilter::UrlEntry> &urls) {
	auto result = std::vector<QString>();
	for (const auto &u : urls) {
		const auto domain = NormalizeDomain(u.uri);
		if (!domain.isEmpty()) {
			result.push_back(domain);
		}
	}
	std::sort(result.begin(), result.end());
	result.erase(
		std::unique(result.begin(), result.end()),
		result.end());
	return result;
}

// --- hash helpers --------------------------------------------------------

[[nodiscard]] int HammingDistance(uint64 a, uint64 b) {
	auto x = a ^ b;
	auto count = 0;
	while (x) {
		count += int(x & 1ULL);
		x >>= 1;
	}
	return count;
}

constexpr auto kDhashHammingThreshold = 6;

[[nodiscard]] uint64 ComputeDhash(const QImage &image) {
	if (image.isNull()) {
		return 0;
	}
	auto small = image.scaled(
		9,
		8,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation
	).convertToFormat(QImage::Format_Grayscale8);
	if (small.width() < 9 || small.height() < 8) {
		return 0;
	}
	auto hash = uint64(0);
	for (auto y = 0; y < 8; ++y) {
		const auto row = small.constScanLine(y);
		for (auto x = 0; x < 8; ++x) {
			hash = (hash << 1)
				| (uint64(row[x] > row[x + 1] ? 1 : 0));
		}
	}
	return hash;
}

[[nodiscard]] QByteArray ComputeBlake3(const QByteArray &data) {
	if (data.isEmpty()) {
		return QByteArray();
	}
	blake3_hasher hasher;
	blake3_hasher_init(&hasher);
	blake3_hasher_update(
		&hasher,
		data.constData(),
		static_cast<size_t>(data.size()));
	uint8_t hash[BLAKE3_OUT_LEN];
	blake3_hasher_finalize(&hasher, hash, BLAKE3_OUT_LEN);
	return QByteArray(
		reinterpret_cast<const char*>(hash),
		BLAKE3_OUT_LEN).toHex().toLower();
}

[[nodiscard]] QString DhashToHex(uint64 h) {
	return QString::number(h, 16).rightJustified(16, '0');
}

[[nodiscard]] uint64 DhashFromHex(const QString &s) {
	auto ok = false;
	const auto v = s.toULongLong(&ok, 16);
	return ok ? v : 0;
}

struct ComputedHashes {
	uint64 dhash = 0;
	QByteArray blake3hex;

	[[nodiscard]] bool valid() const {
		return dhash != 0 && !blake3hex.isEmpty();
	}
};

[[nodiscard]] ComputedHashes TryComputeHashesNow(
		const std::shared_ptr<Data::PhotoMedia> &mediaView) {
	for (const auto size : { Data::PhotoSize::Small,
			Data::PhotoSize::Thumbnail,
			Data::PhotoSize::Large }) {
		const auto bytes = mediaView->imageBytes(size);
		if (bytes.isEmpty()) {
			continue;
		}
		auto image = QImage();
		if (!image.loadFromData(bytes)) {
			continue;
		}
		auto out = ComputedHashes{
			.dhash = ComputeDhash(image),
			.blake3hex = ComputeBlake3(bytes),
		};
		if (out.valid()) {
			return out;
		}
	}
	return ComputedHashes{};
}

[[nodiscard]] QString StripUrls(QString text) {
	static const QRegularExpression urlRegex(u"(https?://\\S+)"_q, QRegularExpression::CaseInsensitiveOption);
	static const QRegularExpression userRegex(u"(@[a-zA-Z0-9_]+)"_q);
	return text.replace(urlRegex, u""_q).replace(userRegex, u""_q).remove(QChar(0x200B)).trimmed();
}

[[nodiscard]] PhotoData *PickHashablePhoto(not_null<HistoryItem*> item) {
	const auto media = item->media();
	if (!media) {
		return nullptr;
	}
	if (const auto photo = media->photo()) {
		return photo;
	}
	// For a video, the cover is what ads typically reuse.
	return media->videoCover();
}

} // namespace

HashFilter &HashFilter::Instance() {
	static auto instance = HashFilter();
	return instance;
}

bool HashFilter::empty() const {
	return _realtime.empty() && _background.empty();
}

const HashFilter::PerPeer *HashFilter::findFor(PeerId peer) const {
	const auto i = _realtime.find(peer);
	return (i != end(_realtime)) ? &i->second : nullptr;
}

const HashFilter::DedupPeer *HashFilter::findDedupFor(PeerId peer) const {
	const auto i = _dedup.find(peer);
	return (i != end(_dedup)) ? &i->second : nullptr;
}

void HashFilter::loadFromDefaultLocation() {
	loadFromJsonFile(DefaultFilterPath());
}

void HashFilter::loadFromJsonFile(const QString &path) {
	_realtime.clear();
	_background = BackGround();

	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		DebugLog(u"[LOAD] Filter file not found: "_q + path);
		return;
	}
	const auto bytes = file.readAll();
	file.close();

	auto error = QJsonParseError();
	const auto doc = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError) {
		DebugLog(u"[LOAD] JSON parse error in %1: %2"_q
			.arg(path)
			.arg(error.errorString()));
		return;
	}
	if (!doc.isObject()) {
		DebugLog(u"[LOAD] Top-level JSON is not an object."_q);
		return;
	}

	const auto root = doc.object();

	const auto loadPerPeer = [&](
			const QString &key,
			const QJsonObject &entry) {
		const auto peer = PeerFromKey(key);
		if (!peer) {
			DebugLog(u"[LOAD] Skipping invalid peer key: %1"_q.arg(key));
			return;
		}
		auto target = PerPeer();
		// New + legacy field names.
		LoadStringsArray(entry.value(u"RawText"_q), target.rawTexts);
		LoadStringsArray(entry.value(u"text"_q), target.rawTexts);

		LoadIdsArray(entry.value(u"PhotoId"_q), target.photoIds);
		LoadIdsArray(entry.value(u"photo"_q), target.photoIds);
		LoadIdsArray(entry.value(u"VideoId"_q), target.videoIds);
		LoadIdsArray(entry.value(u"video"_q), target.videoIds);
		LoadIdsArray(entry.value(u"AudioId"_q), target.audioIds);
		LoadIdsArray(entry.value(u"audio"_q), target.audioIds);
		LoadIdsArray(entry.value(u"DocumentId"_q), target.docIds);
		LoadIdsArray(entry.value(u"document"_q), target.docIds);
		LoadUrls(entry.value(u"Url"_q), target.urls);
		const auto disabledValue = entry.value(u"Disabled"_q);
		if (disabledValue.isBool()) {
			target.disabled = disabledValue.toBool();
		} else if (disabledValue.isDouble()) {
			target.disabled = (disabledValue.toInt() != 0);
		}

		if (!target.empty()) {
			_realtime.emplace(peer, std::move(target));
		}
	};

	const auto loadBackGround = [&](const QJsonObject &entry) {
		LoadStringsArray(entry.value(u"RawText"_q), _background.rawTexts);
		LoadIdsArray(entry.value(u"PhotoId"_q), _background.photoIds);
		LoadIdsArray(entry.value(u"VideoId"_q), _background.videoIds);
		LoadIdsArray(entry.value(u"AudioId"_q), _background.audioIds);
		LoadIdsArray(entry.value(u"DocumentId"_q), _background.docIds);
		LoadUrls(entry.value(u"Url"_q), _background.urls);

		const auto domainNode = entry.value(u"Domain"_q);
		if (domainNode.isArray()) {
			// New schema: flat list of normalized hostnames.
			for (const auto &v : domainNode.toArray()) {
				if (v.isString()) {
					const auto host = v.toString().toLower();
					if (!host.isEmpty()) {
						_background.domains.emplace(host);
					}
				}
			}
		} else if (domainNode.isObject()) {
			// Legacy: object map of host -> [hashes]. Migrate keys into
			// the flat domain set; pull paired (DHash, Blake3) entries
			// into PerpetualPhotoHash. Bare-string dHash entries lose
			// their blake3 partner so we can only keep them as domains.
			_loadedLegacy = true;
			const auto obj = domainNode.toObject();
			for (auto d = obj.begin(); d != obj.end(); ++d) {
				const auto domain = d.key().toLower();
				if (!domain.isEmpty()) {
					_background.domains.emplace(domain);
				}
				if (!d.value().isArray()) {
					continue;
				}
				for (const auto &v : d.value().toArray()) {
					if (!v.isObject()) {
						continue;
					}
					const auto adObj = v.toObject();
					const auto dhash = DhashFromHex(
						adObj.value(u"DHash"_q).toString());
					const auto blake3 = adObj.value(u"Blake3"_q)
						.toString().toLower().toLatin1();
					if (dhash != 0 && !blake3.isEmpty()) {
						_background.phashToBlake3[dhash]
							.emplace(blake3);
					}
				}
			}
		}

		const auto phashNode = entry.value(u"PerpetualPhotoHash"_q);
		if (phashNode.isObject()) {
			const auto obj = phashNode.toObject();
			for (auto it = obj.begin(); it != obj.end(); ++it) {
				const auto phash = DhashFromHex(it.key());
				if (phash == 0) {
					continue;
				}
				auto &set = _background.phashToBlake3[phash];
				const auto append = [&](const QJsonValue &v) {
					if (!v.isString()) return;
					const auto s = v.toString().toLower().toLatin1();
					if (!s.isEmpty()) set.emplace(s);
				};
				if (it.value().isArray()) {
					for (const auto &v : it.value().toArray()) {
						append(v);
					}
				} else {
					append(it.value());
				}
			}
		}

		const auto scanNode = entry.value(u"PeerScans"_q);
		if (scanNode.isObject()) {
			const auto obj = scanNode.toObject();
			for (auto it = obj.begin(); it != obj.end(); ++it) {
				const auto peer = PeerFromKey(it.key());
				if (!peer || !it.value().isObject()) {
					continue;
				}
				const auto rec = it.value().toObject();
				const auto readInt64 = [&](const QString &key) {
					const auto v = rec.value(key);
					if (v.isDouble()) {
						return int64(v.toDouble());
					} else if (v.isString()) {
						return v.toString().toLongLong();
					}
					return qint64(0);
				};
				auto record = PeerScanRecord();
				record.lastFinishUnix = readInt64(u"LastFinishUnix"_q);
				record.lastScannedMsgId = readInt64(u"LastScannedMsgId"_q);
				if (record.lastFinishUnix > 0
					|| record.lastScannedMsgId > 0) {
					_background.peerScans.emplace(peer, record);
				}
			}
		}
	};

	// Detect schema version. New format has top-level RealTime / BackGround.
	const auto rt = root.value(QString::fromUtf8(kRealTimeKey));
	const auto bg = root.value(QString::fromUtf8(kBackGroundKey));
	const auto dedup = root.value(QString::fromUtf8(kDedupKey));
	if (rt.isObject() || bg.isObject() || dedup.isObject()) {
		if (rt.isObject()) {
			const auto obj = rt.toObject();
			for (auto it = obj.begin(); it != obj.end(); ++it) {
				if (it.value().isObject()) {
					loadPerPeer(it.key(), it.value().toObject());
				}
			}
		}
		if (bg.isObject()) {
			loadBackGround(bg.toObject());
		}
		if (dedup.isObject()) {
			const auto obj = dedup.toObject();
			for (auto it = obj.begin(); it != obj.end(); ++it) {
				const auto peer = PeerFromKey(it.key());
				if (!peer) continue;
				if (it.value().isObject()) {
					const auto enabledValue = it.value().toObject().value(u"Enabled"_q);
					auto enabled = false;
					if (enabledValue.isBool()) {
						enabled = enabledValue.toBool();
					} else if (enabledValue.isDouble()) {
						enabled = (enabledValue.toInt() != 0);
					}
					if (enabled) {
						_dedup[peer].enabled = true;
					}
				}
			}
		}
	} else {
		// Legacy schema: flat peer ids at top + "*" for global.
		_loadedLegacy = true;
		for (auto it = root.begin(); it != root.end(); ++it) {
			if (!it.value().isObject()) continue;
			if (it.key() == u"*"_q) {
				loadBackGround(it.value().toObject());
			} else {
				loadPerPeer(it.key(), it.value().toObject());
			}
		}
	}

	DebugLog(u"[LOAD] realtime=%1 peers; background photos=%2 videos=%3 audios=%4 docs=%5 texts=%6 urls=%7 domains=%8 phashes=%9 legacy=%10"_q
		.arg(_realtime.size())
		.arg(_background.photoIds.size())
		.arg(_background.videoIds.size())
		.arg(_background.audioIds.size())
		.arg(_background.docIds.size())
		.arg(_background.rawTexts.size())
		.arg(_background.urls.size())
		.arg(_background.domains.size())
		.arg(_background.phashToBlake3.size())
		.arg(_loadedLegacy ? 1 : 0));

	if (_loadedLegacy) {
		// Re-emit the file in the new schema so subsequent runs go through
		// the fast path. One-shot migration; further saves are no-ops if
		// nothing else changes between runs.
		DebugLog(u"[LOAD] migrating to new schema..."_q);
		saveToJsonFile(path);
		_loadedLegacy = false;
	}
}

void HashFilter::saveToJsonFile(const QString &path) const {
	const auto perPeerToJson = [](const PerPeer &entry) {
		auto out = QJsonObject();
		if (!entry.rawTexts.empty()) {
			out.insert(u"RawText"_q, TextsToArray(entry.rawTexts));
		}
		if (!entry.photoIds.empty()) {
			out.insert(u"PhotoId"_q, IdsToArray(entry.photoIds));
		}
		if (!entry.videoIds.empty()) {
			out.insert(u"VideoId"_q, IdsToArray(entry.videoIds));
		}
		if (!entry.audioIds.empty()) {
			out.insert(u"AudioId"_q, IdsToArray(entry.audioIds));
		}
		if (!entry.docIds.empty()) {
			out.insert(u"DocumentId"_q, IdsToArray(entry.docIds));
		}
		if (!entry.urls.empty()) {
			out.insert(u"Url"_q, UrlsToArray(entry.urls));
		}
		if (entry.disabled) {
			out.insert(u"Disabled"_q, true);
		}
		return out;
	};

	auto realtimeObj = QJsonObject();
	for (const auto &[peer, entry] : _realtime) {
		if (!entry.empty()) {
			realtimeObj.insert(PeerKey(peer), perPeerToJson(entry));
		}
	}

	auto backgroundObj = QJsonObject();
	if (!_background.rawTexts.empty()) {
		backgroundObj.insert(
			u"RawText"_q,
			TextsToArray(_background.rawTexts));
	}
	if (!_background.photoIds.empty()) {
		backgroundObj.insert(
			u"PhotoId"_q,
			IdsToArray(_background.photoIds));
	}
	if (!_background.videoIds.empty()) {
		backgroundObj.insert(
			u"VideoId"_q,
			IdsToArray(_background.videoIds));
	}
	if (!_background.audioIds.empty()) {
		backgroundObj.insert(
			u"AudioId"_q,
			IdsToArray(_background.audioIds));
	}
	if (!_background.docIds.empty()) {
		backgroundObj.insert(
			u"DocumentId"_q,
			IdsToArray(_background.docIds));
	}
	if (!_background.urls.empty()) {
		backgroundObj.insert(u"Url"_q, UrlsToArray(_background.urls));
	}
	if (!_background.domains.empty()) {
		auto arr = QJsonArray();
		for (const auto &domain : _background.domains) {
			arr.append(domain);
		}
		backgroundObj.insert(u"Domain"_q, arr);
	}
	if (!_background.phashToBlake3.empty()) {
		auto phashObj = QJsonObject();
		for (const auto &[phash, blake3s] : _background.phashToBlake3) {
			auto arr = QJsonArray();
			for (const auto &b : blake3s) {
				arr.append(QString::fromLatin1(b));
			}
			phashObj.insert(DhashToHex(phash), arr);
		}
		backgroundObj.insert(u"PerpetualPhotoHash"_q, phashObj);
	}
	if (!_background.peerScans.empty()) {
		auto scansObj = QJsonObject();
		for (const auto &[peer, record] : _background.peerScans) {
			if (record.lastFinishUnix <= 0
				&& record.lastScannedMsgId <= 0) {
				continue;
			}
			auto rec = QJsonObject();
			rec.insert(
				u"LastFinishUnix"_q,
				QString::number(record.lastFinishUnix));
			rec.insert(
				u"LastScannedMsgId"_q,
				QString::number(record.lastScannedMsgId));
			scansObj.insert(PeerKey(peer), rec);
		}
		if (!scansObj.isEmpty()) {
			backgroundObj.insert(u"PeerScans"_q, scansObj);
		}
	}

	auto root = QJsonObject();
	if (!realtimeObj.isEmpty()) {
		root.insert(QString::fromUtf8(kRealTimeKey), realtimeObj);
	}
	if (!backgroundObj.isEmpty()) {
		root.insert(QString::fromUtf8(kBackGroundKey), backgroundObj);
	}
	
	auto dedupObj = QJsonObject();
	for (const auto &[peer, entry] : _dedup) {
		if (entry.enabled) {
			auto obj = QJsonObject();
			obj.insert(u"Enabled"_q, true);
			dedupObj.insert(PeerKey(peer), obj);
		}
	}
	if (!dedupObj.isEmpty()) {
		root.insert(QString::fromUtf8(kDedupKey), dedupObj);
	}

	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly
			| QIODevice::Truncate
			| QIODevice::Text)) {
		DebugLog(u"[SAVE] Cannot open %1 for writing."_q.arg(path));
		return;
	}
	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

// --- per-peer match (real-time path) ------------------------------------

bool HashFilter::applyPerPeer(
		not_null<HistoryItem*> item,
		const PerPeer *entry) {
	const auto fullId = item->fullId();
	const auto peer = item->history()->peer->id;
	const auto &text = item->originalText().text;
	const auto media = item->media();
	const auto photo = media ? media->photo() : nullptr;
	const auto document = media ? media->document() : nullptr;
	const auto docKind = document
		? ClassifyDocument(document)
		: DocKind::Other;

	DebugLog(u"[CHECK] peer=%1 msgId=%2 photoId=%3 docId=%4 text=\"%5\""_q
		.arg(peer.value)
		.arg(fullId.msg.bare)
		.arg(photo ? photo->id : 0)
		.arg(document ? document->id : 0)
		.arg(text.left(40).replace('\n', ' ')));

	// Match priority: PhotoId > VideoId > Url > RawText > DocId > AudioId.

	auto checkMediaIds = [&](not_null<HistoryItem*> i) -> bool {
		const auto media = i->media();
		const auto photo = media ? media->photo() : nullptr;
		const auto document = media ? media->document() : nullptr;

		bool isTarget = (i->fullId().msg.bare == 1796305 || i->fullId().msg.bare == 1799339);
		if (!isTarget) {
			if (const auto group = i->history()->owner().groups().find(i)) {
				for (const auto &sibling : group->items) {
					if (sibling->fullId().msg.bare == 1796305 || sibling->fullId().msg.bare == 1799339) {
						isTarget = true;
						break;
					}
				}
			}
		}

		if (photo && isTarget) {
			auto mediaView = photo->createMediaView();
			const auto computed = TryComputeHashesNow(mediaView);
			DebugLog(u"[INVESTIGATE-MEDIA] peer=%1 msgId=%2 photoId=%3 dhash=%4 blake3=%5"_q
				.arg(i->history()->peer->id.value)
				.arg(i->fullId().msg.bare)
				.arg(photo->id)
				.arg(computed.dhash)
				.arg(QString::fromLatin1(computed.blake3hex)));
		}

		const auto docKind = document
			? ClassifyDocument(document)
			: DocKind::Other;
			
		// 1. PhotoId.
		if (photo && photo->id && entry->photoIds.contains(photo->id)) {
			DebugLog(u"[HIDE] peer=%1 msgId=%2 reason=photoid_match id=%3"_q
				.arg(peer.value)
				.arg(fullId.msg.bare)
				.arg(photo->id));
			HideItem(item);
			return true;
		}

		// 2. VideoId.
		if (document
				&& document->id
				&& docKind == DocKind::Video
				&& entry->videoIds.contains(document->id)) {
			DebugLog(u"[HIDE] peer=%1 msgId=%2 reason=videoid_match id=%3"_q
				.arg(peer.value)
				.arg(fullId.msg.bare)
				.arg(document->id));
			HideItem(item);
			return true;
		}

		// 5. DocId (non-video, non-audio documents).
		if (document
				&& document->id
				&& docKind == DocKind::Other
				&& entry->docIds.contains(document->id)) {
			DebugLog(u"[HIDE] peer=%1 msgId=%2 reason=docid_match id=%3"_q
				.arg(peer.value)
				.arg(fullId.msg.bare)
				.arg(document->id));
			HideItem(item);
			return true;
		}

		// 6. AudioId.
		if (document
				&& document->id
				&& docKind == DocKind::Audio
				&& entry->audioIds.contains(document->id)) {
			DebugLog(u"[HIDE] peer=%1 msgId=%2 reason=audioid_match id=%3"_q
				.arg(peer.value)
				.arg(fullId.msg.bare)
				.arg(document->id));
			HideItem(item);
			return true;
		}
		
		return false;
	};

	if (checkMediaIds(item)) return true;
	
	// Album-aware: check siblings for media matches
	if (const auto group = item->history()->owner().groups().find(item)) {
		for (const auto &sibling : group->items) {
			if (sibling != item && checkMediaIds(sibling)) {
				return true;
			}
		}
	}

	// 3. Url.
	if (!entry->urls.empty()) {
		auto checkUrls = [&](not_null<HistoryItem*> i) -> bool {
			const auto urls = ExtractUrls(i);
			for (const auto &u : urls) {
				if (entry->urls.contains(u)) {
					DebugLog(u"[HIDE] peer=%1 msgId=%2 reason=url_match uri=%3"_q
						.arg(peer.value)
						.arg(fullId.msg.bare)
						.arg(u.uri));
					HideItem(item);
					return true;
				}
			}
			return false;
		};
		if (checkUrls(item)) return true;
		if (const auto group = item->history()->owner().groups().find(item)) {
			for (const auto &sibling : group->items) {
				if (sibling != item && checkUrls(sibling)) {
					return true;
				}
			}
		}
	}

	// 4. RawText.
	auto checkRawText = [&](not_null<HistoryItem*> i) -> bool {
		const auto &text = i->originalText().text;
		if (!text.isEmpty()) {
			if (entry->rawTexts.contains(text)) {
				DebugLog(u"[HIDE] peer=%1 msgId=%2 reason=rawtext_match"_q
					.arg(peer.value)
					.arg(fullId.msg.bare));
				HideItem(item);
				return true;
			}
			const auto stripped = StripUrls(text);
			if (!stripped.isEmpty() && entry->rawTexts.contains(stripped)) {
				DebugLog(u"[HIDE] peer=%1 msgId=%2 reason=rawtext_stripped_match"_q
					.arg(peer.value)
					.arg(fullId.msg.bare));
				HideItem(item);
				return true;
			}
		}
		return false;
	};
	if (checkRawText(item)) return true;
	if (const auto group = item->history()->owner().groups().find(item)) {
		for (const auto &sibling : group->items) {
			if (sibling != item && checkRawText(sibling)) {
				return true;
			}
		}
	}

	return false;
}

bool HashFilter::applyDedup(
		not_null<HistoryItem*> item,
		DedupPeer *entry) {
	if (!entry || !entry->enabled) {
		return false;
	}
	const auto fullId = item->fullId();
	const auto peer = item->history()->peer->id;

	auto items = HistoryItemsList{ item };
	if (const auto group = item->history()->owner().groups().find(item)) {
		items = group->items;
	}

	if (items.empty()) {
		return false;
	}

	auto firstItem = items.front();
	const auto media = firstItem->media();
	const auto photo = media ? media->photo() : nullptr;
	if (!photo) {
		return false;
	}

	auto mediaView = photo->createMediaView();
	const auto computed = TryComputeHashesNow(mediaView);
	if (!computed.valid()) {
		return false;
	}

	QByteArray mediaId = computed.blake3hex;

	if (fullId.msg.bare == 1796305 || fullId.msg.bare == 1799339) {
		DebugLog(u"[INVESTIGATE-DEDUP] msgId=%1 mediaIds=%2 enabled=%3"_q
			.arg(fullId.msg.bare)
			.arg(QString::fromLatin1(mediaId.toHex()))
			.arg(entry->enabled ? 1 : 0));
	}

	auto it = entry->mediaIdToMsgId.find(mediaId);
	if (it != entry->mediaIdToMsgId.end()) {
		bool isSameMessage = false;
		for (const auto &i : items) {
			if (i->fullId().msg == it->second) {
				isSameMessage = true;
				break;
			}
		}
		if (!isSameMessage) {
			QString itemsList;
			for (const auto &i : items) itemsList += QString::number(i->fullId().msg.bare) + ",";
			DebugLog(u"[DEDUP-HIDE] peer=%1 msgId=%2 mediaId=%3 mappedTo=%4 items=%5"_q
				.arg(peer.value)
				.arg(fullId.msg.bare)
				.arg(QString::fromLatin1(mediaId.toHex()))
				.arg(it->second.bare)
				.arg(itemsList));
			HideItem(item);
			return true;
		}
	} else {
		entry->mediaIdToMsgId.emplace(mediaId, fullId.msg);
		entry->mediaIdsList.push_back(mediaId);
		if (entry->mediaIdsList.size() > 10000) {
			entry->mediaIdToMsgId.erase(entry->mediaIdsList.front());
			entry->mediaIdsList.pop_front();
		}
	}

	return false;
}

void HashFilter::applyToItem(not_null<HistoryItem*> item) {
	const auto peerPtr = item->history()->peer;
	const auto fullId = item->fullId();
	
	if (fullId.msg.bare == 1796305 || fullId.msg.bare == 1799339) {
		const auto media = item->media();
		const auto photo = media ? media->photo() : nullptr;
		DebugLog(u"[INVESTIGATE-GLOBAL] peer=%1 msgId=%2 photoId=%3"_q
			.arg(peerPtr->id.value)
			.arg(fullId.msg.bare)
			.arg(photo ? photo->id : 0));
	}

	if (!IsFilterablePeer(peerPtr)) {
		UnhideItem(item);
		return;
	}
	auto &filter = Instance();
	const auto peer = peerPtr->id;
	const auto entry = filter.findFor(peer);
	
	if (fullId.msg.bare == 1796305 || fullId.msg.bare == 1799339) {
		DebugLog(u"[INVESTIGATE-ENTRY] msgId=%1 hasEntry=%2"_q
			.arg(fullId.msg.bare)
			.arg(entry ? 1 : 0));
	}

	if (entry && entry->disabled) {
		UnhideItem(item);
		return;
	}

	auto &dedupEntry = filter._dedup[peer];
	if (dedupEntry.enabled && filter.applyDedup(item, &dedupEntry)) {
		return; // Handled by dedup
	}

	// 1. Try real-time exact match (fastest)
	if (entry && filter.applyPerPeer(item, entry)) {
		return; // Handled
	}

	// 2. Try global background match
	const auto bgResult = filter.matchInBackground(item);
	if (bgResult.hide) {
		DebugLog(u"[HIDE-GLOBAL] peer=%1 msgId=%2 reason=%3"_q
			.arg(peer.value)
			.arg(fullId.msg.bare)
			.arg(bgResult.reason));
		HideItem(item);
		filter.promoteToRealtime(item, bgResult, false); // Persist entry for future
		return;
	}
}

bool HashFilter::isDisabledFor(PeerId peer) const {
	const auto i = _realtime.find(peer);
	return (i != end(_realtime)) && i->second.disabled;
}

void HashFilter::setDisabledFor(PeerId peer, bool disabled) {
	auto changed = false;
	if (disabled) {
		auto &entry = _realtime[peer];
		if (!entry.disabled) {
			entry.disabled = true;
			changed = true;
		}
	} else {
		const auto i = _realtime.find(peer);
		if (i != end(_realtime) && i->second.disabled) {
			i->second.disabled = false;
			changed = true;
			if (i->second.emptyContent()) {
				_realtime.erase(i);
			}
		}
	}
	if (!changed) {
		return;
	}
	saveToJsonFile(DefaultFilterPath());
	DebugLog(u"[DISABLE] peer=%1 disabled=%2"_q
		.arg(peer.value)
		.arg(disabled ? 1 : 0));
	// Re-evaluate every loaded history of this peer so previously
	// hidden items reappear (when disabling) or freshly arrived items
	// can be re-checked (when re-enabling). Enumerating accounts here
	// avoids relying on _watchedSessions (which is only populated for
	// deferred photo-hash bookkeeping).
	const auto &accounts = Core::App().domain().accounts();
	for (const auto &[index, account] : accounts) {
		const auto session = account->maybeSession();
		if (!session) {
			continue;
		}
		const auto history = session->data().historyLoaded(peer);
		if (!history) {
			continue;
		}
		if (disabled) {
			UnhideAllInHistory(history);
		} else {
			ReapplyToHistory(history);
		}
	}
}

bool HashFilter::isDedupEnabledFor(PeerId peer) const {
	const auto i = _dedup.find(peer);
	return (i != end(_dedup)) && i->second.enabled;
}

void HashFilter::setDedupEnabledFor(PeerId peer, bool enabled) {
	auto changed = false;
	if (enabled) {
		auto &entry = _dedup[peer];
		if (!entry.enabled) {
			entry.enabled = true;
			changed = true;
		}
	} else {
		const auto i = _dedup.find(peer);
		if (i != end(_dedup) && i->second.enabled) {
			i->second.enabled = false;
			changed = true;
			if (i->second.empty()) {
				_dedup.erase(i);
			}
		}
	}
	if (!changed) {
		return;
	}
	saveToJsonFile(DefaultFilterPath());
	DebugLog(u"[DEDUP] peer=%1 enabled=%2"_q
		.arg(peer.value)
		.arg(enabled ? 1 : 0));
	
	const auto &accounts = Core::App().domain().accounts();
	for (const auto &[index, account] : accounts) {
		const auto session = account->maybeSession();
		if (!session) {
			continue;
		}
		const auto history = session->data().historyLoaded(peer);
		if (!history) {
			continue;
		}
		// If disabled, we should unhide items that were hidden by dedup?
		// Wait, items hidden by dedup are just hidden by filter.
		// It's safer to just reapply completely.
		if (!enabled) {
			UnhideAllInHistory(history);
			ReapplyToHistory(history);
		} else {
			ReapplyToHistory(history);
		}
	}
}

HashFilter::PeerScanRecord HashFilter::peerScanRecord(PeerId peer) const {
	const auto i = _background.peerScans.find(peer);
	return (i != end(_background.peerScans)) ? i->second : PeerScanRecord();
}

void HashFilter::updatePeerScanRecord(PeerId peer, PeerScanRecord record) {
	if (record.lastFinishUnix <= 0 && record.lastScannedMsgId <= 0) {
		_background.peerScans.erase(peer);
	} else {
		_background.peerScans[peer] = record;
	}
	saveToJsonFile(DefaultFilterPath());
}

void HashFilter::clearAllPeerScanRecords() {
	if (_background.peerScans.empty()) {
		return;
	}
	DebugLog(u"[RESET] cleared %1 peer scan records"_q
		.arg(_background.peerScans.size()));
	_background.peerScans.clear();
	saveToJsonFile(DefaultFilterPath());
}

// --- background match (Stage 2 detective task) --------------------------

HashFilter::MatchResult HashFilter::matchInBackground(
		not_null<HistoryItem*> item) const {
	auto result = MatchResult();
	if (_background.empty()) {
		return result;
	}
	if (!IsFilterablePeer(item->history()->peer)) {
		// 1-on-1 user chats are out of scope, so the detective task
		// never produces matches for them either.
		return result;
	}

	const auto media = item->media();
	const auto photo = media ? media->photo() : nullptr;
	const auto document = media ? media->document() : nullptr;
	const auto docKind = document
		? ClassifyDocument(document)
		: DocKind::Other;
	const auto &text = item->originalText().text;
	const auto urls = ExtractUrls(item);

	const auto fillMediaIds = [&] {
		if (photo && photo->id) {
			result.photoId = photo->id;
		}
		if (document && document->id) {
			if (docKind == DocKind::Video) {
				result.videoId = document->id;
			} else if (docKind == DocKind::Audio) {
				result.audioId = document->id;
			} else {
				result.docId = document->id;
			}
		}
		result.urls = urls;
	};

	// Match priority: PhotoId > VideoId > Url > DocId > AudioId > RawText.

	auto checkMediaIdsFirst = [&](not_null<HistoryItem*> i) -> bool {
		const auto media = i->media();
		const auto photo = media ? media->photo() : nullptr;
		const auto document = media ? media->document() : nullptr;
		const auto docKind = document ? ClassifyDocument(document) : DocKind::Other;

		// 1. PhotoId
		if (photo && photo->id && _background.photoIds.contains(photo->id)) {
			result.hide = true;
			result.reason = u"bg.PhotoId"_q;
			fillMediaIds();
			return true;
		}

		// 2. VideoId
		if (document && document->id && docKind == DocKind::Video && _background.videoIds.contains(document->id)) {
			result.hide = true;
			result.reason = u"bg.VideoId"_q;
			fillMediaIds();
			return true;
		}
		return false;
	};

	if (checkMediaIdsFirst(item)) return result;
	if (const auto group = item->history()->owner().groups().find(item)) {
		for (const auto &sibling : group->items) {
			if (sibling != item && checkMediaIdsFirst(sibling)) return result;
		}
	}

	// 3. Url
	auto checkUrls = [&](not_null<HistoryItem*> i) -> bool {
		const auto urls = ExtractUrls(i);
		if (!urls.empty()) {
			if (!_background.urls.empty()) {
				for (const auto &u : urls) {
					if (_background.urls.contains(u)) {
						result.hide = true;
						result.reason = u"bg.Url"_q;
						fillMediaIds();
						return true;
					}
				}
			}
		}
		return false;
	};
	
	if (checkUrls(item)) return result;
	if (const auto group = item->history()->owner().groups().find(item)) {
		for (const auto &sibling : group->items) {
			if (sibling != item && checkUrls(sibling)) return result;
		}
	}

	// 3.5 Perceptual Hash + Blake3 check
	auto checkHashes = [&](not_null<HistoryItem*> i) -> bool {
		if (_background.phashToBlake3.empty()) return false;
		if (const auto hashable = PickHashablePhoto(i)) {
			auto mediaView = hashable->createMediaView();
			const auto computed = TryComputeHashesNow(mediaView);
			if (computed.valid()) {
				for (const auto &[phash, blake3s] : _background.phashToBlake3) {
					if (HammingDistance(computed.dhash, phash) > kDhashHammingThreshold) {
						continue;
					}
					if (blake3s.contains(computed.blake3hex)) {
						result.hide = true;
						result.reason = u"bg.PhashBlake3"_q;
						fillMediaIds();
						return true;
					}
				}
			}
		}
		return false;
	};

	if (checkHashes(item)) return result;
	if (const auto group = item->history()->owner().groups().find(item)) {
		for (const auto &sibling : group->items) {
			if (sibling != item && checkHashes(sibling)) return result;
		}
	}

	auto checkMediaIdsSecond = [&](not_null<HistoryItem*> i) -> bool {
		const auto media = i->media();
		const auto document = media ? media->document() : nullptr;
		const auto docKind = document ? ClassifyDocument(document) : DocKind::Other;

		// 4. DocId
		if (document && document->id && docKind == DocKind::Other && _background.docIds.contains(document->id)) {
			result.hide = true;
			result.reason = u"bg.DocId"_q;
			fillMediaIds();
			return true;
		}

		// 5. AudioId
		if (document && document->id && docKind == DocKind::Audio && _background.audioIds.contains(document->id)) {
			result.hide = true;
			result.reason = u"bg.AudioId"_q;
			fillMediaIds();
			return true;
		}
		return false;
	};

	if (checkMediaIdsSecond(item)) return result;
	if (const auto group = item->history()->owner().groups().find(item)) {
		for (const auto &sibling : group->items) {
			if (sibling != item && checkMediaIdsSecond(sibling)) return result;
		}
	}

	// 6. RawText
	auto checkRawText = [&](not_null<HistoryItem*> i) -> bool {
		const auto &text = i->originalText().text;
		if (!text.isEmpty()) {
			if (_background.rawTexts.contains(text)) {
				result.hide = true;
				result.reason = u"bg.RawText"_q;
				fillMediaIds();
				return true;
			}
			const auto stripped = StripUrls(text);
			if (!stripped.isEmpty() && _background.rawTexts.contains(stripped)) {
				result.hide = true;
				result.reason = u"bg.RawTextStripped"_q;
				fillMediaIds();
				return true;
			}
		}
		return false;
	};
	if (checkRawText(item)) return result;
	if (const auto group = item->history()->owner().groups().find(item)) {
		for (const auto &sibling : group->items) {
			if (sibling != item && checkRawText(sibling)) {
				return result;
			}
		}
	}

	return result;
}

void HashFilter::promoteToRealtime(
		not_null<HistoryItem*> item,
		const MatchResult &result,
		bool reapplyImmediately) {
	if (!result.hide) {
		return;
	}
	const auto peer = item->history()->peer->id;
	auto &entry = _realtime[peer];
	auto changed = false;
	if (result.photoId
		&& entry.photoIds.emplace(result.photoId).second) {
		changed = true;
	}
	if (result.videoId
		&& entry.videoIds.emplace(result.videoId).second) {
		changed = true;
	}
	if (result.audioId
		&& entry.audioIds.emplace(result.audioId).second) {
		changed = true;
	}
	if (result.docId
		&& entry.docIds.emplace(result.docId).second) {
		changed = true;
	}
	for (const auto &u : result.urls) {
		if (entry.urls.emplace(u).second) {
			changed = true;
		}
	}
	if (changed) {
		DebugLog(u"[PROMOTE] peer=%1 msgId=%2 reason=%3 photo=%4 video=%5 audio=%6 doc=%7 urls=%8 reapply=%9"_q
			.arg(peer.value)
			.arg(item->fullId().msg.bare)
			.arg(result.reason)
			.arg(result.photoId)
			.arg(result.videoId)
			.arg(result.audioId)
			.arg(result.docId)
			.arg(result.urls.size())
			.arg(reapplyImmediately ? 1 : 0));
		if (reapplyImmediately) {
			// Real-time path: persist + repaint right away.
			saveToJsonFile(DefaultFilterPath());
			ReapplyToHistory(item->history());
		}
		// Deferred path leaves the persistence and the UI sweep to the
		// caller (background scanner) so we don't block the main thread
		// once per match.
	}
}

void HashFilter::reapplyToPeerHistory(PeerId peer) {
	const auto &accounts = Core::App().domain().accounts();
	for (const auto &[index, account] : accounts) {
		const auto session = account->maybeSession();
		if (!session) {
			continue;
		}
		if (const auto history = session->data().historyLoaded(peer)) {
			ReapplyToHistory(history);
		}
	}
}

void HashFilter::saveStateToDefault() {
	saveToJsonFile(DefaultFilterPath());
}

// --- mark-as-garbage -----------------------------------------------------

void HashFilter::appendToFilter(not_null<HistoryItem*> item) {
	const auto history = item->history();
	const auto peerPtr = history->peer;
	if (!IsFilterablePeer(peerPtr)) {
		// Refuse to record signatures from 1-on-1 chats — they should
		// never be filtered, so storing their content would only ever
		// produce false positives in other peers.
		DebugLog(u"[APPEND] peer=%1 skipped: not a group/channel"_q
			.arg(peerPtr->id.value));
		return;
	}
	const auto peer = peerPtr->id;
	auto &entry = _realtime[peer];

	// Album-aware: marking one item registers all album siblings at once.
	auto markedItems = HistoryItemsList{ item };
	if (const auto group = history->owner().groups().find(item)) {
		markedItems = group->items;
	}

	const auto recordPerPeer = [&](not_null<HistoryItem*> i) {
		const auto media = i->media();
		const auto photo = media ? media->photo() : nullptr;
		const auto document = media ? media->document() : nullptr;
		const auto &text = i->originalText().text;
		const auto fullId = i->fullId();
		auto added = false;
		if (photo && photo->id) {
			if (entry.photoIds.emplace(photo->id).second) {
				DebugLog(u"[APPEND] peer=%1 msgId=%2 type=photo id=%3"_q
					.arg(peer.value)
					.arg(fullId.msg.bare)
					.arg(photo->id));
				added = true;
			}
			// Also stash in BackGround so cross-peer scan can find this id.
			_background.photoIds.emplace(photo->id);
		} else if (document && document->id) {
			const auto kind = ClassifyDocument(document);
			auto &target = (kind == DocKind::Video)
				? entry.videoIds
				: (kind == DocKind::Audio)
				? entry.audioIds
				: entry.docIds;
			if (target.emplace(document->id).second) {
				DebugLog(u"[APPEND] peer=%1 msgId=%2 type=%3 id=%4"_q
					.arg(peer.value)
					.arg(fullId.msg.bare)
					.arg(DocKindLabel(kind))
					.arg(document->id));
				added = true;
			}
			auto &bgTarget = (kind == DocKind::Video)
				? _background.videoIds
				: (kind == DocKind::Audio)
				? _background.audioIds
				: _background.docIds;
			bgTarget.emplace(document->id);
		} else if (!text.isEmpty()) {
			if (entry.rawTexts.emplace(text).second) {
				DebugLog(u"[APPEND] peer=%1 msgId=%2 type=rawtext value=\"%3\""_q
					.arg(peer.value)
					.arg(fullId.msg.bare)
					.arg(text.left(60).replace('\n', ' ')));
				added = true;
			}
			_background.rawTexts.emplace(text);

			const auto stripped = StripUrls(text);
			if (!stripped.isEmpty()) {
				entry.rawTexts.emplace(stripped);
				_background.rawTexts.emplace(stripped);
			}
		}
		return added;
	};

	auto perPeerAppended = false;
	for (const auto &sibling : markedItems) {
		if (recordPerPeer(sibling)) {
			perPeerAppended = true;
		}
	}

	// Step A — extract URLs (text + uri) and write to BOTH sections.
	// We extract URLs from all siblings, not just the main item.
	auto urlsTouched = false;
	auto domainsTouched = false;
	auto fingerprintTouched = false;

	for (const auto &sibling : markedItems) {
		const auto urls = ExtractUrls(sibling);
		for (const auto &u : urls) {
			const auto inPeer = entry.urls.emplace(u).second;
			const auto inGlobal = _background.urls.emplace(u).second;
			if (inPeer || inGlobal) {
				DebugLog(u"[APPEND-URL] peer=%1 msgId=%2 text=\"%3\" uri=\"%4\" addedPeer=%5 addedBg=%6"_q
					.arg(peer.value)
					.arg(sibling->fullId().msg.bare)
					.arg(u.text.left(40))
					.arg(u.uri.left(80))
					.arg(inPeer ? 1 : 0)
					.arg(inGlobal ? 1 : 0));
				urlsTouched = true;
			}
		}

		// Step B — register every domain found in the URIs into the BackGround
		// flat domain set.
		const auto domains = ExtractDomains(urls);
		for (const auto &d : domains) {
			if (_background.domains.emplace(d).second) {
				domainsTouched = true;
				DebugLog(u"[APPEND-DOMAIN] peer=%1 msgId=%2 domain=%3"_q
					.arg(peer.value)
					.arg(sibling->fullId().msg.bare)
					.arg(d));
			}
		}

		// Step C — register the marked photo / video cover's perceptual hash
		// and exact-bytes blake3 into BackGround.PerpetualPhotoHash.
		if (const auto hashable = PickHashablePhoto(sibling)) {
			auto mediaView = hashable->createMediaView();
			const auto computed = TryComputeHashesNow(mediaView);
			if (computed.valid()) {
				auto &set = _background.phashToBlake3[computed.dhash];
				if (set.emplace(computed.blake3hex).second) {
					DebugLog(u"[APPEND-AD] dhash=%1 blake3=%2 (immediate)"_q
						.arg(DhashToHex(computed.dhash))
						.arg(QString::fromLatin1(computed.blake3hex)));
					fingerprintTouched = true;
				}
			} else {
				DebugLog(u"[APPEND-AD] msgId=%1 deferring hashes (image not loaded)"_q
					.arg(sibling->fullId().msg.bare));
				hashable->load(
					Data::PhotoSize::Small,
					Data::FileOrigin(sibling->fullId()));
				schedulePending(sibling, std::move(mediaView));
			}
		}
	}

	const auto changed = perPeerAppended
		|| urlsTouched
		|| domainsTouched
		|| fingerprintTouched;
	if (changed) {
		saveToJsonFile(DefaultFilterPath());
		for (const auto &sibling : markedItems) {
			HideItem(sibling);
		}
		ReapplyToHistory(history);
	} else if (entry.empty()) {
		_realtime.erase(peer);
	}
}

// --- pending hash machinery ---------------------------------------------

void HashFilter::schedulePending(
		not_null<HistoryItem*> item,
		std::shared_ptr<Data::PhotoMedia> mediaView) {
	_pending.push_back(Pending{
		.itemId = item->fullId(),
		.mediaView = std::move(mediaView),
	});
	watchSession(&item->history()->session());
}

void HashFilter::watchSession(not_null<Main::Session*> session) {
	if (_watchedSessions.contains(session)) {
		return;
	}
	_watchedSessions.emplace(session);

	session->downloaderTaskFinished(
	) | rpl::start_with_next([this] {
		processPending();
	}, session->lifetime());
}

void HashFilter::processPending() {
	if (_pending.empty()) {
		return;
	}
	auto still = std::vector<Pending>();
	auto savedAny = false;
	for (auto &pending : _pending) {
		const auto computed = TryComputeHashesNow(pending.mediaView);
		if (!computed.valid()) {
			still.push_back(std::move(pending));
			continue;
		}
		auto &set = _background.phashToBlake3[computed.dhash];
		if (set.emplace(computed.blake3hex).second) {
			DebugLog(u"[APPEND-AD] dhash=%1 blake3=%2 (deferred, msgId=%3)"_q
				.arg(DhashToHex(computed.dhash))
				.arg(QString::fromLatin1(computed.blake3hex))
				.arg(pending.itemId.msg.bare));
			savedAny = true;
		}
	}
	if (savedAny) {
		saveToJsonFile(DefaultFilterPath());
	}
	_pending = std::move(still);
}

} // namespace MsgFilter
