/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/stories/media_stories_controller.h"

#include "base/timer.h"
#include "base/power_save_blocker.h"
#include "base/qt_signal_producer.h"
#include "base/unixtime.h"
#include "boxes/peers/prepare_short_info_box.h"
#include "chat_helpers/compose/compose_show.h"
#include "core/application.h"
#include "core/update_checker.h"
#include "data/stickers/data_custom_emoji.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_message_reactions.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_user.h"
#include "history/view/reactions/history_view_reactions_strip.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/stories/media_stories_caption_full_view.h"
#include "media/stories/media_stories_delegate.h"
#include "media/stories/media_stories_header.h"
#include "media/stories/media_stories_sibling.h"
#include "media/stories/media_stories_slider.h"
#include "media/stories/media_stories_reactions.h"
#include "media/stories/media_stories_recent_views.h"
#include "media/stories/media_stories_reply.h"
#include "media/stories/media_stories_share.h"
#include "media/stories/media_stories_view.h"
#include "media/audio/media_audio.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/report_box.h"
#include "ui/effects/emoji_fly_animation.h"
#include "ui/effects/message_sending_animation_common.h"
#include "ui/effects/reaction_fly_animation.h"
#include "ui/layers/box_content.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/round_rect.h"
#include "ui/rp_widget.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_media_view.h"
#include "styles/style_widgets.h"
#include "styles/style_boxes.h" // UserpicButton

#include <QtGui/QWindow>

namespace Media::Stories {
namespace {

constexpr auto kPhotoProgressInterval = crl::time(100);
constexpr auto kPhotoDuration = 5 * crl::time(1000);
constexpr auto kFullContentFade = 0.35;
constexpr auto kSiblingMultiplierDefault = 0.448;
constexpr auto kSiblingMultiplierMax = 0.72;
constexpr auto kSiblingOutsidePart = 0.24;
constexpr auto kSiblingUserpicSize = 0.3;
constexpr auto kInnerHeightMultiplier = 1.6;
constexpr auto kPreloadUsersCount = 3;
constexpr auto kPreloadStoriesCount = 5;
constexpr auto kPreloadNextMediaCount = 3;
constexpr auto kPreloadPreviousMediaCount = 1;
constexpr auto kMarkAsReadAfterSeconds = 0.2;
constexpr auto kMarkAsReadAfterProgress = 0.;

struct SameDayRange {
	int from = 0;
	int till = 0;
};
[[nodiscard]] SameDayRange ComputeSameDayRange(
		not_null<Data::Story*> story,
		const Data::StoriesIds &ids,
		int index) {
	Expects(index >= 0 && index < ids.list.size());

	auto result = SameDayRange{ .from = index, .till = index };
	const auto peerId = story->peer()->id;
	const auto stories = &story->owner().stories();
	const auto now = base::unixtime::parse(story->date());
	const auto b = begin(ids.list);
	for (auto i = b + index; i != b;) {
		if (const auto maybeStory = stories->lookup({ peerId, *--i })) {
			const auto day = base::unixtime::parse((*maybeStory)->date());
			if (day.date() != now.date()) {
				break;
			}
		}
		--result.from;
	}
	for (auto i = b + index + 1, e = end(ids.list); i != e; ++i) {
		if (const auto maybeStory = stories->lookup({ peerId, *i })) {
			const auto day = base::unixtime::parse((*maybeStory)->date());
			if (day.date() != now.date()) {
				break;
			}
		}
		++result.till;
	}
	return result;
}

} // namespace

class Controller::PhotoPlayback final {
public:
	explicit PhotoPlayback(not_null<Controller*> controller);

	[[nodiscard]] bool paused() const;
	void togglePaused(bool paused);

private:
	void callback();

	const not_null<Controller*> _controller;

	base::Timer _timer;
	crl::time _started = 0;
	crl::time _paused = 0;

};

class Controller::Unsupported final {
public:
	Unsupported(not_null<Controller*> controller, not_null<UserData*> user);

private:
	void setup(not_null<UserData*> user);

