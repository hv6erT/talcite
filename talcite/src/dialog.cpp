#include "talcite/dialog.hpp"

namespace talcite::ui {

Dialog::Dialog(ModalShape shape) : Modal(shape) {}

// The form dialog is a modal plus two rules, and both are rules of the window, not of a widget.
// Everything else (full screen, modality, Esc, moving with Tab, the shadow and closing) belongs to
// `Modal` and is not repeated here.
//
// The dialog does not know what a button is. Enter presses only the button that has the focus,
// and the button does that itself, because Enter reaches it before it reaches the dialog. The
// window does not search the tree for anyone and presses nothing on the caller's behalf.
// `Button::primary()` is only a marking, which draws the bracket with arrows.
bool Dialog::handle(KeyEvent& what) {
    const Code code = what.key().code();

    // A shortcut key is a broadcast, not a question about types. The window does not guess who
    // answers what: it passes the event down and waits until someone takes it. A node that wants
    // to answer overrides `handle(ShortcutEvent&)` or attaches `on<>`, on equal terms, through one
    // mechanism.
    //
    // This happens before the key is handed to the modal, because the modal swallows everything it
    // did not recognize. Escape and Tab are not broadcast: the modal settles those two earlier, and
    // so it does the up and down arrows, which move the focus like Tab; no hotkey is an arrow.
    const bool walking = code == Code::up || code == Code::down;

    if (code != Code::escape && code != Code::tab && !walking) {
        if (broadcast(what)) return true;

        // Alt with a letter that nobody took is the letter as a hot key, as in Midnight Commander:
        // a text field keeps a plain letter for itself, so Alt is how a button is reached from it.
        // A node that answers to Alt with that letter itself was asked first, with the key as it
        // came.
        const Mods mods = what.mods();
        const char32_t letter = what.key().letter();

        if (mods.alt && !mods.ctrl && letter != 0) {
            KeyEvent plain{Key{letter}};

            (void)broadcast(plain);
        }

        return true;
    }

    return Modal::handle(what);
}

}
