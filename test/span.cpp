#include "proving.hpp"

#include <cstdint>
#include <memory>
#include <string>

#include "talcite/encoding.hpp"
#include "talcite/column.hpp"
#include "talcite/root.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"

using namespace talcite;

namespace {

// Odczytuje wiersz z narysowanej kartki, żeby przypadki mówiły o tym, co widać, a nie o tym, co
// policzył układ.
std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) talcite::spelled(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

}

TRIAL("Span: bez zawijania chce tyle, ile ma znaków") {
    const ui::Span one{"kot"};

    const ui::Size wants = one.wants({80, 25});

    same(wants.wide, 3);
    same(wants.high, 1);
}

TRIAL("Span: pusty nie chce nic, nawet wiersza") {
    const ui::Span one{""};

    const ui::Size wants = one.wants({80, 25});

    same(wants.wide, 0);
    same(wants.high, 0);
}

TRIAL("Span: ogonki liczą się jako znaki, nie jako bajty") {
    const ui::Span one{"żółw"};

    same(one.wants({80, 25}).wide, 4);
}

TRIAL("Span: znak podwójnej szerokości liczy się za dwa") {
    const ui::Span one{"漢字"};

    same(one.wants({80, 25}).wide, 4);
}

TRIAL("Span: bez zawijania nie ogląda się na dostępne miejsce") {
    const ui::Span one{"bardzo długi napis"};

    const ui::Size wants = one.wants({5, 25});

    same(wants.wide, 18);
    same(wants.high, 1);
}

TRIAL("Span: z zawijaniem łamie się po spacjach") {
    ui::Span one{"ala ma kota", ui::SpanStyle{true}};

    const ui::Size wants = one.wants({7, 25});

    same(wants.high, 2);
    same(wants.wide, 6);

    const View made = one.render({{0, 0}, {7, 2}});

    same(read(made, 0), std::string{"ala ma"});
    same(read(made, 1), std::string{"kota"});
}

TRIAL("Span: zawijanie przełącza się w locie") {
    ui::Span one{"ala ma kota"};

    same(one.wants({7, 25}).high, 1);

    one.wrapping(true);

    same(one.wants({7, 25}).high, 2);
    ought(one.wrapping());
}

TRIAL("Span: słowo dłuższe niż wiersz łamie się w miejscu") {
    ui::Span one{"nieprzewidywalnie", ui::SpanStyle{true}};

    const View made = one.render({{0, 0}, {5, 4}});

    same(read(made, 0), std::string{"niepr"});
    same(read(made, 1), std::string{"zewid"});
    same(read(made, 2), std::string{"ywaln"});
    same(read(made, 3), std::string{"ie"});
}

TRIAL("Span: długie słowo nie zjada wiersza, który już stoi") {
    ui::Span one{"do nieprzewidywalnie", ui::SpanStyle{true}};

    const View made = one.render({{0, 0}, {5, 5}});

    same(read(made, 0), std::string{"do"});
    same(read(made, 1), std::string{"niepr"});
}

TRIAL("Span: łamanie liczy szerokości, nie bajty") {
    ui::Span one{"żółw żółw", ui::SpanStyle{true}};

    same(one.wants({9, 25}).high, 1);
    same(one.wants({8, 25}).high, 2);
}

TRIAL("Span: podwójne znaki też łamią się na kratki, nie na znaki") {
    ui::Span one{"漢字 漢字", ui::SpanStyle{true}};

    same(one.wants({9, 25}).high, 1);
    same(one.wants({8, 25}).high, 2);
}

TRIAL("Span: ciąg spacji nie robi pustych wierszy") {
    ui::Span one{"ala   ma", ui::SpanStyle{true}};

    same(one.wants({3, 25}).high, 2);
}

TRIAL("Span: zerowa szerokość nie zawiesza łamania") {
    const ui::Span one{"cokolwiek", ui::SpanStyle{true}};

    same(one.wants({0, 25}).high, 0);
}

TRIAL("Span: rysuje tylko tyle wierszy, ile dostał") {
    ui::Span one{"ala ma kota i psa", ui::SpanStyle{true}};

    const View made = one.render({{0, 0}, {6, 2}});

    same(read(made, 0), std::string{"ala ma"});
    same(read(made, 1), std::string{"kota i"});
}

