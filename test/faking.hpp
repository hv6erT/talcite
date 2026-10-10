#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/clipboard.hpp"
#include "talcite/console.hpp"
#include "talcite/picture.hpp"
#include "talcite/tls.hpp"

namespace faking {

// Atrapa dostawcy obrazów: **nie dekoduje GIF-a** i nie udaje, że to robi. Z pliku GIF bierze
// tylko to, co leży w nagłówku bez rozpakowywania — rozmiar ekranu z bajtów 6–9 i opóźnienia
// z bloków sterujących `21 f9 04` — i oddaje tyle klatek, ile bloków, każdą jako jednolite
// płótno o innym odcieniu. Opóźnienie do jednej setnej podnosi do stu milisekund, tak jak
// prawdziwy dekoder. Wszystko inne odmawia zdaniem, a rozmiar zero odmawia zdaniem osobnym —
// żeby próby widżetu i szwu sprawdzały mechanikę, a nie LZW, który mieszka w talcite-adapters.
class Images : public talcite::ImageProvider {
public:
    [[nodiscard]] std::string_view name() const override { return "fake images"; }

    [[nodiscard]] std::string decode(const talcite::Bytes& given, const talcite::Taking& each) override {
        if (given.size() < 13 || std::to_integer<char>(given[0]) != 'G' ||
            std::to_integer<char>(given[1]) != 'I' || std::to_integer<char>(given[2]) != 'F')
            return "not a GIF the fake would read";

        const int wide = std::to_integer<int>(given[6]) | (std::to_integer<int>(given[7]) << 8);
        const int high = std::to_integer<int>(given[8]) | (std::to_integer<int>(given[9]) << 8);

        if (wide == 0 || high == 0) return "empty screen";

        std::vector<std::chrono::milliseconds> delays;

        for (std::size_t at = 0; at + 5 < given.size(); ++at)
            if (std::to_integer<std::uint8_t>(given[at]) == 0x21 &&
                std::to_integer<std::uint8_t>(given[at + 1]) == 0xF9 &&
                std::to_integer<std::uint8_t>(given[at + 2]) == 0x04) {
                const int hundredths = std::to_integer<int>(given[at + 4]) |
                                       (std::to_integer<int>(given[at + 5]) << 8);

                delays.emplace_back(hundredths <= 1 ? 100 : hundredths * 10);
            }

        if (delays.empty()) delays.emplace_back(0);

        const std::size_t much = static_cast<std::size_t>(wide) * static_cast<std::size_t>(high) * 4;

        // Płótno ponad sufit atrapa **deklaruje, ale nie przydziela**: oddaje pusty bufor z dużym
        // rozmiarem, żeby próba sprawdziła, że szew odetnie takiego dostawcę, zanim ktokolwiek
        // zajrzy do bajtów. Prawdziwy dostawca odmawia sam, z nagłówka, zanim cokolwiek rozpakuje.
        if (much / 4 > talcite::ceiling) {
            (void)each(talcite::Bytes{}, {wide, high}, delays.front());

            return {};
        }

        for (std::size_t which = 0; which < delays.size(); ++which) {
            talcite::Bytes canvas(much, std::byte{static_cast<std::uint8_t>(60 + which * 40)});

            for (std::size_t at = 3; at < much; at += 4) canvas[at] = std::byte{255};

            if (!each(canvas, {wide, high}, delays[which])) return {};
        }

        return {};
    }
};

[[nodiscard]] inline talcite::Seam<talcite::ImageProvider> images() {
    return talcite::Seam<talcite::ImageProvider>{std::make_shared<Images>()};
}

// Atrapa terminala. **Środowisko ma własne**, a nie procesu, i to od razu takie, w jakim stoi
// typowy terminal — próba ustawia tylko to, o co jej chodzi, a wynik nie zależy od tego, z czego
// uruchomiono przebieg. Odpowiedzi czekają w kolejce `replies`, po jednej na słuchanie; podane
// przy budowie idą w całości albo, gdy podano `piece`, po tyle bajtów. Wszystko, co do niej
// napisano, zostaje w `written`, a każde słuchanie zapisuje swoją cierpliwość i to, czy
// przyniesiono mu dzwonek.
class Terminal : public talcite::Console {
public:
    explicit Terminal(std::string said = {}, std::size_t piece = 0) {
        if (said.empty()) return;

        if (piece == 0) {
            replies.push_back(std::move(said));

            return;
        }

        for (std::size_t at = 0; at < said.size(); at += piece) replies.push_back(said.substr(at, piece));
    }

