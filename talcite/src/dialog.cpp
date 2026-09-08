#include "talcite/dialog.hpp"

namespace talcite::ui {

Dialog::Dialog(VeilStyle style) : Veil(style) {}

// **Zasłona formularzowa** — czyli zasłona plus dwie reguły, i obie są regułami okna, nie widżetu.
// Wszystko inne: pełny ekran, modalność, Esc, wędrówka Tabem, cień i zamykanie — należy do `Veil`
// i tu się nie powtarza.
//
// **Dialog nie wie już, czym jest przycisk.** Enter naciska wyłącznie ten przycisk, który ma
// ognisko — a robi to sam przycisk, bo Enter dochodzi do niego wcześniej niż tu. Okno nie szuka
// nikogo po drzewie i nie naciska niczego za wołającego.
//
// Wcześniej stała tu wolna funkcja schodząca w głąb z `dynamic_cast` i wybierająca „przycisk
// podstawowy". Robiła za wołającego coś, o co ten nie prosił, i była jedynym miejscem w bibliotece,
// gdzie okno rozpoznawało rodzaj swojego dziecka. `Button::primary()` zostaje — ale jest **tylko
// oznaczeniem**, które rysuje klamrę ze strzałkami.
bool Dialog::react(KeyEvent& what) {
    const Code code = what.key().code();

    // **Klawisz aktywny to rozdanie, a nie pytanie o typ.** Okno nie zgaduje, kto na co odpowiada —
    // podaje zdarzenie w głąb i czeka, aż ktoś je weźmie. Kto chce odpowiadać, przesłania
    // `react(HotEvent<KeyEvent>&)` albo wiesza `on<>` — na równych prawach, jednym mechanizmem.
    //
    // Robimy to **przed** oddaniem klawisza zasłonie, bo zasłona i tak połknie wszystko, czego nie
    // rozpoznała. Escape i Tab do rozdania nie trafiają: te dwa zasłona rozstrzyga wcześniej.
    if (code != Code::escape && code != Code::tab) {
        (void)offer(what);

        return true;
    }

    return Veil::react(what);
}

}
