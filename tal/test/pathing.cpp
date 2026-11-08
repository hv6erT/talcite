#include <string>

#include "tal/bytes.hpp"
#include "tal/path.hpp"

#include "tal/testing.hpp"

// Paths of tal::core: their one stored form, joining and taking apart, comparing as the system
// does and showing names that are not UTF-8.

namespace {

// The stored form of a path, as a string the trials compare.
std::string kept(const tal::Path& one) { return std::string{one.stored()}; }

}

TRIAL("Path: separators, empty and \".\" names are normalised; \"..\" stays") {
    SAME(kept(tal::Path{"/a//b/./c/"}), std::string{"/a/b/c"});
    SAME(kept(tal::Path{"a/b/"}), std::string{"a/b"});
    SAME(kept(tal::Path{"./"}), std::string{"."});
    SAME(kept(tal::Path{"/a/../b"}), std::string{"/a/../b"});
    SAME(kept(tal::Path{"/"}), std::string{"/"});
    OUGHT(tal::Path{}.empty());
    OUGHT(tal::Path{""}.empty());
    OUGHT(tal::Path{"/x"}.absolute());
    OUGHT(!tal::Path{"x"}.absolute());
}

TRIAL("Path: joining a name, several names, an empty name or onto an empty path") {
    SAME(kept(tal::Path{"/"} / "a"), std::string{"/a"});
    SAME(kept(tal::Path{"/a"} / "b/c"), std::string{"/a/b/c"});
    SAME(kept(tal::Path{"/a"} / ""), std::string{"/a"});
    SAME(kept(tal::Path{"/a"} / "/b"), std::string{"/a/b"});
    SAME(kept(tal::Path{} / "x"), std::string{"x"});
    SAME(kept(tal::Path{"."} / "x"), std::string{"x"});
}

TRIAL("Path: parent and name; a root is its own parent and has no name") {
    const tal::Path file{"/films/Poirot/odcinek.mkv"};

    SAME(kept(file.parent()), std::string{"/films/Poirot"});
    SAME(std::string{file.name()}, std::string{"odcinek.mkv"});
    SAME(kept(tal::Path{"/films"}.parent()), std::string{"/"});
    SAME(kept(tal::Path{"/"}.parent()), std::string{"/"});
    SAME(std::string{tal::Path{"/"}.name()}, std::string{});
    OUGHT(tal::Path{"/"}.root());
    OUGHT(!file.root());
    OUGHT(tal::Path{"plik"}.parent().empty());
    SAME(kept(tal::Path{"a/b"}.parent()), std::string{"a"});
}

TRIAL("Path: the extension is after the last dot, unless the dot starts or ends the name") {
    const tal::Path packed{"/x/archive.tar.gz"};

    SAME(std::string{packed.extension()}, std::string{"gz"});
    SAME(std::string{packed.stem()}, std::string{"archive.tar"});
    SAME(std::string{tal::Path{"/x/.bashrc"}.extension()}, std::string{});
    SAME(std::string{tal::Path{"/x/.bashrc"}.stem()}, std::string{".bashrc"});
    SAME(std::string{tal::Path{"/x/a."}.extension()}, std::string{});
    SAME(std::string{tal::Path{"/x/a."}.stem()}, std::string{"a."});
    SAME(std::string{tal::Path{"/x/README"}.stem()}, std::string{"README"});
}

TRIAL("Path: within() holds for the folder itself and what lies below it, at a name's end") {
    const tal::Path folder{"/films/Poirot"};

    OUGHT(tal::Path{"/films/Poirot/s01/e01.mkv"}.within(folder));
    OUGHT(folder.within(folder));
    OUGHT(!tal::Path{"/films/Poirot (1989)"}.within(folder));
    OUGHT(!tal::Path{"/films"}.within(folder));
    OUGHT(tal::Path{"/films"}.within(tal::Path{"/"}));
    OUGHT(!tal::Path{"/films"}.within(tal::Path{}));
}

TRIAL("Path: == is exact; SAME() and within() compare as the system does") {
    OUGHT(tal::Path{"/A"} != tal::Path{"/a"});
    OUGHT(tal::Path{"/a"} < tal::Path{"/b"});
#ifdef _WIN32
    OUGHT((tal::Path{"/Filmy/ŁÓDŹ"}.same)(tal::Path{"/filmy/łódź"}));
    OUGHT(tal::Path{"/filmy/łódź/x"}.within(tal::Path{"/FILMY/ŁÓDŹ"}));
#else
    OUGHT(!(tal::Path{"/Filmy"}.same)(tal::Path{"/filmy"}));
    OUGHT(!tal::Path{"/filmy/x"}.within(tal::Path{"/Filmy"}));
#endif
    OUGHT((tal::Path{"/filmy"}.same)(tal::Path{"/filmy/"}));
}

TRIAL("Path: names that are not UTF-8 are kept whole and shown with U+FFFD") {
    const std::string broken = "/a\xFF" "b/\xE2\x82";
    const tal::Path odd = tal::Path::restored(broken);

    SAME(kept(odd), broken);
    SAME(odd.text(), std::string{"/a\xEF\xBF\xBD" "b/\xEF\xBF\xBD"});
    OUGHT(tal::Path::restored(odd.stored()) == odd);
    SAME(tal::Path::restored("/\xED\xA0\x80").text(), std::string{"/\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"});
    SAME(tal::Path{"/zażółć/gęślą"}.text(), std::string{"/zażółć/gęślą"});
}

#ifdef _WIN32
TRIAL("Path: on Windows backslashes separate names, drives and shares are roots") {
    SAME(kept(tal::Path{"C:\\Filmy\\Poirot\\"}), std::string{"C:/Filmy/Poirot"});
    SAME(kept(tal::Path{"c:"}), std::string{"c:/"});
    OUGHT(tal::Path{"C:/"}.root());
    SAME(kept(tal::Path{"C:/Filmy"}.parent()), std::string{"C:/"});
    SAME(kept(tal::Path{"\\\\teka\\Biblioteka\\Filmy"}.parent()), std::string{"//teka/Biblioteka"});
    OUGHT(tal::Path{"//teka/Biblioteka"}.root());
    SAME(kept(tal::Path{"//teka/Biblioteka"} / "Filmy"), std::string{"//teka/Biblioteka/Filmy"});
    OUGHT((tal::Path{"C:/A"}.same)(tal::Path{"c:/a"}));
}
#endif

TRIAL("Bytes: text and bytes turn into each other unchanged") {
    const std::string given{"zażółć\0\xFF", sizeof("zażółć\0\xFF") - 1};

    SAME(tal::text::of(tal::bytes(given)), given);
    SAME(tal::bytes("ab").size(), std::size_t{2});
}