TRIAL("Span: bez zawijania kartka ucina to, co wystaje") {
    ui::Span one{"bardzo długi napis"};

    const View made = one.render({{0, 0}, {6, 1}});

    same(read(made, 0), std::string{"bardzo"});
}

TRIAL("Span: napis daje się wymienić i pomiar idzie za nim") {
    ui::Span one{"kot"};

    same(one.text(), std::string{"kot"});
    same(one.wants({80, 25}).wide, 3);

    one.text("kotek");

    same(one.text(), std::string{"kotek"});
    same(one.wants({80, 25}).wide, 5);

    const View made = one.render({{0, 0}, {8, 1}});

    same(read(made, 0), std::string{"kotek"});
}

TRIAL("Span: wymieniony na pusty przestaje czegokolwiek chcieć") {
    ui::Span one{"kot"};

    one.text("");

    same(one.wants({80, 25}).high, 0);
}

// Kolor przychodzi **z roli w skórce**, a nie z napisu: `Span` nie ma już czego ustawiać.
TRIAL("Span: kolor roli jedzie razem ze znakami") {
    ui::Root top;

    const auto one = std::make_shared<ui::Span>("kot");

    top.children().add(one);

    Style told{};

    told.ink = Color{false, 200, 100, 50};

    top.skin(ui::Skin{{{"talcite.normal", told}}});

    const View made = top.render({{0, 0}, {6, 1}});

    ought(!made.at({0, 0}).style.ink.transparent);
    same(made.at({0, 0}).style.ink.red, static_cast<std::uint8_t>(200));

    // Poza napisem nie ma nic — kolor jedzie ze znakami, a nie z prostokątem.
    ought(made.at({5, 0}).style.ink.transparent);
}

TRIAL("Span: wbudowana skórka zostawia barwę terminala") {
    ui::Span one{"kot"};

    const View made = one.render({{0, 0}, {6, 1}});

    ought(made.at({0, 0}).style.ink.transparent);
}

TRIAL("Span: wyrównanie do prawej odsuwa napis od lewej krawędzi") {
    ui::Span one{"kot"};

    one.justify(ui::JustifyStyle::right);

    const View made = one.render({{0, 0}, {7, 1}});

    same(made.at({4, 0}).letter, U'k');
    same(made.at({0, 0}).letter, U' ');
    same(one.justify(), ui::JustifyStyle::right);
}

TRIAL("Span: wyśrodkowanie dzieli nadmiar na pół") {
    ui::Span one{"kot"};

    one.justify(ui::JustifyStyle::center);

    const View made = one.render({{0, 0}, {7, 1}});

    same(made.at({2, 0}).letter, U'k');
}

TRIAL("Span: wyrównanie liczy kratki, nie bajty") {
    ui::Span one{"żółw"};

    one.justify(ui::JustifyStyle::right);

    const View made = one.render({{0, 0}, {6, 1}});

    same(made.at({2, 0}).letter, U'ż');
}

TRIAL("Span: każdy zawinięty wiersz wyrównuje się osobno") {
    ui::Span one{"ala ma kota", ui::SpanStyle{true}};

    one.justify(ui::JustifyStyle::right);

    const View made = one.render({{0, 0}, {7, 2}});

    same(read(made, 0), std::string{" ala ma"});
    same(read(made, 1), std::string{"   kota"});
}

TRIAL("Span: bez skracania napis po prostu się urywa") {
    ui::Span one{"nieprzewidywalnie"};

    const View made = one.render({{0, 0}, {6, 1}});

    same(read(made, 0), std::string{"nieprz"});
}

TRIAL("Span: skracanie z prawej zostawia początek i wielokropek") {
    ui::Span one{"nieprzewidywalnie"};

    one.elide(ui::ElideStyle::right);

    const View made = one.render({{0, 0}, {6, 1}});

    same(read(made, 0), std::string{"niepr…"});
    same(one.elide(), ui::ElideStyle::right);
}

TRIAL("Span: skracanie z lewej zostawia koniec") {
    ui::Span one{"nieprzewidywalnie"};

    one.elide(ui::ElideStyle::left);

    const View made = one.render({{0, 0}, {6, 1}});

    same(read(made, 0), std::string{"…alnie"});
}

