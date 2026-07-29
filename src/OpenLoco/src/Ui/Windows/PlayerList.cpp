#include "Graphics/Colour.h"
#include "Graphics/ImageIds.h"
#include "Graphics/TextRenderer.h"
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

#include <fmt/format.h>
#include <string>

// Read-only roster window, modeled closely on Chat.cpp (same facade /
// registration / CMakeLists pattern). Lists every connected player (plus the
// host) and, for each, either their company or "spectator". Backed entirely
// by Network::getPlayerRoster() - presentation only, never touches
// GameState or game commands.
namespace OpenLoco::Ui::Windows::PlayerList
{
    static constexpr Ui::Size kWindowSize = { 280, 200 };
    static constexpr uint8_t kLineHeight = 11;

    enum widx
    {
        frame,
        caption,
        closeBtn,
        panel,
    };

    namespace Widx
    {
        constexpr WidgetId kCloseBtn{ "closeBtn" };
    }

    static constexpr auto widgets = makeWidgets(
        Widgets::Frame({ 0, 0 }, { 280, 200 }, WindowColour::primary),
        Widgets::Caption({ 1, 1 }, { 278, 13 }, Widgets::Caption::Style::whiteText, WindowColour::primary, StringIds::empty),
        Widgets::ImageButton(Widx::kCloseBtn, { 265, 2 }, { 13, 13 }, WindowColour::primary, ImageIds::close_button, StringIds::tooltip_close_window),
        Widgets::Panel({ 0, 15 }, { 280, 185 }, WindowColour::secondary)

    );

    static const WindowEventList& getEvents();

    Window* open()
    {
        auto window = WindowManager::find(WindowType::playerList);
        if (window != nullptr)
        {
            return WindowManager::bringToFront(*window);
        }

        window = WindowManager::createWindowCentred(
            WindowType::playerList,
            kWindowSize,
            WindowFlags::none,
            getEvents());

        window->setWidgets(widgets);
        window->initScrollWidgets();
        window->setColour(WindowColour::primary, Colour::black);
        window->setColour(WindowColour::secondary, Colour::black);

        return window;
    }

    static void onMouseUp(Window& self, [[maybe_unused]] WidgetIndex_t widgetIndex, const WidgetId id)
    {
        switch (id)
        {
            case Widx::kCloseBtn:
                WindowManager::close(&self);
                break;
        }
    }

    static void draw(Window& self, Gfx::DrawingContext& drawingCtx)
    {
        auto tr = Gfx::TextRenderer(drawingCtx);

        self.draw(drawingCtx);

        auto point = Point(self.x + 8, self.y + 20);
        for (const auto& entry : Network::getPlayerRoster())
        {
            std::string line;
            if (entry.reserved)
            {
                // Timed-out client with its seat/company held for reconnect
                // (docs/multiplayer.md § Reconnect) - not currently connected.
                line = fmt::format("{} (disconnected)", entry.name);
            }
            else if (entry.company == CompanyId::null)
            {
                line = fmt::format("{} - spectator", entry.name);
            }
            else
            {
                line = fmt::format("{} - company {}", entry.name, static_cast<uint32_t>(entry.company));
            }
            tr.drawString(point, Colour::black, line.c_str());
            point.y += kLineHeight;
        }
    }

    static constexpr WindowEventList kEvents = {
        .onMouseUp = onMouseUp,
        .draw = draw,
    };

    static const WindowEventList& getEvents()
    {
        return kEvents;
    }
}
