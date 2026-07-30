#include "Graphics/Colour.h"
#include "Graphics/ImageIds.h"
#include "Graphics/TextRenderer.h"
#include "Localisation/FormatArguments.hpp"
#include "Localisation/StringIds.h"
#include "Network/Network.h"
#include "Ui/ScrollView.h"
#include "Ui/Widget.h"
#include "Ui/Widgets/ButtonWidget.h"
#include "Ui/Widgets/CaptionWidget.h"
#include "Ui/Widgets/FrameWidget.h"
#include "Ui/Widgets/ImageButtonWidget.h"
#include "Ui/Widgets/PanelWidget.h"
#include "Ui/Widgets/ScrollViewWidget.h"
#include "Ui/Window.h"
#include "Ui/WindowManager.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <fmt/format.h>
#include <string>
#include <string_view>

namespace OpenLoco::Ui::Windows::Chat
{
    static constexpr Ui::Size kWindowSize = { 320, 182 };
    static constexpr uint8_t kLineHeight = 11;
    // History storage cap - real scrollback now (see the ScrollView widget
    // below), not just a render-tail window like before.
    static constexpr size_t kMaxMessages = 100;

    enum widx
    {
        frame,
        caption,
        closeBtn,
        panel,
        messageList,
        playersBtn,
        sendBtn,
    };

    namespace Widx
    {
        constexpr WidgetId kCloseBtn{ "closeBtn" };
        constexpr WidgetId kMessageList{ "messageList" };
        constexpr WidgetId kPlayersBtn{ "playersBtn" };
        constexpr WidgetId kSendBtn{ "sendBtn" };
    }

    static constexpr auto widgets = makeWidgets(
        Widgets::Frame({ 0, 0 }, { 320, 182 }, WindowColour::primary),
        Widgets::Caption({ 1, 1 }, { 318, 13 }, Widgets::Caption::Style::whiteText, WindowColour::primary, StringIds::chat_title),
        Widgets::ImageButton(Widx::kCloseBtn, { 305, 2 }, { 13, 13 }, WindowColour::primary, ImageIds::close_button, StringIds::tooltip_close_window),
        Widgets::Panel({ 0, 15 }, { 320, 167 }, WindowColour::secondary),
        Widgets::ScrollView(Widx::kMessageList, { 8, 20 }, { 304, 128 }, WindowColour::secondary, Scrollbars::vertical),
        Widgets::Button(Widx::kPlayersBtn, { 8, 153 }, { 70, 14 }, WindowColour::secondary, StringIds::chat_players_button),
        Widgets::Button(Widx::kSendBtn, { 82, 153 }, { 230, 14 }, WindowColour::secondary, StringIds::chat_send_message)

    );

    // Chat history, oldest message first, capped to kMaxMessages entries.
    // drawScroll() renders the whole deque top-to-bottom; the ScrollView
    // widget/viewport (contentOffsetY) is what clips it to the visible area
    // - see ServerBrowser.cpp/CompanyList.cpp for the same idiom.
    static std::deque<std::string> _history;

    static const WindowEventList& getEvents();

    // Total (unclipped) height of the message list content for the current
    // history.
    static int32_t contentHeight()
    {
        return static_cast<int32_t>(_history.size()) * kLineHeight;
    }

    // The furthest down the view can be scrolled (0 once everything fits).
    static int32_t maxScrollOffset(const Window& window)
    {
        auto viewportHeight = window.widgets[widx::messageList].height();
        return std::max<int32_t>(0, contentHeight() - viewportHeight);
    }

    // Snaps the view to the newest message. Used on (re)open with existing
    // history, and from addMessage() when the view was already pinned to
    // the bottom (see there for why it isn't unconditional).
    static void scrollToBottom(Window& window)
    {
        window.scrollAreas[0].contentOffsetY = maxScrollOffset(window);
        Ui::ScrollView::updateThumbs(window, widx::messageList);
    }

