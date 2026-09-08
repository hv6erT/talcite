#include "stunning.hpp"

#include <cstring>

#include "talcite/digest.hpp"
#include "talcite/random.hpp"

namespace talcite::net {

namespace {

// Ciasteczko jest stałe i to ono odróżnia STUN od czasów, gdy STUN wyglądał inaczej. Bez niego
// nie da się też odkłamać adresu, bo to nim jest przekłamany.
constexpr std::uint32_t cookie = 0x2112A442u;

std::uint16_t whole(const Bytes& what, std::size_t at) {
    return static_cast<std::uint16_t>((std::to_integer<unsigned>(what[at]) << 8) |
                                      std::to_integer<unsigned>(what[at + 1]));
}

std::uint32_t wider(const Bytes& what, std::size_t at) {
    return (static_cast<std::uint32_t>(whole(what, at)) << 16) | whole(what, at + 2);
}

void pushed(Bytes& into, std::uint16_t what) {
    into.push_back(static_cast<std::byte>(what >> 8));
    into.push_back(static_cast<std::byte>(what & 0xFF));
}

// Pole długości siedzi w trzecim i czwartym bajcie i liczy **wszystko po nagłówku**.
void written(Bytes& into, std::uint16_t much) {
    into[2] = static_cast<std::byte>(much >> 8);
    into[3] = static_cast<std::byte>(much & 0xFF);
}

void minded(Bytes& into) { written(into, static_cast<std::uint16_t>(into.size() - 20)); }

// Adres w STUN ma bajt pusty, bajt rodziny, port i tyle bajtów, ile rodzina każe. W postaci
// przekłamanej port jest odwrócony górną połową ciasteczka, adres całym ciasteczkiem, a przy
// IPv6 jeszcze numerem transakcji — bo szesnaście bajtów potrzebuje szesnastu bajtów maski.
Address placed(const Bytes& what, std::size_t at, std::uint16_t much, const Number& number,
               bool xored) {
    if (much < 4) return {};

    const unsigned family = std::to_integer<unsigned>(what[at + 1]);

    std::uint16_t port = whole(what, at + 2);

    if (xored) port = static_cast<std::uint16_t>(port ^ (cookie >> 16));

    const std::size_t wide = family == 0x01 ? 4u : 16u;

    if (family != 0x01 && family != 0x02) return {};

    if (much < 4 + wide) return {};

    std::array<std::byte, 16> raw{};

    std::byte mask[16]{};

    for (std::size_t one = 0; one < 4; ++one)
        mask[one] = static_cast<std::byte>((cookie >> (24 - 8 * one)) & 0xFF);

    for (std::size_t one = 0; one < 12; ++one) mask[4 + one] = number[one];

    for (std::size_t one = 0; one < wide; ++one)
        raw[one] = xored ? (what[at + 4 + one] ^ mask[one]) : what[at + 4 + one];

    return Address{family == 0x01 ? Family::four : Family::six, raw, port};
}

}

// Numer transakcji jest **jedynym dowodem tożsamości odpowiedzi**, więc musi być nie do zgadnięcia
// przez kogoś z boku. Bierzemy go wprost od systemu. Wcześniej stał tu mt19937 zasiany zegarem
// i było to zapisane jako dług — spłacony.
Number numbered() {
    const Bytes given = random(12);

    Number made{};

    for (std::size_t at = 0; at < made.size(); ++at) made[at] = given[at];

    return made;
}

// Rodzaj wiadomości to sprawa i klasa **przemieszane bitami**: sprawa rozbita na trzy kawałki,
// a klasa wciśnięta w bity piąty i dziewiąty. Tak to wymyślono, żeby stare i nowe wiadomości dało
// się odróżnić po dwóch zerach na początku.
std::uint16_t merged(Task task, Sort sort) {
    const std::uint16_t one = static_cast<std::uint16_t>(task);

    const std::uint16_t kind = static_cast<std::uint16_t>(((one & 0x0F80) << 2) |
                                                          ((one & 0x0070) << 1) | (one & 0x000F));

    switch (sort) {
        case Sort::told: return kind | 0x0100;
        case Sort::amiss: return kind | 0x0110;
        case Sort::telling: return kind | 0x0010;
        default: return kind;
    }
}

// I z powrotem: klasa z dwóch bitów, sprawa ze zlepionych kawałków.
void parted(std::uint16_t kind, Message& made) {
    const std::uint16_t one = static_cast<std::uint16_t>(((kind & 0x3E00) >> 2) |
                                                          ((kind & 0x00E0) >> 1) | (kind & 0x000F));

    made.task = static_cast<Task>(one);

    switch (kind & 0x0110) {
        case 0x0100: made.sort = Sort::told; break;
        case 0x0110: made.sort = Sort::amiss; break;
        case 0x0010: made.sort = Sort::telling; break;
        default: made.sort = Sort::asking; break;
    }
}

Bytes begun(Task task, Sort sort, const Number& number) {
    Bytes made;

    made.reserve(20);

    pushed(made, merged(task, sort));
    pushed(made, 0);
    pushed(made, static_cast<std::uint16_t>(cookie >> 16));
    pushed(made, static_cast<std::uint16_t>(cookie & 0xFFFF));

    for (const std::byte one : number) made.push_back(one);

    return made;
}

// Adres w atrybucie — w postaci przekłamanej albo gołej, zależnie od tego, czego żąda atrybut.
void placing(Bytes& made, std::uint16_t kind, const Address& where, bool xored) {
    const bool four = where.family() == Family::four;

    const std::size_t wide = four ? 4u : 16u;

    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(static_cast<std::byte>(four ? 1 : 2));

    pushed(value, xored ? static_cast<std::uint16_t>(where.port() ^ (cookie >> 16)) : where.port());

    std::byte mask[16]{};

    for (std::size_t one = 0; one < 4; ++one)
        mask[one] = static_cast<std::byte>((cookie >> (24 - 8 * one)) & 0xFF);

    for (std::size_t one = 0; one < 12; ++one) mask[4 + one] = made[8 + one];

    for (std::size_t one = 0; one < wide; ++one)
        value.push_back(xored ? (where.bytes()[one] ^ mask[one]) : where.bytes()[one]);

    pushed(made, kind);
    pushed(made, static_cast<std::uint16_t>(value.size()));

    for (const std::byte one : value) made.push_back(one);

    minded(made);
}

void wording(Bytes& made, std::uint16_t kind, std::uint32_t what) {
    pushed(made, kind);
    pushed(made, 4);

    pushed(made, static_cast<std::uint16_t>(what >> 16));
    pushed(made, static_cast<std::uint16_t>(what & 0xFFFF));

    minded(made);
}

void giving(Bytes& made, const Bytes& what) {
    pushed(made, 0x0013);
    pushed(made, static_cast<std::uint16_t>(what.size()));

    for (const std::byte one : what) made.push_back(one);

    while (made.size() % 4 != 0) made.push_back(std::byte{0});

    minded(made);
}

// Kanał, czyli ta sama treść w czterech bajtach zamiast trzydziestu sześciu: numer kanału zamiast
// nagłówka STUN, ciasteczka, numeru transakcji i atrybutu z adresem rozmówcy. Adres siedzi
// w numerze — serwer wie, komu ten numer przypisał.
//
// Po dwóch najstarszych bitach pierwszego bajtu **poznaje się, co przyszło**: zerowe znaczą STUN,
// `01` znaczy kanał. Numery kanałów zaczynają się dlatego od 0x4000. Dopełnienia do czterech bajtów
// po UDP nie ma — datagram sam wyznacza koniec treści, a tu każdy bajt jest oszczędzany celowo.
Bytes wrapped(std::uint16_t channel, const Bytes& what) {
    Bytes made;

    made.reserve(4 + what.size());

    pushed(made, channel);
    pushed(made, static_cast<std::uint16_t>(what.size()));

    for (const std::byte one : what) made.push_back(one);

    return made;
}

bool unwrapped(const Bytes& what, std::uint16_t& channel, Bytes& data) {
    if (what.size() < 4) return false;

    // Te dwa bity **są** zakresem numerów kanałów: `01` na początku to dokładnie 0x4000 do 0x7FFF.
    // Osobne sprawdzanie granic byłoby martwym kodem.
    if ((std::to_integer<unsigned>(what[0]) & 0xC0) != 0x40) return false;

    const std::uint16_t much = whole(what, 2);

    if (what.size() < 4u + much) return false;

    channel = whole(what, 0);

    data.assign(what.begin() + 4, what.begin() + 4 + much);

    return true;
}

Bytes asked(const Number& number) {
    Bytes made;

    made.reserve(20);

    pushed(made, 0x0001);
    pushed(made, 0);
    pushed(made, static_cast<std::uint16_t>(cookie >> 16));
    pushed(made, static_cast<std::uint16_t>(cookie & 0xFFFF));

    for (const std::byte one : number) made.push_back(one);

    return made;
}

// Odpowiedź na sprawdzenie łączności: ten sam numer transakcji co w pytaniu i jeden atrybut —
// przekłamany adres, z którego pytanie przyszło. Tyle wystarczy, żeby druga strona wiedziała, że
// dziura w NAT-cie stoi, i przy okazji poznała swój adres zewnętrzny.
Bytes told(const Number& number, const Address& seen) {
    const bool four = seen.family() == Family::four;

    const std::size_t wide = four ? 4u : 16u;

    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(static_cast<std::byte>(four ? 1 : 2));

    pushed(value, static_cast<std::uint16_t>(seen.port() ^ (cookie >> 16)));

    std::byte mask[16]{};

    for (std::size_t one = 0; one < 4; ++one)
        mask[one] = static_cast<std::byte>((cookie >> (24 - 8 * one)) & 0xFF);

    for (std::size_t one = 0; one < 12; ++one) mask[4 + one] = number[one];

    for (std::size_t one = 0; one < wide; ++one) value.push_back(seen.bytes()[one] ^ mask[one]);

    Bytes made;

    made.reserve(20 + 4 + value.size());

    pushed(made, 0x0101);
    pushed(made, static_cast<std::uint16_t>(4 + value.size()));
    pushed(made, static_cast<std::uint16_t>(cookie >> 16));
    pushed(made, static_cast<std::uint16_t>(cookie & 0xFFFF));

    for (const std::byte one : number) made.push_back(one);

    pushed(made, 0x0020);
    pushed(made, static_cast<std::uint16_t>(value.size()));

    for (const std::byte one : value) made.push_back(one);

    return made;
}

bool understood(const Bytes& what, Message& made) {
    if (what.size() < 20) return false;

    // Dwa najstarsze bity zerowe — po tym i tylko po tym poznaje się STUN na wspólnym gnieździe.
    if ((std::to_integer<unsigned>(what[0]) & 0xC0) != 0) return false;

    if (wider(what, 4) != cookie) return false;

    const std::uint16_t much = whole(what, 2);

    if (what.size() < 20u + much) return false;

    parted(whole(what, 0), made);

    for (std::size_t at = 0; at < 12; ++at) made.number[at] = what[8 + at];

    made.seen = {};
    made.lent = {};
    made.peer = {};
    made.data.clear();
    made.code = 0;
    made.lifetime = 0;
    made.said.clear();
    made.realm.clear();
    made.nonce.clear();

    bool xored = false;

    std::size_t at = 20;

    while (at + 4 <= 20u + much) {
        const std::uint16_t kind = whole(what, at);
        const std::uint16_t wide = whole(what, at + 2);

        at += 4;

        // Atrybut musi mieścić się w **zadeklarowanej** wiadomości, nie tylko w buforze. Inaczej
        // nadawca deklarował długość 4 i kazał nam czytać dalej, spoza tego, co sam nazwał.
        if (at + wide > 20u + much) break;

        if (at + wide > what.size()) break;

        if (kind == 0x0020) {
            // Popsuty atrybut **nie kasuje dobrego**. Wcześniej przypisanie było bezwarunkowe, więc
            // wystarczył jeden zły bajt rodziny za poprawnym adresem, żeby odpowiedź stała się
            // pusta — i to razem z zamknięciem drogi staremu atrybutowi.
            if (const Address one = placed(what, at, wide, made.number, true); one.held()) {
                made.seen = one;
                xored = true;
            }
        } else if (kind == 0x0001 && !xored) {
            // Stary atrybut bierzemy tylko wtedy, gdy nowego nie było — część serwerów wysyła oba.
            if (const Address one = placed(what, at, wide, made.number, false); one.held())
                made.seen = one;
        } else if (kind == 0x0016) {
            // Adres użyczony przez przekaźnik: to na niego piszą do nas obcy.
            made.lent = placed(what, at, wide, made.number, true);
        } else if (kind == 0x0012) {
            made.peer = placed(what, at, wide, made.number, true);
        } else if (kind == 0x0013) {
            made.data.assign(what.begin() + static_cast<std::ptrdiff_t>(at),
                             what.begin() + static_cast<std::ptrdiff_t>(at + wide));
        } else if (kind == 0x000D && wide >= 4) {
            made.lifetime = static_cast<int>(wider(what, at));
        } else if (kind == 0x0014 || kind == 0x0015) {
            std::string said;

            for (std::size_t one = 0; one < wide && said.size() < 768; ++one) {
                const unsigned sign = std::to_integer<unsigned>(what[at + one]);

                said += sign < 0x20 || sign == 0x7F ? '?' : static_cast<char>(sign);
            }

            // Dziedzina i jednorazowy znacznik przychodzą od serwera i wracają do niego bez zmian
            // przy uwierzytelnianiu długoterminowym. Też są tekstem od obcego, więc też bez
            // znaków sterujących.
            (kind == 0x0014 ? made.realm : made.nonce) = said;
        } else if (kind == 0x0009 && wide >= 4) {
            const unsigned sort = std::to_integer<unsigned>(what[at + 2]) & 0x07;

            made.code = static_cast<int>(sort * 100 + std::to_integer<unsigned>(what[at + 3]) % 100);

            // Powód pisze obcy i **nie ma prawa nieść znaków sterujących**: to zdanie trafia
            // wprost do komunikatu, a ten na terminal. Sekwencje ucieczki z cudzej sieci
            // przemalowałyby ekran albo podmieniły tytuł okna. Bierzemy też skończenie wiele.
            for (std::size_t one = 4; one < wide && made.said.size() < 128; ++one) {
                const unsigned sign = std::to_integer<unsigned>(what[at + one]);

                made.said += sign < 0x20 || sign == 0x7F ? '?' : static_cast<char>(sign);
            }
        }

        at += wide;
        at += (4 - wide % 4) % 4;
    }

    return true;
}


// CRC-32 z wielomianem IEEE. **To nie jest kryptografia** — to suma kontrolna, taka sama jak
// w archiwach, i piszemy ją sami, bo żaden system jej nie wystawia, a tu jest potrzebna tylko po
// to, żeby odróżnić wiadomość STUN od czegokolwiek innego na tym samym gnieździe.
std::uint32_t tallied(const Bytes& what, std::size_t much) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> made{};