    std::map<std::string, std::string, std::less<>> environment{{"TERM", "xterm-256color"},
                                                                {"LANG", "C.UTF-8"}};

    bool alive = true;
    bool sequences = false;
    std::optional<talcite::ui::Size> window;
    std::chrono::milliseconds twice{500};

    std::deque<std::string> replies;

    std::string written;
    int rounds = 0;
    int depth = 0;
    int entered = 0;
    int belled = 0;
    std::vector<std::optional<std::chrono::milliseconds>> patience;

    [[nodiscard]] std::string_view name() const override { return "fake terminal"; }

    [[nodiscard]] std::optional<std::string> variable(std::string_view name) const override {
        const auto found = environment.find(name);

        if (found == environment.end()) return std::nullopt;

        return found->second;
    }

    [[nodiscard]] bool lively() const override { return alive; }

    [[nodiscard]] bool vt() const override { return sequences; }

    [[nodiscard]] std::optional<talcite::ui::Size> size() const override { return window; }

    [[nodiscard]] std::chrono::milliseconds double_click() const override { return twice; }

    void raw(bool on) override {
        if (on) {
            ++depth;
            ++entered;
        } else if (depth > 0) {
            --depth;
        }
    }

    void write(std::string_view bytes) override { written += bytes; }

    // Wznowienie po zatrzymaniu, jednorazowe — tak jak w prawdziwej konsoli pytanie zdejmuje znak.
    bool comeback = false;

    [[nodiscard]] bool resumed() override {
        const bool made = comeback;

        comeback = false;

        return made;
    }

    [[nodiscard]] std::string hear(std::optional<std::chrono::milliseconds> waited,
                                   const talcite::Bell* also) override {
        ++rounds;

        patience.push_back(waited);

        if (also != nullptr) ++belled;

        if (replies.empty()) return {};

        std::string made = std::move(replies.front());

        replies.pop_front();

        return made;
    }
};

[[nodiscard]] inline std::shared_ptr<Terminal> terminal(std::string said = {}, std::size_t piece = 0) {
    return std::make_shared<Terminal>(std::move(said), piece);
}

// Atrapa schowka: jedna zmienna w pamięci. `refusing` udaje schowek, który nie przyjął kopii —
// wtedy nic się nie zmienia, tak jak przy prawdziwej odmowie.
class Clipboard : public talcite::Clipboard {
public:
    std::optional<std::string> kept;
    bool refusing = false;
    int copies = 0;

    [[nodiscard]] std::string_view name() const override { return "fake clipboard"; }

    [[nodiscard]] bool copy(std::string_view what) override {
        ++copies;

        if (refusing) return false;

        kept = std::string{what};

        return true;
    }

    [[nodiscard]] std::optional<std::string> paste() override { return kept; }
};

[[nodiscard]] inline std::shared_ptr<Clipboard> clipboard() { return std::make_shared<Clipboard>(); }

// **To nie jest kryptografia i nie udaje nią być.** To atrapa: odwraca bity jednym bajtem, żeby
// dało się zobaczyć, że treść naprawdę przechodzi przez szew, i wymienia powitanie, żeby pompa
// miała uścisk dłoni do przeprowadzenia. Cały sens: sprawdzić `TlsStream` — i to, co na nim stoi, jak
// klient HTTP — bez żadnej biblioteki. `refused` każe klientowi odrzucić uścisk z podanym powodem,
// tak jak przejściówka, która go rozpoznała.
class Turning : public talcite::net::TlsSession {
public:
    explicit Turning(bool poison = false, std::optional<talcite::net::TlsTrouble::Why> refused = std::nullopt)
        : poison_(poison), refused_(refused) {}

