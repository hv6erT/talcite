#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/history.hpp"
#include "talcite/root.hpp"
#include "talcite/veilwindow.hpp"

using namespace talcite;

namespace {

void counted(const std::shared_ptr<ui::History>& one, int& done) {
    (void)one->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });
}

// Historia **w oknie**, bo bez okna zasłon nie ma jej gdzie rozwinąć.
struct Stage {
    ui::Root top;
    std::shared_ptr<ui::VeilWindow> window = std::make_shared<ui::VeilWindow>();
    std::shared_ptr<ui::Column> column = std::make_shared<ui::Column>();
    std::shared_ptr<ui::History> one = std::make_shared<ui::History>(ui::plenty);

    Stage() {
        column->children().add(one);
        window->body()->children().add(column);
        top.children().add(window);
        one->focus();
    }

    // Trzy polecenia, najświeższe pierwsze — czyli „git status" jest tym, do którego wraca się
    // pierwszą strzałką w górę.
    void filled() {
        one->options().insert(0, "make -j8");
        one->options().insert(0, "ls -la /etc");
        one->options().insert(0, "git status");
    }

    View drawn(ui::Size size = {40, 12}) { return top.render({{0, 0}, size}); }
};

}

// --- Co historia wie o sobie ---------------------------------------------------------------------

TRIAL("History: świeża nic nie pamięta i jest zwykłym polem") {
    const ui::History one{20};

    ought(one.options().empty());
    ought(one.text().empty());
    ought(one.focusable());
    ought(!one.opened());
    same(one.keeps(), 60);
}

TRIAL("History: dokładanie stawia najświeższy wpis na czele") {
    Stage stage;

    stage.filled();

    same(static_cast<int>(stage.one->options().many()), 3);
    same(stage.one->options().front(), std::string{"git status"});
    same(stage.one->options().back(), std::string{"make -j8"});
}

// Powtórzony wpis **wraca na czoło** zamiast leżeć dwa razy — inaczej historia człowieka, który
// dziesięć razy wpisał to samo, składałaby się z niego jednego.
TRIAL("History: powtórzony wpis wraca na czoło, a nie dopisuje się drugi raz") {
    Stage stage;

    stage.filled();
    stage.one->options().insert(0, "make -j8");

    same(static_cast<int>(stage.one->options().many()), 3);
    same(stage.one->options().front(), std::string{"make -j8"});
}

// Reguła należy dziś do zbioru, nie do historii — i obowiązuje tak samo w wyborze i w grupie opcji.
TRIAL("History: pusty wpis nie wchodzi") {
    Stage stage;

    stage.one->options().insert(0, "");

    ought(stage.one->options().empty());
}

// **Sufit obowiązuje od najbliższej klatki.** Historia sprząta po sobie sama — przy malowaniu
// i przy klawiszu — a nie każe się zawiadamiać o każdej zmianie zbioru. Między dopisaniem wpisu
// a pierwszym użyciem pola lista bywa więc o wpis za długa i to jest cena tego, że `Options`
// nie zna swojego właściciela.
TRIAL("History: sufit ucina najstarsze") {
    Stage stage;

    stage.one->keeps(2);
    stage.filled();

    (void)stage.drawn();

    same(static_cast<int>(stage.one->options().many()), 2);
    same(stage.one->options().front(), std::string{"git status"});
    same(stage.one->options().back(), std::string{"ls -la /etc"});
}

TRIAL("History: obniżenie sufitu ucina to, co już jest") {
    Stage stage;

    stage.filled();
    stage.one->keeps(1);

    same(static_cast<int>(stage.one->options().many()), 1);
    same(stage.one->options().front(), std::string{"git status"});
}

TRIAL("History: clear czyści wszystko") {
    Stage stage;

    stage.filled();
    stage.one->clear();

    ought(stage.one->options().empty());
}

// --- Wędrówka strzałkami -------------------------------------------------------------------------

TRIAL("History: góra wchodzi w przeszłość, dół z niej wraca") {
    Stage stage;

    stage.filled();

    ui::KeyEvent up{ui::Code::up};

    ought(stage.top.deal(up));
    same(stage.one->text(), std::string{"git status"});

    ought(stage.top.deal(up));
    same(stage.one->text(), std::string{"ls -la /etc"});

    ui::KeyEvent down{ui::Code::down};

    ought(stage.top.deal(down));
    same(stage.one->text(), std::string{"git status"});
}

