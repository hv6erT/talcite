#include "tal/app.hpp"
#include "tal/controls.hpp"
#include "tal/document.hpp"
#include "tal/layout.hpp"

// tal-hello, the smallest program on tal and the example of the README: a document holding a
// column of two labels, shown in the terminal until q or F10. The application is made first and
// the document after it, so the document goes first at the end, which tal allows.
int main() {
    tal::App app;
    tal::Document document;
    tal::Column& column = document.children().add<tal::Column>(1);

    column.children().add<tal::Label>("Hello from tal.");
    column.children().add<tal::Label>("q or F10 quits.");

    const tal::Hook keys = document.on<tal::KeyEvent>([&app](tal::KeyEvent& event) {
        if (event.key == tal::Key{U'q'} || event.key == tal::Key{tal::Code::f10}) app.stop();
    });

    app.show(document);

    return app.run();
}
