#pragma once

#include <memory>
#include <string_view>

#include "talcite/child.hpp"
#include "talcite/console.hpp"
#include "talcite/loop.hpp"
#include "talcite/root.hpp"
#include "talcite/setup.hpp"

namespace talcite {

class Term {
public:
    explicit Term(Setup given = {}, Seam<Console> with = console());

    ~Term();

    Term(const Term&) = delete;
    Term& operator=(const Term&) = delete;
    Term(Term&&) = delete;
    Term& operator=(Term&&) = delete;

    [[nodiscard]] Loop& loop() const;

    void reconfigure(Setup given);

    [[nodiscard]] Colors colors() const;

    [[nodiscard]] Mouse mouse() const;

    [[nodiscard]] Shade shade() const;

    [[nodiscard]] Graphics graphics() const;

    [[nodiscard]] ui::Blocks blocks() const;

    [[nodiscard]] ui::Size cell() const;

    [[nodiscard]] bool unicode() const;

    void show(ui::Root& given);

    [[nodiscard]] ui::Root* shown() const;

    [[nodiscard]] ui::Size size() const;

    void title(std::string_view said);

    void draw();

    bool turn();

    void run();

    void stop();

    [[nodiscard]] Exit execute(Command what);

    class Pause {
    public:
        explicit Pause(Term& given);

        ~Pause();

        Pause(const Pause&) = delete;
        Pause& operator=(const Pause&) = delete;
        Pause(Pause&&) = delete;
        Pause& operator=(Pause&&) = delete;

    private:
        Term& term_;
    };

private:
    struct Held;

    std::unique_ptr<Held> held_;
};

}