// **To jest ta rzecz, bez której historia jest pułapką**: raz wszedłeś i nie ma jak wrócić do
// swojego wiersza. Dół z najświeższego wpisu przywraca to, co użytkownik pisał.
TRIAL("History: dół z najświeższego wraca do tego, co pisał użytkownik") {
    Stage stage;

    stage.filled();
    stage.one->text("cos wlasnego");

    ui::KeyEvent up{ui::Code::up};

    ought(stage.top.deal(up));
    same(stage.one->text(), std::string{"git status"});

    ui::KeyEvent down{ui::Code::down};

    ought(stage.top.deal(down));
    same(stage.one->text(), std::string{"cos wlasnego"});
}

// Kolejne kroki w górę mają wracać **do tego samego** własnego wiersza, a nie do poprzedniego wpisu.
TRIAL("History: własny wiersz zapamiętuje się raz, przy wejściu w przeszłość") {
    Stage stage;

    stage.filled();
    stage.one->text("moje");

    ui::KeyEvent up{ui::Code::up};
    ui::KeyEvent down{ui::Code::down};

    ought(stage.top.deal(up));
    ought(stage.top.deal(up));
    ought(stage.top.deal(up));

    same(stage.one->text(), std::string{"make -j8"});

    ought(stage.top.deal(down));
    ought(stage.top.deal(down));
    ought(stage.top.deal(down));

    same(stage.one->text(), std::string{"moje"});
}

// Na krańcu strzałka **idzie wyżej**, żeby w oknie przeszła dalej zamiast utknąć.
TRIAL("History: na krańcach oddaje strzałkę wyżej") {
    Stage stage;

    stage.filled();

    ui::KeyEvent down{ui::Code::down};

    // Przy własnym wierszu nie ma dokąd iść w przód.
    ought(!stage.top.deal(down));

    ui::KeyEvent up{ui::Code::up};

    ought(stage.top.deal(up));
    ought(stage.top.deal(up));
    ought(stage.top.deal(up));

    // Najstarszy wpis to koniec przeszłości.
    ought(!stage.top.deal(up));
    same(stage.one->text(), std::string{"make -j8"});
}

TRIAL("History: pusta nie bierze strzałek") {
    Stage stage;

    ui::KeyEvent up{ui::Code::up};

    ought(!stage.top.deal(up));

    ui::KeyEvent down{ui::Code::down};

    ought(!stage.top.deal(down));
}

// --- Zawiadamianie -------------------------------------------------------------------------------

// Zawiadamiamy, **ilekroć wpis z przeszłości trafia do pola**. Pisania nikt nie ogłasza i ogłaszać
// nie będzie; powrót do własnego wiersza też nie jest wzięciem niczego z przeszłości.
TRIAL("History: wzięcie wpisu zawiadamia, powrót do swojego już nie") {
    Stage stage;

    stage.filled();

    int done = 0;

    counted(stage.one, done);

    ui::KeyEvent up{ui::Code::up};

    ought(stage.top.deal(up));
    same(done, 1);

    ought(stage.top.deal(up));
    same(done, 2);

    ui::KeyEvent down{ui::Code::down};

    ought(stage.top.deal(down));
    same(done, 3);

    ought(stage.top.deal(down));
    same(done, 3);
}

TRIAL("History: pisanie nie zawiadamia") {
    Stage stage;

    stage.filled();

    int done = 0;

    counted(stage.one, done);

    ui::KeyEvent letter{U'x'};

    ought(stage.top.deal(letter));

    same(stage.one->text(), std::string{"x"});
    same(done, 0);
}

// --- Pole tekstowe pod spodem --------------------------------------------------------------------

// Historia **jest polem** i niczego mu nie odbiera: litery, kasowanie i kursor mają działać dalej.
TRIAL("History: dalej jest zwykłym polem tekstowym") {
    Stage stage;

    stage.filled();

    for (const char32_t one : {U'a', U'b', U'c'}) {
        ui::KeyEvent what{one};

        ought(stage.top.deal(what));
    }

    same(stage.one->text(), std::string{"abc"});

    ui::KeyEvent left{ui::Code::left};

    ought(stage.top.deal(left));

    ui::KeyEvent back{ui::Code::backspace};

    ought(stage.top.deal(back));

    same(stage.one->text(), std::string{"ac"});
}

// Wpis wzięty z przeszłości ma kursor **na końcu**, żeby dało się od razu dopisywać.
TRIAL("History: wzięty wpis ma kursor na końcu") {
    Stage stage;

    stage.filled();

    ui::KeyEvent up{ui::Code::up};

    ought(stage.top.deal(up));

    same(stage.one->cursor(), std::string{"git status"}.size());
}

// --- Rozwijanie ----------------------------------------------------------------------------------

