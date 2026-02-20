/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;
class PhotoData;

namespace Data {
class PhotoMedia;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace MsgFilter {

class HashFilter final {
public:
	void loadFromDefaultLocation();
	void loadFromFile(const QString &path);

	[[nodiscard]] bool shouldHideByText(const QString &text) const;
	[[nodiscard]] bool shouldHideByHash(const QByteArray &blake3hex) const;
	[[nodiscard]] bool shouldHideByBytes(const QByteArray &bytes) const;

	static void applyToItem(not_null<HistoryItem*> item);

	void appendToFilter(not_null<HistoryItem*> item);

	void schedulePhotoCheck(
		not_null<HistoryItem*> item,
		not_null<PhotoData*> photo);
	void processPendingPhotoChecks();

private:
	struct PendingCheck {
		FullMsgId itemId;
		std::shared_ptr<Data::PhotoMedia> mediaView;
	};

	[[nodiscard]] bool empty() const;
	void appendEntry(const QString &line);
	void watchSession(not_null<Main::Session*> session);

	base::flat_set<QByteArray> _blake3Hashes;
	base::flat_set<QString> _texts;
	std::vector<PendingCheck> _pendingChecks;
	base::flat_set<Main::Session*> _watchedSessions;
};

[[nodiscard]] HashFilter &Instance();

} // namespace MsgFilter
