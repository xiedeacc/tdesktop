/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "msg_filter/msg_filter.h"

#include "msg_filter/blake3.h"
#include "history/history.h"
#include "history/history_item.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "main/main_session.h"

#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>

namespace MsgFilter {
namespace {

constexpr auto kBlake3HexLen = BLAKE3_OUT_LEN * 2; // 64

QString DefaultFilterPath() {
	return cWorkingDir() + u"tdata/msg_filter.txt"_q;
}

QString DebugLogPath() {
	return cWorkingDir() + u"tdata/msg_filter_debug.log"_q;
}

QByteArray ComputeBlake3(const QByteArray &data) {
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

void DebugLog(const QString &msg) {
	auto file = QFile(DebugLogPath());
	file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
	auto stream = QTextStream(&file);
	stream << QDateTime::currentDateTime().toString(u"hh:mm:ss.zzz"_q)
		<< u" "_q << msg << u"\n"_q;
}

QString EscapeText(const QString &text) {
	auto result = text;
	result.replace('\\', u"\\\\"_q);
	result.replace('\n', u"\\n"_q);
	return result;
}

QString UnescapeText(const QString &text) {
	auto result = QString();
	result.reserve(text.size());
	for (auto i = 0; i < text.size(); ++i) {
		if (text[i] == '\\' && i + 1 < text.size()) {
			const auto next = text[i + 1];
			if (next == 'n') {
				result += '\n';
				++i;
			} else if (next == '\\') {
				result += '\\';
				++i;
			} else {
				result += text[i];
			}
		} else {
			result += text[i];
		}
	}
	return result;
}

bool IsBlake3Hex(const QString &line) {
	if (line.size() != kBlake3HexLen) {
		return false;
	}
	static const auto hexPattern = QRegularExpression(
		u"^[0-9a-f]{64}$"_q);
	return hexPattern.match(line).hasMatch();
}

void HideItem(not_null<HistoryItem*> item) {
	item->setHiddenByFilter(true);
	item->history()->owner().requestItemResize(item);
}

} // namespace

HashFilter &Instance() {
	static auto instance = HashFilter();
	return instance;
}

bool HashFilter::empty() const {
	return _blake3Hashes.empty()
		&& _texts.empty()
		&& _photoIds.empty()
		&& _docIds.empty();
}

void HashFilter::loadFromDefaultLocation() {
	loadFromFile(DefaultFilterPath());
}

void HashFilter::loadFromFile(const QString &path) {
	_photoIds.clear();
	_docIds.clear();
	_blake3Hashes.clear();
	_texts.clear();
	_pendingChecks.clear();

	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		DebugLog(u"[LOAD] Filter file not found: "_q + path);
		return;
	}

