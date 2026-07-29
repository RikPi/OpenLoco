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
#include "Ui/Widgets/ScrollViewWidget.h"
#include "Ui/Window.h"
#include "Ui/WindowManager.h"

#include <cstring>
#include <fmt/format.h>
#include <string>
#include <vector>

// Phase 1 lobby: LAN server browser (docs/multiplayer.md § LAN server
// discovery). Modeled closely on PlayerList.cpp for the facade/registration/
// CMakeLists pattern; the clickable row list itself follows CompanyList.cpp's
// (trimmed down) ScrollView idiom. Discovery (Network::beginServerDiscovery/
// endServerDiscovery) runs only while this window is open. Never touches
// GameState or the game command stream - purely a UI over presentation data.
namespace OpenLoco::Ui::Windows::ServerBrowser
{
    static constexpr Ui::Size kWindowSize = { 380, 220 };
    static constexpr uint8_t kRowHeight = 12;

    enum widx
    {
        frame,
        caption,
        closeBtn,
        panel,
        serverList,
        joinByAddressBtn,
    };

    namespace Widx
    {
        constexpr WidgetId kCloseBtn{ "closeBtn" };
        constexpr WidgetId kServerList{ "serverList" };
        constexpr WidgetId kJoinByAddressBtn{ "joinByAddressBtn" };
    }

    static constexpr auto widgets = makeWidgets(
        Widgets::Frame({ 0, 0 }, { 380, 220 }, WindowColour::primary),
        Widgets::Caption({ 1, 1 }, { 378, 13 }, Widgets::Caption::Style::whiteText, WindowColour::primary, StringIds::empty),
        Widgets::ImageButton(Widx::kCloseBtn, { 365, 2 }, { 13, 13 }, WindowColour::primary, ImageIds::close_button, StringIds::tooltip_close_window),
        Widgets::Panel({ 0, 15 }, { 380, 205 }, WindowColour::secondary),
        Widgets::ScrollView(Widx::kServerList, { 8, 20 }, { 364, 160 }, WindowColour::secondary, Scrollbars::vertical),
        Widgets::Button(Widx::kJoinByAddressBtn, { 8, 188 }, { 200, 16 }, WindowColour::secondary, StringIds::server_browser_join_by_address)

    );

    // Snapshot refreshed every onUpdate(); rowInfo/rowCount just index into
    // this rather than duplicating the data into the Window struct.
    static std::vector<Network::DiscoveredServer> _servers;

    static const WindowEventList& getEvents();

    Window* open()
    {
        auto window = WindowManager::find(WindowType::serverBrowser);
        if (window != nullptr)
        {
            return WindowManager::bringToFront(*window);
        }

        window = WindowManager::createWindowCentred(
            WindowType::serverBrowser,
            kWindowSize,
            WindowFlags::none,
            getEvents());

        window->setWidgets(widgets);
        window->initScrollWidgets();
        window->setColour(WindowColour::primary, Colour::black);
        window->setColour(WindowColour::secondary, Colour::black);

        // Discovery runs only while this window is open - see onClose().
        _servers.clear();
        Network::beginServerDiscovery();

        return window;
    }

    static void onClose([[maybe_unused]] Window& self)
    {
        Network::endServerDiscovery();
        _servers.clear();
    }

    static void onMouseUp(Window& self, [[maybe_unused]] WidgetIndex_t widgetIndex, const WidgetId id)
    {
        switch (id)
        {
            case Widx::kCloseBtn:
                WindowManager::close(&self);
                break;

            case Widx::kJoinByAddressBtn:
            {
                StringManager::setString(StringIds::buffer_2039, "");
                TextInput::openTextInput(&self, StringIds::enter_host_address, StringIds::enter_host_address_description, StringIds::buffer_2039, widx::joinByAddressBtn, {});
                break;
            }
        }
    }

    static void textInput([[maybe_unused]] Window& self, [[maybe_unused]] WidgetIndex_t widgetIndex, const WidgetId id, const char* str)
    {
        if (id != Widx::kJoinByAddressBtn || str == nullptr || std::strlen(str) == 0)
        {
            return;
        }

        auto [host, port] = Network::parseServerAddress(str);
        Network::joinServer(host, port);
        WindowManager::close(&self);
    }

    static void onUpdate(Window& self)
    {
        self.frameNo++;
        _servers = Network::getDiscoveredServers();
        self.rowCount = static_cast<uint16_t>(_servers.size());
        self.invalidate();
    }

    static void getScrollSize(Window& self, [[maybe_unused]] uint32_t scrollIndex, [[maybe_unused]] int32_t& scrollWidth, int32_t& scrollHeight)
    {
        scrollHeight = self.rowCount * kRowHeight;
    }

    static void onScrollMouseDown(Window& self, [[maybe_unused]] int16_t x, int16_t y, [[maybe_unused]] uint8_t scrollIndex)
    {
        auto row = static_cast<size_t>(y / kRowHeight);
        if (row >= _servers.size())
        {
            return;
        }

        const auto& server = _servers[row];
        if (server.version != Network::kNetworkVersion)
        {
            // Incompatible version - rendered greyed out in drawScroll and
            // deliberately not joinable from here.
            return;
        }

        Network::joinServer(server.address, server.port);
        WindowManager::close(&self);
    }

    static Ui::CursorId cursor([[maybe_unused]] Window& self, [[maybe_unused]] WidgetIndex_t widgetIdx, const WidgetId id, [[maybe_unused]] int16_t xPos, int16_t yPos, Ui::CursorId fallback)
    {
        if (id != Widx::kServerList)
        {
            return fallback;
        }

        auto row = static_cast<size_t>(yPos / kRowHeight);
        if (row < _servers.size() && _servers[row].version == Network::kNetworkVersion)
        {
            return CursorId::handPointer;
        }
        return fallback;
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
        for (const auto& server : _servers)
        {
            auto compatible = server.version == Network::kNetworkVersion;
            auto textColour = compatible ? Colour::black : Colour::grey;

            // Master-sourced entries (docs/multiplayer.md § "Master server
            // (phase 2 - design)") get a subtle "[master]" suffix so players
            // can tell a LAN-discovered server apart from an internet one;
            // LAN entries are unmarked (also the more common/expected case).
            auto sourceSuffix = server.source == Network::DiscoveredServerSource::master ? " [master]" : "";
            auto line = fmt::format("{} - {}/{} - {}:{}{}", server.name, server.playerCount, server.maxPlayers, server.address, server.port, sourceSuffix);
            tr.drawString(point, textColour, line.c_str());
            point.y += kRowHeight;
        }
    }

    static constexpr WindowEventList kEvents = {
        .onClose = onClose,
        .onMouseUp = onMouseUp,
        .onUpdate = onUpdate,
        .getScrollSize = getScrollSize,
        .scrollMouseDown = onScrollMouseDown,
        .textInput = textInput,
        .cursor = cursor,
        .draw = draw,
        .drawScroll = drawScroll,
    };

    static const WindowEventList& getEvents()
    {
        return kEvents;
    }
}