// **Domyślnego skrótu nie ma.** Pole stało kiedyś na Alt+H przypisanym w konstruktorze, zanim
// wołający cokolwiek powiedział. Kto chce rozwijanie z klawiatury, podaje klawisz sam.
TRIAL("History: bez podanego klawisza nic nie rozwija") {
    Stage stage;

    stage.filled();

    ui::Mods alt;
    alt.alt = true;

    ui::KeyEvent what{ui::Key{U'h', alt}};

    ought(!stage.top.deal(what));
    ought(!stage.one->opened());
}

TRIAL("History: podany klawisz rozwijający otwiera listę, Esc ją zamyka") {
    Stage stage;

    stage.filled();

    ui::Mods alt;
    alt.alt = true;

    stage.one->keys().add(ui::Key{U'h', alt});

    ui::KeyEvent what{ui::Key{U'h', alt}};

    ought(stage.top.deal(what));
    ought(stage.one->opened());
    same(static_cast<int>(stage.window->veils().size()), 1);

    ui::KeyEvent escape{ui::Code::escape};

    ought(stage.top.deal(escape));

    ought(!stage.one->opened());
    same(stage.top.focus(), static_cast<ui::Node*>(stage.one.get()));
}

TRIAL("History: klawisz rozwijający da się przestawić") {
    Stage stage;

    stage.filled();
    stage.one->keys().add(ui::Key{ui::Code::f4});

    ui::KeyEvent what{ui::Code::f4};

    ought(stage.top.deal(what));
    ought(stage.one->opened());
}

// Sam klawisz bez modyfikatora ma **pisać**, a nie rozwijać.
TRIAL("History: sama litera bez modyfikatora nie rozwija") {
    Stage stage;

    stage.filled();

    ui::KeyEvent what{U'h'};

    ought(stage.top.deal(what));

    ought(!stage.one->opened());
    same(stage.one->text(), std::string{"h"});
}

TRIAL("History: pusta nie ma czego rozwijać") {
    Stage stage;

    ui::Mods alt;
    alt.alt = true;

    ui::KeyEvent what{ui::Key{U'h', alt}};

    (void)stage.top.deal(what);

    ought(!stage.one->opened());
    ought(stage.window->veils().empty());
}

// Rozwinięta lista staje **pod polem**, tak samo jak przy wyborze — bo to ta sama zasłona.
TRIAL("History: rozwinięta lista staje pod polem") {
    Stage stage;

    stage.filled();

    (void)stage.drawn();

    stage.one->open();

    const View made = stage.drawn();

    same(made.at({0, 1}).letter, U'┌');
    same(made.at({1, 2}).letter, U'g');
}

TRIAL("History: wybór z listy trafia do pola i zawiadamia") {
    Stage stage;

    stage.filled();

    int done = 0;

    counted(stage.one, done);

    stage.one->open();

    (void)stage.drawn();

    ui::KeyEvent down{ui::Code::down};

    ought(stage.top.deal(down));

    ui::KeyEvent enter{ui::Code::enter};

    ought(stage.top.deal(enter));

    same(stage.one->text(), std::string{"ls -la /etc"});
    same(done, 1);
    ought(!stage.one->opened());
}

// Dołożenie świeżego wpisu kończy wędrówkę: stoimy znowu przy tym, co w polu.
TRIAL("History: dołożenie wpisu kończy wędrówkę") {
    Stage stage;

    stage.filled();

    ui::KeyEvent up{ui::Code::up};

    ought(stage.top.deal(up));
    same(stage.one->text(), std::string{"git status"});

    stage.one->options().insert(0, "cargo build");

    ui::KeyEvent down{ui::Code::down};

    // Skoro wędrówka się skończyła, dół nie ma dokąd iść i idzie wyżej.
    ought(!stage.top.deal(down));
}

// --- Wyłączona -----------------------------------------------------------------------------------

TRIAL("History: wyłączona nie rozwija i nie wędruje") {
    Stage stage;

    stage.filled();
    stage.one->able(false);

    stage.one->open();

    ought(!stage.one->opened());

    ui::KeyEvent up{ui::Code::up};

    (void)stage.top.deal(up);

    ought(stage.one->text().empty());
}

// Historia jest dziś węzłem z klawiszami, więc jej klawisz rozwijający leży w `keys()` — razem
// z modyfikatorami, co jest całym powodem, dla którego `Key` się ich nauczył.
TRIAL("History: klawisz z Altem odpowiada tylko z Altem") {
    Stage stage;

    stage.filled();

    ui::Mods alt;
    alt.alt = true;

    stage.one->keys().add(ui::Key{U'h', alt});

    ui::KeyEvent bare{ui::Key{U'h'}};

    ought(stage.top.deal(bare));
    ought(!stage.one->opened());
    same(stage.one->text(), std::string{"h"});

    ui::KeyEvent with{ui::Key{U'h', alt}};

    ought(stage.top.deal(with));
    ought(stage.one->opened());
}
