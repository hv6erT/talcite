#include "tal/fonts.hpp"

#include "fonts_of.hpp"

// The font built in (E7b): DejaVu Sans 2.37, carried inside the program (fonts/DejaVuSans.ttf, its
// license in fonts/LICENSE, which a program that ships it must ship too). It stands in a source of
// its own, so that a program linked statically takes the font's 740 KB only when it calls
// `Font::builtin`: the linker leaves out an object that nothing asks for.
namespace tal::drawing {

// DejaVu Sans, read once.
Font Font::builtin() {
    static const Font made = Font::read(Bytes{reinterpret_cast<const std::byte*>(fonts_of::DejaVuSans.data()), reinterpret_cast<const std::byte*>(fonts_of::DejaVuSans.data()) + fonts_of::DejaVuSans.size()});

    return made;
}

}