    Window* open()
    {
        auto window = WindowManager::find(WindowType::chat);
        if (window != nullptr)
        {
            return WindowManager::bringToFront(*window);
        }

        window = WindowManager::createWindowCentred(
            WindowType::chat,
            kWindowSize,
            WindowFlags::none,
            getEvents());

        window->setWidgets(widgets);
        window->initScrollWidgets();
        window->setColour(WindowColour::primary, Colour::black);
        window->setColour(WindowColour::secondary, Colour::black);

        // Reopening with existing history (e.g. via the roster button after
        // the window was closed) should show the newest messages first, not
        // the oldest retained ones.
        scrollToBottom(*window);

        return window;
    }

    // Called by Network::receiveChatMessage whenever a chat message (including
    // our own, echoed back by the server) arrives. Opens the window on the
    // first message if it isn't already open. senderName is already resolved
    // (roster lookup, or a "Player #N" fallback) by the caller.
    void addMessage(std::string_view senderName, std::string_view message)
    {
        auto window = WindowManager::find(WindowType::chat);

        // Was the view already scrolled all the way down? If so, keep
        // following new messages; if the player scrolled up to read
        // backlog, leave their position alone rather than yanking them back
        // down to the bottom. Must be captured before _history is mutated.
        bool pinnedToBottom = true;
        if (window != nullptr)
        {
            pinnedToBottom = window->scrollAreas[0].contentOffsetY >= maxScrollOffset(*window);
        }

        _history.push_back(fmt::format("{}: {}", senderName, message));
        while (_history.size() > kMaxMessages)
        {
            _history.pop_front();
        }

        if (window == nullptr)
        {
            // open() already pins a freshly (re)created window to the
            // bottom - nothing further to do.
            open();
            return;
        }

        if (pinnedToBottom)
        {
            scrollToBottom(*window);
        }
        window->invalidate();
    }

    static void onMouseUp(Window& self, [[maybe_unused]] WidgetIndex_t widgetIndex, const WidgetId id)
    {
        switch (id)
        {
            case Widx::kCloseBtn:
                WindowManager::close(&self);
                break;

            case Widx::kPlayersBtn:
                PlayerList::open();
                break;

            case Widx::kSendBtn:
            {
                auto args = FormatArguments::common();
                TextInput::openTextInput(&self, StringIds::chat_title, StringIds::chat_instructions, StringIds::empty, widx::sendBtn, args);
                break;
            }
        }
    }

    static void textInput([[maybe_unused]] Window& self, [[maybe_unused]] WidgetIndex_t widgetIndex, const WidgetId id, const char* str)
    {
        if (id != Widx::kSendBtn || str == nullptr)
        {
            return;
        }

        if (std::strlen(str) > 0)
        {
            Network::sendChatMessage(str);
        }
    }

    static void getScrollSize([[maybe_unused]] Window& self, [[maybe_unused]] uint32_t scrollIndex, [[maybe_unused]] int32_t& scrollWidth, int32_t& scrollHeight)
    {
        scrollHeight = contentHeight();
    }

    static void draw(Window& self, Gfx::DrawingContext& drawingCtx)
    {
        self.draw(drawingCtx);
    }

    static void drawScroll(Window& self, Gfx::DrawingContext& drawingCtx, [[maybe_unused]] const uint32_t scrollIndex)
    {
        auto tr = Gfx::TextRenderer(drawingCtx);

        auto colour = Colours::getShade(self.getColour(WindowColour::secondary).c(), 3);
        drawingCtx.clearSingle(colour);

        auto point = Point(2, 1);
        for (const auto& line : _history)
        {
            tr.drawString(point, Colour::black, line.c_str());
            point.y += kLineHeight;
        }
    }

    static constexpr WindowEventList kEvents = {
        .onMouseUp = onMouseUp,
        .getScrollSize = getScrollSize,
        .textInput = textInput,
        .draw = draw,
        .drawScroll = drawScroll,
    };

    static const WindowEventList& getEvents()
    {
        return kEvents;
    }
}
