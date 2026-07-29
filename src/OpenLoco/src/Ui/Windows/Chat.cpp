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
    static constexpr size_t kMaxMessages = 12;

    enum widx
    {
        frame,
        caption,
        closeBtn,
        panel,
        sendBtn,
    };

    namespace Widx
    {
        constexpr WidgetId kCloseBtn{ "closeBtn" };
        constexpr WidgetId kSendBtn{ "sendBtn" };
    }

    static constexpr auto widgets = makeWidgets(
        Widgets::Frame({ 0, 0 }, { 320, 182 }, WindowColour::primary),
        Widgets::Caption({ 1, 1 }, { 318, 13 }, Widgets::Caption::Style::whiteText, WindowColour::primary, StringIds::chat_title),
        Widgets::ImageButton(Widx::kCloseBtn, { 305, 2 }, { 13, 13 }, WindowColour::primary, ImageIds::close_button, StringIds::tooltip_close_window),
        Widgets::Panel({ 0, 15 }, { 320, 167 }, WindowColour::secondary),
        Widgets::Button(Widx::kSendBtn, { 8, 153 }, { 304, 14 }, WindowColour::secondary, StringIds::chat_send_message)

    );

    // Chat history, oldest message first. Capped to kMaxMessages entries.
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

        auto point = Point(self.x + 4, self.y + 20);
        for (const auto& line : _history)
        {
            tr.drawString(point, Colour::black, line.c_str());
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