        for (std::uint32_t at = 0; at < 256; ++at) {
            std::uint32_t one = at;

            for (int turn = 0; turn < 8; ++turn)
                one = (one & 1) != 0 ? 0xEDB88320u ^ (one >> 1) : one >> 1;

            made[at] = one;
        }

        return made;
    }();

    std::uint32_t made = 0xFFFFFFFFu;

    for (std::size_t at = 0; at < much && at < what.size(); ++at)
        made = table[(made ^ std::to_integer<unsigned>(what[at])) & 0xFF] ^ (made >> 8);

    return made ^ 0xFFFFFFFFu;
}

void texting(Bytes& made, std::uint16_t kind, const std::string& said) {
    pushed(made, kind);
    pushed(made, static_cast<std::uint16_t>(said.size()));

    for (const char one : said) made.push_back(static_cast<std::byte>(one));

    while (made.size() % 4 != 0) made.push_back(std::byte{0});

    minded(made);
}

// **Podpis liczy się po treści, w której pole długości już udaje, że atrybut podpisu tam jest.**
// To jest miejsce, w którym implementacje mylą się najczęściej: gdyby liczyć po długości sprzed
// dopisania, druga strona policzyłaby co innego i każda wiadomość byłaby odrzucona.
void stamping(Bytes& made, const Bytes& key) {
    written(made, static_cast<std::uint16_t>(made.size() - 20 + 24));

    const Bytes sign = stamped(Sum::sha1, key, made);

    pushed(made, 0x0008);
    pushed(made, 20);

    for (const std::byte one : sign) made.push_back(one);
}