TRIAL("Span: skracanie w środku zostawia oba końce") {
    ui::Span one{"nieprzewidywalnie"};

    one.elide(ui::ElideStyle::middle);

    const View made = one.render({{0, 0}, {7, 1}});

    same(read(made, 0), std::string{"nie…nie"});
}

TRIAL("Span: napis, który się mieści, nie jest skracany") {
    ui::Span one{"kot"};

    one.elide(ui::ElideStyle::right);

    const View made = one.render({{0, 0}, {6, 1}});

    same(read(made, 0), std::string{"kot"});
}

TRIAL("Span: jedna kratka to sam wielokropek") {
    ui::Span one{"kotek"};

    one.elide(ui::ElideStyle::right);

    const View made = one.render({{0, 0}, {1, 1}});

    same(read(made, 0), std::string{"…"});
}

TRIAL("Span: skracanie nie zmienia tego, ile napis chce") {
    ui::Span one{"nieprzewidywalnie"};

    one.elide(ui::ElideStyle::right);

    same(one.wants({6, 1}).wide, 17);
}

TRIAL("Span: skracanie liczy kratki podwójnych znaków") {
    ui::Span one{"漢字漢字"};

    one.elide(ui::ElideStyle::right);

    const View made = one.render({{0, 0}, {5, 1}});

    same(read(made, 0), std::string{"漢字…"});
}

TRIAL("Span: w drzewie staje tam, gdzie postawi go rodzic") {
    ui::Column top;

    const auto first = std::make_shared<ui::Span>("pierwszy");
    const auto second = std::make_shared<ui::Span>("drugi");

    top.children().add(first);
    top.children().add(second);

    const View made = top.render({{0, 0}, {10, 3}});

    same(read(made, 0), std::string{"pierwszy"});
    same(read(made, 1), std::string{"drugi"});
    same(second->area().at.y, 1);
}

// --- Twarde końce wiersza ---------------------------------------------------------------------
//
// Dotąd `\n` szedł do kartki jak każdy inny bajt i wychodził z niego znak zastępczy w środku
// zdania. Komunikaty z dwóch zdań pisze się właśnie tak, więc napis musi je rozumieć.

TRIAL("Span: twardy koniec wiersza dzieli, także bez zawijania") {
    const ui::Span one{"pierwszy\ndrugi"};

    const ui::Size wants = one.wants({80, 25});

    same(wants.wide, 8);
    same(wants.high, 2);

    ui::Span other{"pierwszy\ndrugi"};

    const View made = other.render({{0, 0}, {10, 3}});

    same(read(made, 0), std::string{"pierwszy"});
    same(read(made, 1), std::string{"drugi"});
}

TRIAL("Span: pusty akapit zostaje pustym wierszem") {
    ui::Span one{"nad\n\npod"};

    const View made = one.render({{0, 0}, {10, 4}});

    same(read(made, 0), std::string{"nad"});
    same(read(made, 1), std::string{});
    same(read(made, 2), std::string{"pod"});
    same(one.wants({80, 25}).high, 3);
}

TRIAL("Span: koniec wiersza na końcu daje pusty wiersz po nim") {
    const ui::Span one{"sam\n"};

    same(one.wants({80, 25}).high, 2);
}

TRIAL("Span: powrót karetki nie zostaje w treści") {
    ui::Span one{"pierwszy\r\ndrugi"};

    const View made = one.render({{0, 0}, {12, 2}});

    same(read(made, 0), std::string{"pierwszy"});
    same(read(made, 1), std::string{"drugi"});
    same(one.wants({80, 25}).wide, 8);
}

TRIAL("Span: każdy akapit zawija się osobno") {
    ui::Span one{"ala ma kota\nas", ui::SpanStyle{true}};

    const View made = one.render({{0, 0}, {7, 4}});

    same(read(made, 0), std::string{"ala ma"});
    same(read(made, 1), std::string{"kota"});
    same(read(made, 2), std::string{"as"});
    same(one.wants({7, 25}).high, 3);
}

TRIAL("Span: wyrównanie liczy się w każdym wierszu osobno") {
    ui::Span one{"dluuugi\nkot", ui::SpanStyle{false, ui::JustifyStyle::center}};

    const View made = one.render({{0, 0}, {9, 2}});

    same(read(made, 0), std::string{" dluuugi"});
    same(read(made, 1), std::string{"   kot"});
}

