/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"

namespace Window {
class SessionController;
} // namespace Window

namespace MsgFilter {

// Walks the active session's main chat list, finds every 1-on-1 chat with
// a deleted account (UserDataFlag::Deleted, exposed as
// UserData::isInaccessible()), shows a confirmation, and on confirm
// blocks each user and deletes the conversation locally (revoke=false).
void ClearDeletedAccountChats(
	not_null<Window::SessionController*> controller);

// Walks the active session's main chat list and mutes (forever) every
// broadcast channel that isn't already muted. Shows a confirmation box
// first so accidental right-clicks don't nuke notifications silently.
void MuteAllChannels(not_null<Window::SessionController*> controller);

// Same as MuteAllChannels but for chats: basic groups and megagroups.
void MuteAllChats(not_null<Window::SessionController*> controller);

} // namespace MsgFilter