	auto stream = QTextStream(&file);
	while (!stream.atEnd()) {
		const auto raw = stream.readLine().trimmed();
		if (raw.isEmpty() || raw.startsWith('#')) {
			continue;
		}
		if (raw.startsWith(u"photoid:"_q)) {
			auto ok = false;
			const auto id = raw.mid(8).toULongLong(&ok);
			if (ok && id) {
				_photoIds.emplace(id);
			}
		} else if (raw.startsWith(u"docid:"_q)) {
			auto ok = false;
			const auto id = raw.mid(6).toULongLong(&ok);
			if (ok && id) {
				_docIds.emplace(id);
			}
		} else if (raw.startsWith(u"blake3:"_q)) {
			_blake3Hashes.emplace(raw.mid(7).toLower().toLatin1());
		} else if (raw.startsWith(u"text:"_q)) {
			_texts.emplace(UnescapeText(raw.mid(5)));
		} else if (IsBlake3Hex(raw.toLower())) {
			_blake3Hashes.emplace(raw.toLower().toLatin1());
		} else {
			_texts.emplace(raw);
		}
	}
	DebugLog(u"[LOAD] Loaded %1 photoIds + %2 docIds + %3 hashes + %4 texts from %5"_q
		.arg(_photoIds.size())
		.arg(_docIds.size())
		.arg(_blake3Hashes.size())
		.arg(_texts.size())
		.arg(path));
}

bool HashFilter::shouldHideByText(const QString &text) const {
	return !text.isEmpty() && _texts.contains(text);
}

bool HashFilter::shouldHideByPhotoId(uint64 photoId) const {
	return photoId && _photoIds.contains(photoId);
}

bool HashFilter::shouldHideByDocId(uint64 docId) const {
	return docId && _docIds.contains(docId);
}

bool HashFilter::shouldHideByHash(const QByteArray &blake3hex) const {
	return !blake3hex.isEmpty() && _blake3Hashes.contains(blake3hex);
}

bool HashFilter::shouldHideByBytes(const QByteArray &bytes) const {
	if (bytes.isEmpty()) {
		return false;
	}
	return shouldHideByHash(ComputeBlake3(bytes));
}

void HashFilter::appendEntry(const QString &line) {
	auto file = QFile(DefaultFilterPath());
	file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
	auto stream = QTextStream(&file);
	stream << line << u"\n"_q;
}

void HashFilter::appendToFilter(not_null<HistoryItem*> item) {
	const auto &text = item->originalText().text;
	const auto media = item->media();
	const auto photo = media ? media->photo() : nullptr;
	const auto document = media ? media->document() : nullptr;
	auto appended = false;

	if (!text.isEmpty()) {
		const auto escaped = EscapeText(text);
		appendEntry(u"text:"_q + escaped);
		_texts.emplace(text);
		DebugLog(u"[APPEND] msgId=%1 type=text value=\"%2\""_q
			.arg(item->fullId().msg.bare)
			.arg(text.left(60).replace('\n', ' ')));
		appended = true;
	}

	if (photo && photo->id) {
		appendEntry(u"photoid:"_q + QString::number(photo->id));
		_photoIds.emplace(photo->id);
		DebugLog(u"[APPEND] msgId=%1 type=photo photoid=%2"_q
			.arg(item->fullId().msg.bare)
			.arg(photo->id));
		appended = true;
	}

	if (document && document->id) {
		appendEntry(u"docid:"_q + QString::number(document->id));
		_docIds.emplace(document->id);
		DebugLog(u"[APPEND] msgId=%1 type=doc docid=%2"_q
			.arg(item->fullId().msg.bare)
			.arg(document->id));
		appended = true;
	}

	if (appended) {
		HideItem(item);
	}
}

void HashFilter::applyToItem(not_null<HistoryItem*> item) {
	auto &filter = Instance();
	if (filter.empty()) {
		return;
	}

	const auto msgId = item->fullId();
	const auto &text = item->originalText().text;
	const auto media = item->media();
	const auto photo = media ? media->photo() : nullptr;
	const auto document = media ? media->document() : nullptr;

	DebugLog(u"[CHECK] msgId=%1 peer=%2 photoId=%3 docId=%4 text=\"%5\""_q
		.arg(msgId.msg.bare)
		.arg(msgId.peer.value)
		.arg(photo ? photo->id : 0)
		.arg(document ? document->id : 0)
		.arg(text.left(40).replace('\n', ' ')));

	if (filter.shouldHideByText(text)) {
		DebugLog(u"[HIDE] msgId=%1 reason=text_match"_q
			.arg(msgId.msg.bare));
		HideItem(item);
		return;
	}

	if (photo && filter.shouldHideByPhotoId(photo->id)) {
		DebugLog(u"[HIDE] msgId=%1 reason=photoid_match id=%2"_q
			.arg(msgId.msg.bare)
			.arg(photo->id));
		HideItem(item);
		return;
	}

	if (document && filter.shouldHideByDocId(document->id)) {
		DebugLog(u"[HIDE] msgId=%1 reason=docid_match id=%2"_q
			.arg(msgId.msg.bare)
			.arg(document->id));
		HideItem(item);
		return;
	}

	if (!filter._blake3Hashes.empty()) {
		if (photo) {
			filter.schedulePhotoCheck(item, photo);
		}
		if (document && document->isVideoFile()) {
			if (const auto cover = media->videoCover()) {
				filter.schedulePhotoCheck(item, cover);
			}
		}
	}
}

void HashFilter::schedulePhotoCheck(
		not_null<HistoryItem*> item,
		not_null<PhotoData*> photo) {
	const auto origin = Data::FileOrigin(item->fullId());
	auto mediaView = photo->createMediaView();

	photo->load(Data::PhotoSize::Large, origin);

	const auto bytes = mediaView->imageBytes(Data::PhotoSize::Large);
	if (!bytes.isEmpty()) {
		const auto hash = ComputeBlake3(bytes);
		const auto match = shouldHideByHash(hash);
		DebugLog(u"[PHOTO-IMMEDIATE] msgId=%1 photoId=%2 size=%3 blake3=%4 match=%5"_q
			.arg(item->fullId().msg.bare)
			.arg(photo->id)
			.arg(bytes.size())
			.arg(QString::fromLatin1(hash))
			.arg(match));
		if (match) {
			HideItem(item);
		}
		return;
	}

	DebugLog(u"[PHOTO-PENDING] msgId=%1 photoId=%2 (queued)"_q
		.arg(item->fullId().msg.bare)
		.arg(photo->id));

	_pendingChecks.push_back({
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
		processPendingPhotoChecks();
	}, session->lifetime());
}

void HashFilter::processPendingPhotoChecks() {
	if (_pendingChecks.empty()) {
		return;
	}

	auto still = std::vector<PendingCheck>();
	for (auto &pending : _pendingChecks) {
		const auto bytes = pending.mediaView->imageBytes(
			Data::PhotoSize::Large);
		if (bytes.isEmpty()) {
			still.push_back(std::move(pending));
			continue;
		}
		const auto hash = ComputeBlake3(bytes);
		const auto match = shouldHideByHash(hash);
		const auto owner = &pending.mediaView->owner()->owner();

		DebugLog(u"[PHOTO-RESOLVED] msgId=%1 photoId=%2 size=%3 blake3=%4 match=%5"_q
			.arg(pending.itemId.msg.bare)
			.arg(pending.mediaView->owner()->id)
			.arg(bytes.size())
			.arg(QString::fromLatin1(hash))
			.arg(match));

		if (const auto item = owner->message(pending.itemId)) {
			if (match) {
				HideItem(item);
			}
		}
	}
	_pendingChecks = std::move(still);
}

} // namespace MsgFilter
