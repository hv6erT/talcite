#pragma once

#include <memory>
#include <string_view>

#include "tal/capabilities.hpp"
#include "tal/clock.hpp"
#include "tal/console.hpp"
#include "tal/document.hpp"
#include "tal/executor.hpp"
#include "tal/geometry.hpp"
#include "tal/port.hpp"
#include "tal/style.hpp"
#include "tal/text.hpp"

namespace tal {

struct Abilities {
    Size size;
    Colors colors = Colors::mono;
    Mouse mouse = Mouse::none;
    Screen screen = Screen::none;
    Shade shade = Shade::neutral;
    Graphics graphics = Graphics::none;
    Blocks blocks = Blocks::basic;
    Size cell;
    bool unicode = false;
    text::Rendering rendering;
    bool links = false;
    bool titles = false;
    bool synchronized = false;
    bool bracketed_paste = false;

    friend bool operator==(const Abilities&, const Abilities&) = default;
};

class App {
public:
    explicit App(Setup wish = {}, Port<Console> console = {}, Port<Clock> time = {});

    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    [[nodiscard]] Loop& loop();

    void reconfigure(Setup wish);

    [[nodiscard]] const Abilities& abilities() const;

    void show(Document& one);

    [[nodiscard]] Document* shown() const;

    void title(std::string_view said);

    bool turn();

    int run();

    void stop(int code = 0);

    class Pause {
    public:
        explicit Pause(App& one);

        ~Pause();

        Pause(const Pause&) = delete;
        Pause& operator=(const Pause&) = delete;
        Pause(Pause&&) = delete;
        Pause& operator=(Pause&&) = delete;

    private:
        App& app_;
    };

private:
    friend class Document;

    struct Held;

    void draw();

    void forget();

    std::unique_ptr<Held> held_;
};

}