	const not_null<Controller*> _controller;
	std::unique_ptr<Ui::RpWidget> _bg;
	std::unique_ptr<Ui::FlatLabel> _text;
	std::unique_ptr<Ui::RoundButton> _button;
	Ui::RoundRect _bgRound;

};

Controller::PhotoPlayback::PhotoPlayback(not_null<Controller*> controller)
: _controller(controller)
, _timer([=] { callback(); })
, _started(crl::now())
, _paused(_started) {
}

bool Controller::PhotoPlayback::paused() const {
	return _paused != 0;
}

void Controller::PhotoPlayback::togglePaused(bool paused) {
	if (!_paused == !paused) {
		return;
	} else if (paused) {
		const auto now = crl::now();
		if (now - _started >= kPhotoDuration) {
			return;
		}
		_paused = now;
		_timer.cancel();
	} else {
		_started += crl::now() - _paused;
		_paused = 0;
		_timer.callEach(kPhotoProgressInterval);
	}
	callback();
}

void Controller::PhotoPlayback::callback() {
	const auto now = crl::now();
	const auto elapsed = now - _started;
	const auto finished = (now - _started >= kPhotoDuration);
	if (finished) {
		_timer.cancel();
	}
	using State = Player::State;
	const auto state = finished
		? State::StoppedAtEnd
		: _paused
		? State::Paused
		: State::Playing;
	_controller->updatePhotoPlayback({
		.state = state,
		.position = elapsed,
		.receivedTill = kPhotoDuration,
		.length = kPhotoDuration,
		.frequency = 1000,
	});
}

Controller::Unsupported::Unsupported(
	not_null<Controller*> controller,
	not_null<UserData*> user)
: _controller(controller)
, _bgRound(st::storiesRadius, st::storiesComposeBg) {
	setup(user);
}

void Controller::Unsupported::setup(not_null<UserData*> user) {
	const auto wrap = _controller->wrap();

	_bg = std::make_unique<Ui::RpWidget>(wrap);
	_bg->show();
	_bg->paintRequest() | rpl::start_with_next([=] {
		auto p = QPainter(_bg.get());
		_bgRound.paint(p, _bg->rect());
	}, _bg->lifetime());

	_controller->layoutValue(
	) | rpl::start_with_next([=](const Layout &layout) {
		_bg->setGeometry(layout.content);
	}, _bg->lifetime());

	_text = std::make_unique<Ui::FlatLabel>(
		wrap,
		tr::lng_stories_unsupported(),
		st::storiesUnsupportedLabel);
	_text->show();

	_button = std::make_unique<Ui::RoundButton>(
		wrap,
		tr::lng_update_telegram(),
		st::storiesUnsupportedUpdate);
	_button->setTextTransform(Ui::RoundButton::TextTransform::NoTransform);
	_button->show();

	rpl::combine(
		_controller->layoutValue(),
		_text->sizeValue(),
		_button->sizeValue()
	) | rpl::start_with_next([=](
			const Layout &layout,
			QSize text,
			QSize button) {
		const auto wrap = layout.content;
		const auto totalHeight = st::storiesUnsupportedTop
			+ text.height()
			+ st::storiesUnsupportedSkip
			+ button.height();
		const auto top = (wrap.height() - totalHeight) / 2;
		_text->move(
			wrap.x() + (wrap.width() - text.width()) / 2,
			wrap.y() + top + st::storiesUnsupportedTop);
		_button->move(
			wrap.x() + (wrap.width() - button.width()) / 2,
			wrap.y() + top + totalHeight - button.height());
	}, _button->lifetime());

	_button->setClickedCallback([=] {
		Core::UpdateApplication();
	});
}

Controller::Controller(not_null<Delegate*> delegate)
: _delegate(delegate)
, _wrap(_delegate->storiesWrap())
, _header(std::make_unique<Header>(this))
, _slider(std::make_unique<Slider>(this))
, _replyArea(std::make_unique<ReplyArea>(this))
, _reactions(std::make_unique<Reactions>(this))
, _recentViews(std::make_unique<RecentViews>(this)) {
	initLayout();

	using namespace rpl::mappers;

	rpl::combine(
		_replyArea->activeValue(),
		_reactions->expandedValue(),
		_1 || _2
	) | rpl::distinct_until_changed(
	) | rpl::start_with_next([=](bool active) {
		if (active) {
			_captionFullView = nullptr;
		}
		_replyActive = active;
		updateContentFaded();
	}, _lifetime);

	_replyArea->focusedValue(
	) | rpl::start_with_next([=](bool focused) {
		_replyFocused = focused;
		if (!_replyFocused) {
			_reactions->hideIfCollapsed();
		} else if (!_hasSendText) {
			_reactions->show();
		}
	}, _lifetime);

	_replyArea->hasSendTextValue(
	) | rpl::start_with_next([=](bool has) {
		_hasSendText = has;
		if (_replyFocused) {
			if (_hasSendText) {
				_reactions->hide();
			} else {
				_reactions->show();
			}
		}
	}, _lifetime);

	_reactions->chosen(
	) | rpl::start_with_next([=](HistoryView::Reactions::ChosenReaction id) {
		startReactionAnimation(id.id, {
			.type = Ui::MessageSendingAnimationFrom::Type::Emoji,
			.globalStartGeometry = id.globalGeometry,
			.frame = id.icon,
		});
		_replyArea->sendReaction(id.id);
		unfocusReply();
	}, _lifetime);

	_delegate->storiesLayerShown(
	) | rpl::start_with_next([=](bool shown) {
		_layerShown = shown;
		updatePlayingAllowed();
	}, _lifetime);

	const auto window = _wrap->window()->windowHandle();
	Assert(window != nullptr);
	base::qt_signal_producer(
		window,
		&QWindow::activeChanged
	) | rpl::start_with_next([=] {
		_windowActive = window->isActive();
		updatePlayingAllowed();
	}, _lifetime);
	_windowActive = window->isActive();

	_contentFadeAnimation.stop();
}

Controller::~Controller() {
	changeShown(nullptr);
}

void Controller::updateContentFaded() {
	if (_contentFaded == _replyActive) {
		return;
	}
	_contentFaded = _replyActive;
	_contentFadeAnimation.start(
		[=] { _delegate->storiesRepaint(); },
		_contentFaded ? 0. : 1.,
		_contentFaded ? 1. : 0.,
		st::fadeWrapDuration);
	updatePlayingAllowed();
}

void Controller::initLayout() {
	const auto headerHeight = st::storiesHeaderMargin.top()
		+ st::storiesHeaderPhoto.photoSize
		+ st::storiesHeaderMargin.bottom();
	const auto sliderHeight = st::storiesSliderMargin.top()
		+ st::storiesSliderWidth
		+ st::storiesSliderMargin.bottom();
	const auto outsideHeaderHeight = headerHeight
		+ sliderHeight
		+ st::storiesSliderOutsideSkip;
	const auto fieldMinHeight = st::storiesFieldMargin.top()
		+ st::storiesAttach.height
		+ st::storiesFieldMargin.bottom();
	const auto minHeightForOutsideHeader = st::storiesFieldMargin.bottom()
		+ outsideHeaderHeight
		+ st::storiesMaxSize.height()
		+ fieldMinHeight;

	_layout = _wrap->sizeValue(
	) | rpl::map([=](QSize size) {
		size = QSize(
			std::max(size.width(), st::mediaviewMinWidth),
			std::max(size.height(), st::mediaviewMinHeight));

		auto layout = Layout();
		layout.headerLayout = (size.height() >= minHeightForOutsideHeader)
			? HeaderLayout::Outside
			: HeaderLayout::Normal;

		const auto topSkip = st::storiesFieldMargin.bottom()
			+ (layout.headerLayout == HeaderLayout::Outside
				? outsideHeaderHeight
				: 0);
		const auto bottomSkip = fieldMinHeight;
		const auto maxWidth = size.width() - 2 * st::storiesSideSkip;
		const auto availableHeight = size.height() - topSkip - bottomSkip;
		const auto maxContentHeight = std::min(
			availableHeight,
			st::storiesMaxSize.height());
		const auto nowWidth = maxContentHeight * st::storiesMaxSize.width()
			/ st::storiesMaxSize.height();
		const auto contentWidth = std::min(nowWidth, maxWidth);
		const auto contentHeight = (contentWidth < nowWidth)
			? (contentWidth * st::storiesMaxSize.height()
				/ st::storiesMaxSize.width())
			: maxContentHeight;
		const auto addedTopSkip = (availableHeight - contentHeight) / 2;
		layout.content = QRect(
			(size.width() - contentWidth) / 2,
			addedTopSkip + topSkip,
			contentWidth,
			contentHeight);

		const auto reactionsWidth = st::storiesReactionsWidth;
		layout.reactions = QRect(
			(size.width() - reactionsWidth) / 2,
			layout.content.y(),
			reactionsWidth,
			contentHeight);

		if (layout.headerLayout == HeaderLayout::Outside) {
			layout.header = QRect(
				layout.content.topLeft() - QPoint(0, outsideHeaderHeight),
				QSize(contentWidth, outsideHeaderHeight));
			layout.slider = QRect(
				layout.header.topLeft() + QPoint(0, headerHeight),
				QSize(contentWidth, sliderHeight));
		} else {
			layout.slider = QRect(
				layout.content.topLeft(),
				QSize(contentWidth, sliderHeight));
			layout.header = QRect(
				layout.slider.topLeft() + QPoint(0, sliderHeight),
				QSize(contentWidth, headerHeight));
		}
		layout.controlsWidth = std::max(
			layout.content.width(),
			st::storiesControlsMinWidth);
		layout.controlsBottomPosition = QPoint(
			(size.width() - layout.controlsWidth) / 2,
			(layout.content.y()
				+ layout.content.height()
				+ fieldMinHeight
				- st::storiesFieldMargin.bottom()));
		layout.views = QRect(
			layout.controlsBottomPosition - QPoint(0, fieldMinHeight),
			QSize(layout.controlsWidth, fieldMinHeight));
		layout.autocompleteRect = QRect(
			layout.controlsBottomPosition.x(),
			0,
			layout.controlsWidth,
			layout.controlsBottomPosition.y());

		const auto sidesAvailable = size.width() - layout.content.width();
		const auto widthForSiblings = sidesAvailable
			- 2 * st::storiesFieldMargin.bottom();
		const auto siblingWidthMax = widthForSiblings
			/ (2 * (1. - kSiblingOutsidePart));
		const auto siblingMultiplierMax = std::max(
			kSiblingMultiplierDefault,
			st::storiesSiblingWidthMin / float64(layout.content.width()));
		const auto siblingMultiplier = std::min({
			siblingMultiplierMax,
			kSiblingMultiplierMax,
			siblingWidthMax / layout.content.width(),
		});
		const auto siblingSize = layout.content.size() * siblingMultiplier;
		const auto siblingTop = (size.height() - siblingSize.height()) / 2;
		const auto outsideMax = int(base::SafeRound(
			siblingSize.width() * kSiblingOutsidePart));
		const auto leftAvailable = layout.content.x() - siblingSize.width();
		const auto xDesired = leftAvailable / 3;
		const auto xPossible = std::min(
			xDesired,
			(leftAvailable - st::storiesControlSize));
		const auto xLeft = std::max(xPossible, -outsideMax);
		const auto xRight = size.width() - siblingSize.width() - xLeft;
		const auto userpicSize = int(base::SafeRound(
			siblingSize.width() * kSiblingUserpicSize));
		const auto innerHeight = userpicSize * kInnerHeightMultiplier;
		const auto userpic = [&](QRect geometry) {
			return QRect(
				(geometry.width() - userpicSize) / 2,
				(geometry.height() - innerHeight) / 2,
				userpicSize,
				userpicSize
			).translated(geometry.topLeft());
		};
		const auto nameFontSize = std::max(
			(st::storiesMaxNameFontSize * contentHeight
				/ st::storiesMaxSize.height()),
			st::fsize);
		const auto nameBoundingRect = [&](QRect geometry, bool left) {
			const auto skipSmall = nameFontSize;
			const auto skipBig = skipSmall - std::min(xLeft, 0);
			return QRect(
				left ? skipBig : skipSmall,
				(geometry.height() - innerHeight) / 2,
				geometry.width() - skipSmall - skipBig,
				innerHeight
			).translated(geometry.topLeft());
		};
		const auto left = QRect({ xLeft, siblingTop }, siblingSize);
		const auto right = QRect({ xRight, siblingTop }, siblingSize);
		layout.siblingLeft = {
			.geometry = left,
			.userpic = userpic(left),
			.nameBoundingRect = nameBoundingRect(left, true),
			.nameFontSize = nameFontSize,
		};
		layout.siblingRight = {
			.geometry = right,
			.userpic = userpic(right),
			.nameBoundingRect = nameBoundingRect(right, false),
			.nameFontSize = nameFontSize,
		};

		return layout;
	});
}

Data::Story *Controller::story() const {
	if (!_session) {
		return nullptr;
	}
	const auto maybeStory = _session->data().stories().lookup(_shown);
	return maybeStory ? maybeStory->get() : nullptr;
}

not_null<Ui::RpWidget*> Controller::wrap() const {
	return _wrap;
}

Layout Controller::layout() const {
	Expects(_layout.current().has_value());

	return *_layout.current();
}

rpl::producer<Layout> Controller::layoutValue() const {
	return _layout.value() | rpl::filter_optional();
}

ContentLayout Controller::contentLayout() const {
	const auto &current = _layout.current();
	Assert(current.has_value());

	return {
		.geometry = current->content,
		.fade = (_contentFadeAnimation.value(_contentFaded ? 1. : 0.)
			* kFullContentFade),
		.radius = st::storiesRadius,
		.headerOutside = (current->headerLayout == HeaderLayout::Outside),
	};
}

bool Controller::closeByClickAt(QPoint position) const {
	const auto &current = _layout.current();
	Assert(current.has_value());

	return (position.x() < current->content.x() - st::storiesControlSize)
		|| (position.x() > current->content.x() + current->content.width()
			+ st::storiesControlSize);
}

Data::FileOrigin Controller::fileOrigin() const {
	return Data::FileOriginStory(_shown.peer, _shown.story);
}

TextWithEntities Controller::captionText() const {
	return _captionText;
}

void Controller::showFullCaption() {
	if (_captionText.empty()) {
		return;
	}
	togglePaused(true);
	_captionFullView = std::make_unique<CaptionFullView>(
		wrap(),
		&_delegate->storiesShow()->session(),
		_captionText,
		[=] { togglePaused(false); });
}

std::shared_ptr<ChatHelpers::Show> Controller::uiShow() const {
	return _delegate->storiesShow();
}

auto Controller::stickerOrEmojiChosen() const
-> rpl::producer<ChatHelpers::FileChosen> {
	return _delegate->storiesStickerOrEmojiChosen();
}

auto Controller::cachedReactionIconFactory() const
-> HistoryView::Reactions::CachedIconFactory & {
	return _delegate->storiesCachedReactionIconFactory();
}

void Controller::rebuildFromContext(
		not_null<UserData*> user,
		FullStoryId storyId) {
	using namespace Data;

	auto &stories = user->owner().stories();
	auto list = std::optional<StoriesList>();
	auto source = (const StoriesSource*)nullptr;
	const auto peerId = storyId.peer;
	const auto id = storyId.story;
	v::match(_context.data, [&](StoriesContextSingle) {
		hideSiblings();
	}, [&](StoriesContextPeer) {
		source = stories.source(peerId);
		hideSiblings();
	}, [&](StoriesContextSaved) {
		if (stories.savedCountKnown(peerId)) {
			if (const auto saved = stories.saved(peerId)) {
				const auto &ids = saved->list;
				const auto i = ids.find(id);
				if (i != end(ids)) {
					list = StoriesList{
						.user = user,
						.ids = *saved,
						.total = stories.savedCount(peerId),
					};
					_index = int(i - begin(ids));
					if (ids.size() < list->total
						&& (end(ids) - i) < kPreloadStoriesCount) {
						stories.savedLoadMore(peerId);
					}
				}
			}
		}
		hideSiblings();
	}, [&](StoriesContextArchive) {
		Expects(user->isSelf());

		if (stories.archiveCountKnown()) {
			const auto &archive = stories.archive();
			const auto &ids = archive.list;
			const auto i = ids.find(id);
			if (i != end(ids)) {
				list = StoriesList{
					.user = user,
					.ids = archive,
					.total = stories.archiveCount(),
				};
				_index = int(i - begin(ids));
				if (ids.size() < list->total
					&& (end(ids) - i) < kPreloadStoriesCount) {
					stories.archiveLoadMore();
				}
			}
		}
		hideSiblings();
	}, [&](StorySourcesList list) {
		source = stories.source(peerId);
		const auto &sources = stories.sources(list);
		const auto i = ranges::find(
			sources,
			storyId.peer,
			&StoriesSourceInfo::id);
		if (i != end(sources)) {
			rebuildCachedSourcesList(sources, (i - begin(sources)));
			showSiblings(&user->session());
			if (int(sources.end() - i) < kPreloadUsersCount) {
				stories.loadMore(list);
			}
		}
	});
	_sliderIndex = 0;
	_sliderCount = 0;
	if (list) {
		_source = std::nullopt;
		if (_list != list) {
			_list = std::move(list);
		}
		if (const auto maybe = user->owner().stories().lookup(storyId)) {
			const auto now = *maybe;
			const auto range = ComputeSameDayRange(now, _list->ids, _index);
			_sliderCount = range.till - range.from + 1;
			_sliderIndex = _index - range.from;
		}
	} else {
		if (source) {
			const auto i = source->ids.lower_bound(StoryIdDates{ id });
			if (i != end(source->ids) && i->id == id) {
				_index = int(i - begin(source->ids));
			} else {
				source = nullptr;
			}
		}
		if (!source) {
			_source = std::nullopt;
			_list = StoriesList{
				.user = user,
				.ids = { { id } },
				.total = 1,
			};
			_index = 0;
		} else {
			_list = std::nullopt;
			if (_source != *source) {
				_source = *source;
			}
		}
	}
	preloadNext();
	_slider->show({
		.index = _sliderCount ? _sliderIndex : _index,
		.total = _sliderCount ? _sliderCount : shownCount(),
	});
}

void Controller::preloadNext() {
	Expects(shown());

	auto ids = std::vector<FullStoryId>();
	ids.reserve(kPreloadPreviousMediaCount + kPreloadNextMediaCount);
	const auto user = shownUser();
	const auto count = shownCount();
	const auto till = std::min(_index + kPreloadNextMediaCount, count);
	for (auto i = _index + 1; i != till; ++i) {
		ids.push_back({ .peer = user->id, .story = shownId(i) });
	}
	const auto from = std::max(_index - kPreloadPreviousMediaCount, 0);
	for (auto i = _index; i != from;) {
		ids.push_back({ .peer = user->id, .story = shownId(--i) });
	}
	user->owner().stories().setPreloadingInViewer(std::move(ids));
}

void Controller::checkMoveByDelta() {
	const auto index = _index + _waitingForDelta;
	if (_waitingForDelta && shown() && index >= 0 && index < shownCount()) {
		subjumpTo(index);
	}
}

void Controller::show(
		not_null<Data::Story*> story,
		Data::StoriesContext context) {
	auto &stories = story->owner().stories();
	const auto storyId = story->fullId();
	const auto user = story->peer()->asUser();
	_context = context;
	_waitingForId = {};
	_waitingForDelta = 0;

	rebuildFromContext(user, storyId);
	_contextLifetime.destroy();
	const auto subscribeToSource = [&] {
		stories.sourceChanged() | rpl::filter(
			rpl::mappers::_1 == storyId.peer
		) | rpl::start_with_next([=] {
			rebuildFromContext(user, storyId);
		}, _contextLifetime);
	};
	v::match(_context.data, [&](Data::StoriesContextSingle) {
	}, [&](Data::StoriesContextPeer) {
		subscribeToSource();
	}, [&](Data::StoriesContextSaved) {
		stories.savedChanged() | rpl::filter(
			rpl::mappers::_1 == storyId.peer
		) | rpl::start_with_next([=] {
			rebuildFromContext(user, storyId);
			checkMoveByDelta();
		}, _contextLifetime);
	}, [&](Data::StoriesContextArchive) {
		stories.archiveChanged(
		) | rpl::start_with_next([=] {
			rebuildFromContext(user, storyId);
			checkMoveByDelta();
		}, _contextLifetime);
	}, [&](Data::StorySourcesList) {
		subscribeToSource();
	});

	const auto guard = gsl::finally([&] {
		_paused = false;
		_started = false;
		if (!story->document()) {
			_photoPlayback = std::make_unique<PhotoPlayback>(this);
		} else {
			_photoPlayback = nullptr;
		}
	});

	const auto unsupported = story->unsupported();
	if (!unsupported) {
		_unsupported = nullptr;
	} else {
		_unsupported = std::make_unique<Unsupported>(this, user);
		_header->raise();
		_slider->raise();
	}

	_captionText = story->caption();
	_captionFullView = nullptr;
	const auto document = story->document();
	_header->show({
		.user = user,
		.date = story->date(),
		.fullIndex = _sliderCount ? _index : 0,
		.fullCount = _sliderCount ? shownCount() : 0,
		.privacy = story->privacy(),
		.edited = story->edited(),
		.video = (document != nullptr),
		.silent = (document && document->isSilentVideo()),
	});
	if (!changeShown(story)) {
		return;
	}
	_viewed = false;
	invalidate_weak_ptrs(&_viewsLoadGuard);
	_reactions->hide();
	if (_replyFocused) {
		unfocusReply();
	}

	_replyArea->show({
		.user = unsupported ? nullptr : user,
		.id = story->id(),
	});
	_recentViews->show({
		.list = story->recentViewers(),
		.total = story->views(),
		.valid = user->isSelf(),
	});

	stories.loadAround(storyId, context);

	updatePlayingAllowed();
	user->updateFull();
}

bool Controller::changeShown(Data::Story *story) {
	const auto id = story ? story->fullId() : FullStoryId();
	const auto session = story ? &story->session() : nullptr;
	const auto sessionChanged = (_session != session);
	if (_shown == id && !sessionChanged) {
		return false;
	}
	if (const auto now = this->story()) {
		now->owner().stories().unregisterPolling(
			now,
			Data::Stories::Polling::Viewer);
	}
	if (sessionChanged) {
		_sessionLifetime.destroy();
	}
	_shown = id;
	_session = session;
	if (sessionChanged) {
		subscribeToSession();
	}
	if (story) {
		story->owner().stories().registerPolling(
			story,
			Data::Stories::Polling::Viewer);
	}
	return true;
}

void Controller::subscribeToSession() {
	Expects(!_sessionLifetime);

	if (!_session) {
		return;
	}
	_session->changes().storyUpdates(
		Data::StoryUpdate::Flag::Destroyed
	) | rpl::start_with_next([=](Data::StoryUpdate update) {
		if (update.story->fullId() == _shown) {
			_delegate->storiesClose();
		}
	}, _sessionLifetime);
	_session->data().stories().itemsChanged(
	) | rpl::start_with_next([=](PeerId peerId) {
		if (_waitingForId.peer == peerId) {
			checkWaitingFor();
		}
	}, _sessionLifetime);
	_session->changes().storyUpdates(
		Data::StoryUpdate::Flag::Edited
		| Data::StoryUpdate::Flag::ViewsAdded
	) | rpl::filter([=](const Data::StoryUpdate &update) {
		return (update.story == this->story());
	}) | rpl::start_with_next([=](const Data::StoryUpdate &update) {
		if (update.flags & Data::StoryUpdate::Flag::Edited) {
			show(update.story, _context);
			_delegate->storiesRedisplay(update.story);
		} else {
			_recentViews->show({
				.list = update.story->recentViewers(),
				.total = update.story->views(),
				.valid = update.story->peer()->isSelf(),
			});
		}
	}, _sessionLifetime);
	_sessionLifetime.add([=] {
		_session->data().stories().setPreloadingInViewer({});
	});
}

PauseState Controller::pauseState() const {
	const auto inactive = !_windowActive
		|| _replyActive
		|| _layerShown
		|| _menuShown;
	const auto playing = !inactive && !_paused;
	return playing
		? PauseState::Playing
		: !inactive
		? PauseState::Paused
		: _paused
		? PauseState::InactivePaused
		: PauseState::Inactive;
}

float64 Controller::currentVolume() const {
	return Core::App().settings().videoVolume();
}

void Controller::toggleVolume() {
	_delegate->storiesVolumeToggle();
}

void Controller::changeVolume(float64 volume) {
	_delegate->storiesVolumeChanged(volume);
}

void Controller::volumeChangeFinished() {
	_delegate->storiesVolumeChangeFinished();
}

void Controller::updatePlayingAllowed() {
	if (!_shown) {
		return;
	}
	_header->updatePauseState();
	setPlayingAllowed(_started
		&& _windowActive
		&& !_paused
		&& !_replyActive
		&& !_layerShown
		&& !_menuShown);
}

void Controller::setPlayingAllowed(bool allowed) {
	if (allowed) {
		_captionFullView = nullptr;
	}
	if (_photoPlayback) {
		_photoPlayback->togglePaused(!allowed);
	} else {
		_delegate->storiesTogglePaused(!allowed);
	}
}

void Controller::showSiblings(not_null<Main::Session*> session) {
	showSibling(
		_siblingLeft,
		session,
		(_cachedSourceIndex > 0
			? _cachedSourcesList[_cachedSourceIndex - 1]
			: PeerId()));
	showSibling(
		_siblingRight,
		session,
		(_cachedSourceIndex + 1 < _cachedSourcesList.size()
			? _cachedSourcesList[_cachedSourceIndex + 1]
			: PeerId()));
}

void Controller::hideSiblings() {
	_siblingLeft = nullptr;
	_siblingRight = nullptr;
}

void Controller::showSibling(
		std::unique_ptr<Sibling> &sibling,
		not_null<Main::Session*> session,
		PeerId peerId) {
	if (!peerId) {
		sibling = nullptr;
		return;
	}
	const auto source = session->data().stories().source(peerId);
	if (!source) {
		sibling = nullptr;
	} else if (!sibling || !sibling->shows(*source)) {
		sibling = std::make_unique<Sibling>(this, *source);
	}
}

void Controller::ready() {
	if (_started) {
		return;
	}
	_started = true;
	updatePlayingAllowed();
}

void Controller::updateVideoPlayback(const Player::TrackState &state) {
	updatePlayback(state);
}

void Controller::updatePhotoPlayback(const Player::TrackState &state) {
	updatePlayback(state);
}

void Controller::updatePlayback(const Player::TrackState &state) {
	_slider->updatePlayback(state);
	updatePowerSaveBlocker(state);
	maybeMarkAsRead(state);
	if (Player::IsStoppedAtEnd(state.state)) {
		if (!subjumpFor(1)) {
			_delegate->storiesClose();
		}
	}
}

void Controller::maybeMarkAsRead(const Player::TrackState &state) {
	const auto length = state.length;
	const auto position = Player::IsStoppedAtEnd(state.state)
		? state.length
		: Player::IsStoppedOrStopping(state.state)
		? 0
		: state.position;
	if (position > state.frequency * kMarkAsReadAfterSeconds) {
		if (position > kMarkAsReadAfterProgress * length) {
			markAsRead();
		}
	}
}

void Controller::markAsRead() {
	Expects(shown());

	if (_viewed) {
		return;
	}
	_viewed = true;
	shownUser()->owner().stories().markAsRead(_shown, _started);
}

bool Controller::subjumpAvailable(int delta) const {
	const auto index = _index + delta;
	if (index < 0) {
		return _siblingLeft && _siblingLeft->shownId().valid();
	} else if (index >= shownCount()) {
		return _siblingRight && _siblingRight->shownId().valid();
	}
	return index >= 0 && index < shownCount();
}

bool Controller::subjumpFor(int delta) {
	if (delta > 0) {
		markAsRead();
	}
	const auto index = _index + delta;
	if (index < 0) {
		if (_siblingLeft && _siblingLeft->shownId().valid()) {
			return jumpFor(-1);
		} else if (!shown() || !shownCount()) {
			return false;
		}
		subjumpTo(0);
		return true;
	} else if (index >= shownCount()) {
		return _siblingRight
			&& _siblingRight->shownId().valid()
			&& jumpFor(1);
	} else {
		subjumpTo(index);
	}
	return true;
}

void Controller::subjumpTo(int index) {
	Expects(shown());
	Expects(index >= 0 && index < shownCount());

	const auto user = shownUser();
	const auto id = FullStoryId{
		.peer = user->id,
		.story = shownId(index),
	};
	auto &stories = user->owner().stories();
	if (!id.story) {
		const auto delta = index - _index;
		if (_waitingForDelta != delta) {
			_waitingForDelta = delta;
			_waitingForId = {};
			loadMoreToList();
		}
	} else if (stories.lookup(id)) {
		_delegate->storiesJumpTo(&user->session(), id, _context);
	} else if (_waitingForId != id) {
		_waitingForId = id;
		_waitingForDelta = 0;
		stories.loadAround(id, _context);
	}
}

void Controller::checkWaitingFor() {
	Expects(_waitingForId.valid());
	Expects(shown());

	const auto user = shownUser();
	auto &stories = user->owner().stories();
	const auto maybe = stories.lookup(_waitingForId);
	if (!maybe) {
		if (maybe.error() == Data::NoStory::Deleted) {
			_waitingForId = {};
		}
		return;
	}
	_delegate->storiesJumpTo(
		&user->session(),
		base::take(_waitingForId),
		_context);
}

bool Controller::jumpFor(int delta) {
	if (delta == -1) {
		if (const auto left = _siblingLeft.get()) {
			_delegate->storiesJumpTo(
				&left->peer()->session(),
				left->shownId(),
				_context);
			return true;
		}
	} else if (delta == 1) {
		if (shown() && _index + 1 >= shownCount()) {
			markAsRead();
		}
		if (const auto right = _siblingRight.get()) {
			_delegate->storiesJumpTo(
				&right->peer()->session(),
				right->shownId(),
				_context);
			return true;
		}
	}
	return false;
}

bool Controller::paused() const {
	return _paused;
}

void Controller::togglePaused(bool paused) {
	if (_paused != paused) {
		_paused = paused;
		updatePlayingAllowed();
	}
}

void Controller::contentPressed(bool pressed) {
	togglePaused(pressed);
	if (pressed) {
		_reactions->collapse();
	}
}

void Controller::setMenuShown(bool shown) {
	if (_menuShown != shown) {
		_menuShown = shown;
		updatePlayingAllowed();
	}
}

void Controller::repaintSibling(not_null<Sibling*> sibling) {
	if (sibling == _siblingLeft.get() || sibling == _siblingRight.get()) {
		_delegate->storiesRepaint();
	}
}

SiblingView Controller::sibling(SiblingType type) const {
	const auto &pointer = (type == SiblingType::Left)
		? _siblingLeft
		: _siblingRight;
	if (const auto value = pointer.get()) {
		const auto over = _delegate->storiesSiblingOver(type);
		const auto layout = (type == SiblingType::Left)
			? _layout.current()->siblingLeft
			: _layout.current()->siblingRight;
		return value->view(layout, over);
	}
	return {};
}

ViewsSlice Controller::views(PeerId offset) {
	invalidate_weak_ptrs(&_viewsLoadGuard);
	if (!offset) {
		refreshViewsFromData();
	} else if (!sliceViewsTo(offset)) {
		return { .left = _viewsSlice.left };
	}
	return _viewsSlice;
}

rpl::producer<> Controller::moreViewsLoaded() const {
	return _moreViewsLoaded.events();
}

Fn<void(std::vector<Data::StoryView>)> Controller::viewsGotMoreCallback() {
	return crl::guard(&_viewsLoadGuard, [=](
			const std::vector<Data::StoryView> &result) {
		if (_viewsSlice.list.empty()) {
			const auto user = shownUser();
			auto &stories = user->owner().stories();
			if (const auto maybeStory = stories.lookup(_shown)) {
				_viewsSlice = {
					.list = result,
					.left = (*maybeStory)->views() - int(result.size()),
				};
			} else {
				_viewsSlice = {};
			}
		} else {
			_viewsSlice.list.insert(
				end(_viewsSlice.list),
				begin(result),
				end(result));
			_viewsSlice.left
				= std::max(_viewsSlice.left - int(result.size()), 0);
		}
		_moreViewsLoaded.fire({});
	});
}

bool Controller::shown() const {
	return _source || _list;
}

UserData *Controller::shownUser() const {
	return _source
		? _source->user.get()
		: _list
		? _list->user.get()
		: nullptr;
}

int Controller::shownCount() const {
	return _source ? int(_source->ids.size()) : _list ? _list->total : 0;
}

StoryId Controller::shownId(int index) const {
	Expects(index >= 0 && index < shownCount());

	return _source
		? (_source->ids.begin() + index)->id
		: (index < int(_list->ids.list.size()))
		? *(_list->ids.list.begin() + index)
		: StoryId();
}

void Controller::loadMoreToList() {
	Expects(shown());

	using namespace Data;

	const auto user = shownUser();
	const auto peerId = _shown.peer;
	auto &stories = user->owner().stories();
	v::match(_context.data, [&](StoriesContextSaved) {
		stories.savedLoadMore(peerId);
	}, [&](StoriesContextArchive) {
		Expects(user->isSelf());

		stories.archiveLoadMore();
	}, [](const auto &) {
	});
}

void Controller::rebuildCachedSourcesList(
		const std::vector<Data::StoriesSourceInfo> &lists,
		int index) {
	Expects(index >= 0 && index < lists.size());

	// Remove removed.
	_cachedSourcesList.erase(ranges::remove_if(_cachedSourcesList, [&](
			PeerId id) {
		return !ranges::contains(lists, id, &Data::StoriesSourceInfo::id);
	}), end(_cachedSourcesList));

	// Find current, full rebuild if can't find.
	const auto i = ranges::find(_cachedSourcesList, lists[index].id);
	if (i == end(_cachedSourcesList)) {
		_cachedSourcesList.clear();
	} else {
		_cachedSourceIndex = int(i - begin(_cachedSourcesList));
	}

	if (_cachedSourcesList.empty()) {
		// Full rebuild.
		_cachedSourcesList = lists
			| ranges::views::transform(&Data::StoriesSourceInfo::id)
			| ranges::to_vector;
		_cachedSourceIndex = index;
	} else if (ranges::equal(
			lists,
			_cachedSourcesList,
			ranges::equal_to(),
			&Data::StoriesSourceInfo::id)) {
		// No rebuild needed.
		_cachedSourceIndex = index;
	} else {
		// All that go before the current push to front.
		for (auto before = index; before > 0;) {
			const auto peerId = lists[--before].id;
			if (!ranges::contains(_cachedSourcesList, peerId)) {
				_cachedSourcesList.insert(
					begin(_cachedSourcesList),
					peerId);
				++_cachedSourceIndex;
			}
		}
		// All that go after the current push to back.
		for (auto after = index + 1, count = int(lists.size()); after != count; ++after) {
			const auto peerId = lists[after].id;
			if (!ranges::contains(_cachedSourcesList, peerId)) {
				_cachedSourcesList.push_back(peerId);
			}
		}
	}

	Ensures(_cachedSourcesList.size() == lists.size());
	Ensures(_cachedSourceIndex >= 0
		&& _cachedSourceIndex < _cachedSourcesList.size());
}

void Controller::refreshViewsFromData() {
	Expects(shown());

	const auto user = shownUser();
	auto &stories = user->owner().stories();
	const auto maybeStory = stories.lookup(_shown);
	if (!maybeStory || !user->isSelf()) {
		_viewsSlice = {};
		return;
	}
	const auto story = *maybeStory;
	const auto &list = story->viewsList();
	const auto total = story->views();
	_viewsSlice.list = list
		| ranges::views::take(Data::Stories::kViewsPerPage)
		| ranges::to_vector;
	_viewsSlice.left = total - int(_viewsSlice.list.size());
	if (_viewsSlice.list.empty() && _viewsSlice.left > 0) {
		const auto done = viewsGotMoreCallback();
		stories.loadViewsSlice(_shown.story, std::nullopt, done);
	}
}

bool Controller::sliceViewsTo(PeerId offset) {
	Expects(shown());

	const auto user = shownUser();
	auto &stories = user->owner().stories();
	const auto maybeStory = stories.lookup(_shown);
	if (!maybeStory || !user->isSelf()) {
		_viewsSlice = {};
		return true;
	}
	const auto story = *maybeStory;
	const auto &list = story->viewsList();
	const auto proj = [&](const Data::StoryView &single) {
		return single.peer->id;
	};
	const auto i = ranges::find(list, _viewsSlice.list.back());
	const auto add = (i != end(list)) ? int(end(list) - i - 1) : 0;
	const auto j = ranges::find(_viewsSlice.list, offset, proj);
	Assert(j != end(_viewsSlice.list));
	if (!add && (j + 1) == end(_viewsSlice.list)) {
		const auto done = viewsGotMoreCallback();
		stories.loadViewsSlice(_shown.story, _viewsSlice.list.back(), done);
		return false;
	}
	_viewsSlice.list.erase(begin(_viewsSlice.list), j + 1);
	_viewsSlice.list.insert(end(_viewsSlice.list), i + 1, end(list));
	_viewsSlice.left -= add;
	return true;
}

void Controller::unfocusReply() {
	_wrap->setFocus();
}

void Controller::shareRequested() {
	const auto show = _delegate->storiesShow();
	if (auto box = PrepareShareBox(show, _shown, true)) {
		show->show(std::move(box));
	}
}

void Controller::deleteRequested() {
	const auto story = this->story();
	if (!story) {
		return;
	}
	const auto id = story->fullId();
	const auto weak = base::make_weak(this);
	const auto owner = &story->owner();
	const auto confirmed = [=](Fn<void()> close) {
		if (const auto strong = weak.get()) {
			if (const auto story = strong->story()) {
				if (story->fullId() == id) {
					moveFromShown();
				}
			}
		}
		owner->stories().deleteList({ id });
		close();
	};
	uiShow()->show(Ui::MakeConfirmBox({
		.text = tr::lng_stories_delete_one_sure(),
		.confirmed = confirmed,
		.confirmText = tr::lng_selected_delete(),
		.labelStyle = &st::storiesBoxLabel,
	}));
}

void Controller::reportRequested() {
	ReportRequested(uiShow(), _shown, &st::storiesReportBox);
}

void Controller::togglePinnedRequested(bool pinned) {
	const auto story = this->story();
	if (!story || !story->peer()->isSelf()) {
		return;
	}
	if (!pinned && v::is<Data::StoriesContextSaved>(_context.data)) {
		moveFromShown();
	}
	story->owner().stories().togglePinnedList({ story->fullId() }, pinned);
	uiShow()->showToast(PrepareTogglePinnedToast(1, pinned));
}

void Controller::moveFromShown() {
	if (!subjumpFor(1)) {
		[[maybe_unused]] const auto jumped = subjumpFor(-1);
	}
}

bool Controller::ignoreWindowMove(QPoint position) const {
	return _replyArea->ignoreWindowMove(position)
		|| _header->ignoreWindowMove(position);
}

void Controller::tryProcessKeyInput(not_null<QKeyEvent*> e) {
	_replyArea->tryProcessKeyInput(e);
}

rpl::lifetime &Controller::lifetime() {
	return _lifetime;
}

void Controller::updatePowerSaveBlocker(const Player::TrackState &state) {
	const auto block = !Player::IsPausedOrPausing(state.state)
		&& !Player::IsStoppedOrStopping(state.state);
	base::UpdatePowerSaveBlocker(
		_powerSaveBlocker,
		block,
		base::PowerSaveBlockType::PreventDisplaySleep,
		[] { return u"Stories playback is active"_q; },
		[=] { return _wrap->window()->windowHandle(); });
}

void Controller::startReactionAnimation(
		Data::ReactionId id,
		Ui::MessageSendingAnimationFrom from) {
	Expects(shown());

	auto args = Ui::ReactionFlyAnimationArgs{
		.id = id,
		.flyIcon = from.frame,
		.flyFrom = _wrap->mapFromGlobal(from.globalStartGeometry),
		.scaleOutDuration = st::fadeWrapDuration * 2,
	};
	_reactionAnimation = std::make_unique<Ui::EmojiFlyAnimation>(
		_wrap,
		&shownUser()->owner().reactions(),
		std::move(args),
		[=] { _reactionAnimation->repaint(); },
		Data::CustomEmojiSizeTag::Isolated);
	const auto layer = _reactionAnimation->layer();
	_wrap->paintRequest() | rpl::start_with_next([=] {
		if (!_reactionAnimation->paintBadgeFrame(_wrap.get())) {
			InvokeQueued(layer, [=] {
				_reactionAnimation = nullptr;
				_wrap->update();
			});
		}
	}, layer->lifetime());
}

Ui::Toast::Config PrepareTogglePinnedToast(int count, bool pinned) {
	return {
		.text = (pinned
			? (count == 1
				? tr::lng_stories_save_done(
					tr::now,
					Ui::Text::Bold)
				: tr::lng_stories_save_done_many(
					tr::now,
					lt_count,
					count,
					Ui::Text::Bold)).append(
						'\n').append(
							tr::lng_stories_save_done_about(tr::now))
			: (count == 1
				? tr::lng_stories_archive_done(
					tr::now,
					Ui::Text::WithEntities)
				: tr::lng_stories_archive_done_many(
					tr::now,
					lt_count,
					count,
					Ui::Text::WithEntities))),
		.st = &st::storiesActionToast,
		.duration = (pinned
			? Data::Stories::kPinnedToastDuration
			: Ui::Toast::kDefaultDuration),
	};
}

void ReportRequested(
		std::shared_ptr<Main::SessionShow> show,
		FullStoryId id,
		const style::ReportBox *stOverride) {
	const auto owner = &show->session().data();
	const auto st = stOverride ? stOverride : &st::defaultReportBox;
	show->show(Box(Ui::ReportReasonBox, *st, Ui::ReportSource::Story, [=](
			Ui::ReportReason reason) {
		const auto done = [=](const QString &text) {
			owner->stories().report(show, id, reason, text);
			show->hideLayer();
		};
		show->showBox(Box(Ui::ReportDetailsBox, *st, done));
	}));
}

object_ptr<Ui::BoxContent> PrepareShortInfoBox(not_null<PeerData*> peer) {
	const auto open = [=] {
		if (const auto window = Core::App().windowFor(peer)) {
			window->invokeForSessionController(
				&peer->session().account(),
				peer,
				[&](not_null<Window::SessionController*> controller) {
					Core::App().hideMediaView();
					controller->showPeerHistory(peer);
				});
		}
	};
	return ::PrepareShortInfoBox(
		peer,
		open,
		[] { return false; },
		&st::storiesShortInfoBox);
}

} // namespace Media::Stories