    [[nodiscard]] bool ready() const override { return said_ && heard_; }

    void push(talcite::Bytes given) override {
        came_.insert(came_.end(), given.begin(), given.end());

        for (;;) {
            if (came_.empty()) return;

            const std::size_t much = static_cast<std::size_t>(came_.front());

            if (came_.size() < much + 1) return;

            talcite::Bytes one(came_.begin() + 1, came_.begin() + 1 + static_cast<std::ptrdiff_t>(much));

            came_.erase(came_.begin(), came_.begin() + 1 + static_cast<std::ptrdiff_t>(much));

            for (std::byte& what : one) what ^= std::byte{0x5A};

            if (!heard_) {
                if (talcite::text(one) != "CZESC") {
                    amiss_ = "Powitanie nie tak brzmi.";

                    return;
                }

                heard_ = true;

                continue;
            }

            if (talcite::text(one) == "TRUCIZNA") {
                amiss_ = "Trucizna w rekordzie.";

                return;
            }

            plain_.insert(plain_.end(), one.begin(), one.end());
        }
    }

    // Pierwsze pytanie o szyfrogram **napędza** uścisk dłoni — bez tego nikt by go nie zaczął.
    [[nodiscard]] talcite::Bytes pull() override {
        if (refused_.has_value()) {
            amiss_ = "Handshake rejected.";

            return {};
        }

        if (!said_) {
            said_ = true;

            framed(talcite::bytes("CZESC"));
        }

        talcite::Bytes made = std::move(out_);

        out_.clear();

        return made;
    }

    void write(talcite::Bytes given) override {
        if (poison_) given = talcite::bytes("TRUCIZNA");

        for (std::size_t at = 0; at < given.size(); at += 255) {
            const std::size_t much = std::min<std::size_t>(255, given.size() - at);

            framed(talcite::Bytes{given.begin() + static_cast<std::ptrdiff_t>(at),
                         given.begin() + static_cast<std::ptrdiff_t>(at + much)});
        }
    }

    [[nodiscard]] talcite::Bytes read() override {
        talcite::Bytes made = std::move(plain_);

        plain_.clear();

        return made;
    }

    void close() override {}

    [[nodiscard]] std::string error() const override { return amiss_; }

    [[nodiscard]] talcite::net::TlsTrouble::Why why() const override {
        return refused_.value_or(talcite::net::TlsTrouble::Why::other);
    }

private:
    void framed(talcite::Bytes what) {
        out_.push_back(static_cast<std::byte>(what.size()));

        for (std::byte one : what) out_.push_back(one ^ std::byte{0x5A});
    }

    talcite::Bytes came_;
    talcite::Bytes plain_;
    talcite::Bytes out_;
    std::string amiss_;
    bool said_ = false;
    bool heard_ = false;
    bool poison_ = false;
    std::optional<talcite::net::TlsTrouble::Why> refused_;
};

class Watching : public talcite::net::TlsProvider {
public:
    explicit Watching(bool poison = false, std::optional<talcite::net::TlsTrouble::Why> refused = std::nullopt)
        : poison_(poison), refused_(refused) {}

    [[nodiscard]] std::string_view name() const override { return "flipping"; }

    [[nodiscard]] std::unique_ptr<talcite::net::TlsSession> client(std::string) override {
        return std::make_unique<Turning>(poison_, refused_);
    }

    [[nodiscard]] std::unique_ptr<talcite::net::TlsSession> server() override {
        return std::make_unique<Turning>();
    }

private:
    bool poison_ = false;
    std::optional<talcite::net::TlsTrouble::Why> refused_;
};

}