// Ta sama sztuczka z długością, tylko że odciskiem jest suma kontrolna, a nie podpis. Odcisk idzie
// **na sam koniec**, po podpisie, bo obejmuje także jego.
//
// Nazywało się to `marking` — słowem, które w tej bibliotece znaczy literę aktywną. `summing` też
// odpadło, bo `talcite::summing()` z `digest.hpp` znaczy „czy ten system daje skróty". Zostaje odcisk.
void printing(Bytes& made) {
    written(made, static_cast<std::uint16_t>(made.size() - 20 + 8));

    const std::uint32_t sum = tallied(made, made.size()) ^ 0x5354554Eu;

    pushed(made, 0x8028);
    pushed(made, 4);

    pushed(made, static_cast<std::uint16_t>(sum >> 16));
    pushed(made, static_cast<std::uint16_t>(sum & 0xFFFF));
}

// Sprawdzenie podpisu wymaga odtworzenia dokładnie tej treści, po której liczyła go druga strona:
// wiadomości uciętej przed atrybutem podpisu i z długością udającą, że podpis już tam stoi.
bool trusted(const Bytes& what, const Bytes& key) {
    if (what.size() < 20) return false;

    const std::uint16_t much = whole(what, 2);

    if (what.size() < 20u + much) return false;

    std::size_t at = 20;

    while (at + 4 <= 20u + much) {
        const std::uint16_t kind = whole(what, at);
        const std::uint16_t wide = whole(what, at + 2);

        if (kind == 0x0008) {
            if (wide != 20 || at + 4 + 20 > what.size()) return false;

            Bytes over{what.begin(), what.begin() + static_cast<std::ptrdiff_t>(at)};

            written(over, static_cast<std::uint16_t>(at - 20 + 24));

            const Bytes sign = stamped(Sum::sha1, key, over);

            if (sign.size() != 20) return false;

            // Porównanie **do końca, bez skracania na pierwszej różnicy**. Wychodzenie wcześniej
            // mówiłoby czasem odpytującemu, ile bajtów zgadło.
            unsigned apart = 0;

            for (std::size_t one = 0; one < 20; ++one)
                apart |= std::to_integer<unsigned>(sign[one] ^ what[at + 4 + one]);

            return apart == 0;
        }

        at += 4;
        at += wide;
        at += (4 - wide % 4) % 4;
    }

    return false;
}

// Klucz uwierzytelniania długoterminowego jest skrótem z nazwy, dziedziny i hasła sklejonych
// dwukropkami. Tak każe RFC i tak liczą go serwery TURN.
Bytes keyed(const std::string& user, const std::string& realm, const std::string& word) {
    const std::string said = user + ":" + realm + ":" + word;

    Bytes what(said.size());

    for (std::size_t at = 0; at < said.size(); ++at) what[at] = static_cast<std::byte>(said[at]);

    return summed(Sum::md5, what);
}

}
