#include <string>

#include "tal/bytes.hpp"
#include "tal/path.hpp"

#include "proving.hpp"

// Paths of tal::core: their one stored form, joining and taking apart, comparing as the system
// does and showing names that are not UTF-8.

namespace {

// The stored form of a path, as a string the trials compare.
std::string kept(const tal::Path& one) { return std::string{one.stored()}; }

}

TRIAL("Path: separators, empty and \".\" names are normalised; \"..\" stays") {
    same(kept(tal::Path{"/a//b/./c/"}), std::string{"/a/b/c"});
    same(kept(tal::Path{"a/b/"}), std::string{"a/b"});
    same(kept(tal::Path{"./"}), std::string{"."});
    same(kept(tal::Path{"/a/../b"}), std::string{"/a/../b"});
    same(kept(tal::Path{"/"}), std::string{"/"});
    ought(tal::Path{}.empty());
    ought(tal::Path{""}.empty());
    ought(tal::Path{"/x"}.absolute());
    ought(!tal::Path{"x"}.absolute());
}

TRIAL("Path: joining a name, several names, an empty name or onto an empty path") {
    same(kept(tal::Path{"/"} / "a"), std::string{"/a"});
    same(kept(tal::Path{"/a"} / "b/c"), std::string{"/a/b/c"});
    same(kept(tal::Path{"/a"} / ""), std::string{"/a"});
    same(kept(tal::Path{"/a"} / "/b"), std::string{"/a/b"});
    same(kept(tal::Path{} / "x"), std::string{"x"});
    same(kept(tal::Path{"."} / "x"), std::string{"x"});
}

TRIAL("Path: parent and name; a root is its own parent and has no name") {
    const tal::Path file{"/films/Poirot/odcinek.mkv"};

    same(kept(file.parent()), std::string{"/films/Poirot"});
    same(std::string{file.name()}, std::string{"odcinek.mkv"});
    same(kept(tal::Path{"/films"}.parent()), std::string{"/"});
    same(kept(tal::Path{"/"}.parent()), std::string{"/"});
    same(std::string{tal::Path{"/"}.name()}, std::string{});
    ought(tal::Path{"/"}.root());
    ought(!file.root());
    ought(tal::Path{"plik"}.parent().empty());
    same(kept(tal::Path{"a/b"}.parent()), std::string{"a"});
}

TRIAL("Path: the extension is after the last dot, unless the dot starts or ends the name") {
    const tal::Path packed{"/x/archive.tar.gz"};

    same(std::string{packed.extension()}, std::string{"gz"});
    same(std::string{packed.stem()}, std::string{"archive.tar"});
    same(std::string{tal::Path{"/x/.bashrc"}.extension()}, std::string{});
    same(std::string{tal::Path{"/x/.bashrc"}.stem()}, std::string{".bashrc"});
    same(std::string{tal::Path{"/x/a."}.extension()}, std::string{});
    same(std::string{tal::Path{"/x/a."}.stem()}, std::string{"a."});
    same(std::string{tal::Path{"/x/README"}.stem()}, std::string{"README"});
}

TRIAL("Path: within() holds for the folder itself and what lies below it, at a name's end") {
    const tal::Path folder{"/films/Poirot"};

    ought(tal::Path{"/films/Poirot/s01/e01.mkv"}.within(folder));
    ought(folder.within(folder));
    ought(!tal::Path{"/films/Poirot (1989)"}.within(folder));
    ought(!tal::Path{"/films"}.within(folder));
    ought(tal::Path{"/films"}.within(tal::Path{"/"}));
    ought(!tal::Path{"/films"}.within(tal::Path{}));
}

TRIAL("Path: == is exact; same() and within() compare as the system does") {
    ought(tal::Path{"/A"} != tal::Path{"/a"});
    ought(tal::Path{"/a"} < tal::Path{"/b"});
#ifdef _WIN32
    ought((tal::Path{"/Filmy/ŁÓDŹ"}.same)(tal::Path{"/filmy/łódź"}));
    ought(tal::Path{"/filmy/łódź/x"}.within(tal::Path{"/FILMY/ŁÓDŹ"}));
#else
    ought(!(tal::Path{"/Filmy"}.same)(tal::Path{"/filmy"}));
    ought(!tal::Path{"/filmy/x"}.within(tal::Path{"/Filmy"}));
#endif
    ought((tal::Path{"/filmy"}.same)(tal::Path{"/filmy/"}));
}

TRIAL("Path: names that are not UTF-8 are kept whole and shown with U+FFFD") {
    const std::string broken = "/a\xFF" "b/\xE2\x82";
    const tal::Path odd = tal::Path::restored(broken);

    same(kept(odd), broken);
    same(odd.text(), std::string{"/a\xEF\xBF\xBD" "b/\xEF\xBF\xBD"});
    ought(tal::Path::restored(odd.stored()) == odd);
    same(tal::Path::restored("/\xED\xA0\x80").text(), std::string{"/\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"});
    same(tal::Path{"/zażółć/gęślą"}.text(), std::string{"/zażółć/gęślą"});
}

#ifdef _WIN32
TRIAL("Path: on Windows backslashes separate names, drives and shares are roots") {
    same(kept(tal::Path{"C:\\Filmy\\Poirot\\"}), std::string{"C:/Filmy/Poirot"});
    same(kept(tal::Path{"c:"}), std::string{"c:/"});
    ought(tal::Path{"C:/"}.root());
    same(kept(tal::Path{"C:/Filmy"}.parent()), std::string{"C:/"});
    same(kept(tal::Path{"\\\\teka\\Biblioteka\\Filmy"}.parent()), std::string{"//teka/Biblioteka"});
    ought(tal::Path{"//teka/Biblioteka"}.root());
    same(kept(tal::Path{"//teka/Biblioteka"} / "Filmy"), std::string{"//teka/Biblioteka/Filmy"});
    ought((tal::Path{"C:/A"}.same)(tal::Path{"c:/a"}));
}
#endif

TRIAL("Bytes: text and bytes turn into each other unchanged") {
    const std::string given{"zażółć\0\xFF", sizeof("zażółć\0\xFF") - 1};

    same(tal::text(tal::bytes(given)), given);
    same(tal::bytes("ab").size(), std::size_t{2});
}
