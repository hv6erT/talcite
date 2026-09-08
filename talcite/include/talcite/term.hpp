#pragma once

#include <memory>
#include <string_view>

#include "talcite/loop.hpp"
#include "talcite/root.hpp"
#include "talcite/setup.hpp"

namespace talcite {

class Term {
public:
    explicit Term(Setup given = {});

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

    void onto(ui::Root& given);

    [[nodiscard]] ui::Root* onto() const;

    [[nodiscard]] int wide() const;

    [[nodiscard]] int high() const;

    void title(std::string_view said);

    void draw();

    bool turn();

    void run();

    void stop();

    class Aside {
    public:
        explicit Aside(Term& given);

        ~Aside();

        Aside(const Aside&) = delete;
        Aside& operator=(const Aside&) = delete;
        Aside(Aside&&) = delete;
        Aside& operator=(Aside&&) = delete;

    private:
        Term& term_;
    };

private:
    struct Held;

    std::unique_ptr<Held> held_;
};

}
