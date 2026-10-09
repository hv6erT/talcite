#pragma once

#include <memory>
#include <optional>

#include "talcite/address.hpp"
#include "talcite/patience.hpp"
#include "talcite/promise.hpp"
#include "talcite/stream.hpp"

#include "inside.hpp"

// **Rura nazwana w miejscu gniazda lokalnego.** Windows ma AF_UNIX dopiero od 10 1803; wcześniejszy
// (7, 8, 8.1, pierwsze wydania 10) i Wine — nie. Adres lokalny zostaje ścieżką, brama, strumień
// i centrala zostają te same, a pod spodem jest rura `\\.\pipe\talcite-…` z nazwą liczoną ze
// ścieżki. Wybór zapada raz, w bibliotece: wołający o nim nie wie.
//
// Uchwyty rur są zwykłymi uchwytami centrali (`Handle`); rejestr mówi, który uchwyt jest rurą i po
// której stronie, bo zamknięcie, nazwa i rozmówca idą dla rury inaczej niż dla gniazda.
namespace talcite::net::pipes {

// Prawda tam, gdzie adres lokalny idzie rurą: Windows bez AF_UNIX albo budowa z `TALCITE_PIPES`.
[[nodiscard]] bool chosen();

[[nodiscard]] bool piped(Handle one);

// Koniec serwera — ten, który przyszedł z bramy.
[[nodiscard]] bool served(Handle one);

[[nodiscard]] Address named(Handle one, bool peer);

// Numer procesu po drugiej stronie albo nic.
[[nodiscard]] std::optional<unsigned long> facing(Handle one);

void closed(Handle one);

// Brama rury: nazwa zajęta pierwszą instancją, a potem zawsze dokładnie jedna słuchająca — wolna
// albo ta, na której czeka przyjęcie — żeby nazwa nie znikała między przyjęciami.
struct Gate;

[[nodiscard]] std::shared_ptr<Gate> gated(const Address& where);

// Instancja do czekania na klienta (wolna albo nowa). Po zamknięciu — nic.
[[nodiscard]] Handle spare(Gate& gate);

// Przyjęcie na `done` się skończyło (czymkolwiek): brama znowu ma wolną instancję.
void refill(Gate& gate, Handle done);

void shut(Gate& gate);

[[nodiscard]] Promise<Stream> dialed(Address where, Patience patience);

}
