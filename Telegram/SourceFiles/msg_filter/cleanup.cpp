/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "msg_filter/cleanup.h"

#include "api/api_blocked_peers.h"
#include "apiwrap.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/notify/data_notify_settings.h"
#include "data/notify/data_peer_notify_settings.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "history/history.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "window/window_session_controller.h"

namespace MsgFilter {
namespace {

// Mutes every peer in `victims` forever via the notify settings API.
// Used by MuteAllChannels / MuteAllChats so the two helpers share the
// "iterate + confirm + apply" pipeline.
void MutePeersForever(
		not_null<Main::Session*> session,
		const std::vector<not_null<PeerData*>> &victims) {
	auto &notify = session->data().notifySettings();
	for (const auto peer : victims) {
		notify.update(peer, Data::MuteValue{ .forever = true });
	}
}

template <typename Predicate>
[[nodiscard]] std::vector<not_null<PeerData*>> CollectPeers(
		not_null<Main::Session*> session,
		Predicate &&predicate) {
	auto result = std::vector<not_null<PeerData*>>();
	const auto list = session->data().chatsList(nullptr);
	if (!list || !list->indexed()) {
		return result;
	}
	for (const auto &row : list->indexed()->all()) {
		const auto history = row->history();
		if (!history) {
			continue;
		}
		const auto peer = history->peer;
		if (!peer || peer->isSelf()) {
			continue;
		}
		if (predicate(peer)) {
			result.push_back(peer);
		}
	}
	return result;
}

void BulkMuteWithConfirm(
		not_null<Window::SessionController*> controller,
		std::vector<not_null<PeerData*>> victims,
		const QString &emptyMsg,
		const QString &confirmTemplate,
		const QString &doneTemplate) {
	if (victims.empty()) {
		controller->showToast(emptyMsg);
		return;
	}
	const auto count = int(victims.size());
	const auto weak = base::make_weak(controller);
	controller->show(Ui::MakeConfirmBox({
		.text = confirmTemplate.arg(count),
		.confirmed = [=, victims = std::move(victims)](Fn<void()> close) {
			close();
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			// Re-resolve the session through the controller so we never
			// touch a freed Main::Session* if the user logged out while
			// the confirm box was open.
			MutePeersForever(&strong->session(), victims);
			strong->showToast(doneTemplate.arg(int(victims.size())));
		},
		.confirmText = u"Mute"_q,
	}));
}

} // namespace

void ClearDeletedAccountChats(
		not_null<Window::SessionController*> controller) {
	auto victims = std::vector<not_null<UserData*>>();
	const auto session = &controller->session();
	const auto list = session->data().chatsList(nullptr);
	if (list && list->indexed()) {
		for (const auto &row : list->indexed()->all()) {
			const auto history = row->history();
			if (!history) {
				continue;
			}
			const auto user = history->peer->asUser();
			if (user && user->isInaccessible()) {
				victims.push_back(user);
			}
		}
	}

	if (victims.empty()) {
		controller->showToast(u"No deleted-account chats found."_q);
		return;
	}

	const auto count = int(victims.size());
	const auto weak = base::make_weak(controller);
	auto text = u"Found %1 deleted-account chat(s).\n"_q
			.arg(count)
		+ u"Block and delete all of them?"_q;
	controller->show(Ui::MakeConfirmBox({
		.text = std::move(text),
		.confirmed = [=](Fn<void()> close) {
			close();
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			auto cleared = 0;
			for (const auto user : victims) {
				if (!user) {
					continue;
				}
				auto &api = user->session().api();
				api.blockedPeers().block(user);
				api.deleteConversation(user, false);
				++cleared;
			}
			strong->showToast(
				u"Cleared %1 deleted-account chat(s)."_q.arg(cleared));
		},
		.confirmText = u"Clear"_q,
	}));
}

void MuteAllChannels(not_null<Window::SessionController*> controller) {
	auto victims = CollectPeers(
		&controller->session(),
		[](not_null<PeerData*> peer) {
			if (!peer->isBroadcast()) {
				return false;
			}
			return !peer->owner().notifySettings().isMuted(peer);
		});
	BulkMuteWithConfirm(
		controller,
		std::move(victims),
		u"No unmuted channels to mute."_q,
		u"Mute notifications for %1 channel(s)?"_q,
		u"Muted %1 channel(s)."_q);
}

void MuteAllChats(not_null<Window::SessionController*> controller) {
	auto victims = CollectPeers(
		&controller->session(),
		[](not_null<PeerData*> peer) {
			if (!peer->isChat() && !peer->isMegagroup()) {
				return false;
			}
			return !peer->owner().notifySettings().isMuted(peer);
		});
	BulkMuteWithConfirm(
		controller,
		std::move(victims),
		u"No unmuted chats to mute."_q,
		u"Mute notifications for %1 chat(s)?"_q,
		u"Muted %1 chat(s)."_q);
}

} // namespace MsgFilter
