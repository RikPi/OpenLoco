#include "Graphics/Colour.h"
#include "Graphics/ImageIds.h"
#include "Graphics/TextRenderer.h"
#include "Localisation/FormatArguments.hpp"
#include "Localisation/StringIds.h"
#include "Network/Network.h"
#include "Ui/Widget.h"
#include "Ui/Widgets/ButtonWidget.h"
#include "Ui/Widgets/CaptionWidget.h"
#include "Ui/Widgets/FrameWidget.h"
#include "Ui/Widgets/ImageButtonWidget.h"
#include "Ui/Widgets/PanelWidget.h"
#include "Ui/Window.h"
#include "Ui/WindowManager.h"

#include <cstring>
#include <deque>
#include <fmt/format.h>
#include <string>
#include <string_view>

namespace OpenLoco::Ui::Windows::Chat
{
    static constexpr Ui::Size kWindowSize = { 320, 182 };
    static constexpr uint8_t kLineHeight = 11;
    // History storage cap. Kept well above the number of lines the window
    // can actually show at once (kVisibleMessages) so scrolling back through
    // recent chat history isn't lost the moment a handful of new messages
    // arrive - draw() only ever renders the most recent kVisibleMessages
    // entries (see draw() below for why this is a "render the tail" approach
    // rather than a real ScrollView: the window has no scrollbar/viewport
    // widget today, and adding one is a bigger change than this pass's
    // scope - noted here as a follow-up rather than done).
    static constexpr size_t kMaxMessages = 100;
    // Fixed number of lines actually drawn - matches the panel's available
    // height, same as the old kMaxMessages value.
    static constexpr size_t kVisibleMessages = 12;

    enum widx
    {
        frame,
        caption,
        closeBtn,
        panel,
        playersBtn,
        sendBtn,
    };

    namespace Widx
    {
        constexpr WidgetId kCloseBtn{ "closeBtn" };
        constexpr WidgetId kPlayersBtn{ "playersBtn" };
        constexpr WidgetId kSendBtn{ "sendBtn" };
    }

    static constexpr auto widgets = makeWidgets(
        Widgets::Frame({ 0, 0 }, { 320, 182 }, WindowColour::primary),
        Widgets::Caption({ 1, 1 }, { 318, 13 }, Widgets::Caption::Style::whiteText, WindowColour::primary, StringIds::chat_title),
        Widgets::ImageButton(Widx::kCloseBtn, { 305, 2 }, { 13, 13 }, WindowColour::primary, ImageIds::close_button, StringIds::tooltip_close_window),
        Widgets::Panel({ 0, 15 }, { 320, 167 }, WindowColour::secondary),
        Widgets::Button(Widx::kPlayersBtn, { 8, 153 }, { 70, 14 }, WindowColour::secondary, StringIds::chat_players_button),
        Widgets::Button(Widx::kSendBtn, { 82, 153 }, { 230, 14 }, WindowColour::secondary, StringIds::chat_send_message)

    );

    // Chat history, oldest message first. Capped to kMaxMessages entries;
    // draw() only shows the most recent kVisibleMessages of these (see
    // kMaxMessages comment above).
    static std::deque<std::string> _history;

    static const WindowEventList& getEvents();

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

        return window;
    }

    // Called by Network::receiveChatMessage whenever a chat message (including
    // our own, echoed back by the server) arrives. Opens the window on the
    // first message if it isn't already open. senderName is already resolved
    // (roster lookup, or a "Player #N" fallback) by the caller.
    void addMessage(std::string_view senderName, std::string_view message)
    {
        _history.push_back(fmt::format("{}: {}", senderName, message));
        while (_history.size() > kMaxMessages)
        {
            _history.pop_front();
        }

        auto window = WindowManager::find(WindowType::chat);
        if (window == nullptr)
        {
            open();
        }
        else
        {
            WindowManager::invalidate(WindowType::chat);
        }
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

    static void draw(Window& self, Gfx::DrawingContext& drawingCtx)
    {
        auto tr = Gfx::TextRenderer(drawingCtx);

        self.draw(drawingCtx);

        // Only the most recent kVisibleMessages entries fit in the fixed-size
        // panel; with kMaxMessages raised well above that, older history is
        // still retained in _history (e.g. for a future real scrollback) but
        // simply isn't drawn - the visible window always shows the tail end
        // of the conversation, i.e. the newest messages, oldest-of-the-shown
        // batch at the top and the newest at the bottom.
        auto firstVisible = _history.size() > kVisibleMessages ? _history.size() - kVisibleMessages : 0;

        auto point = Point(self.x + 4, self.y + 20);
        for (auto i = firstVisible; i < _history.size(); i++)
        {
            tr.drawString(point, Colour::black, _history[i].c_str());
            point.y += kLineHeight;
        }
    }

    static constexpr WindowEventList kEvents = {
        .onMouseUp = onMouseUp,
        .textInput = textInput,
        .draw = draw,
    };

    static const WindowEventList& getEvents()
    {
        return kEvents;
    }
}
