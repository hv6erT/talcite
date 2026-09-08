#pragma once

#include <memory>
#include <utility>

#include "talcite/promise.hpp"
#include "talcite/result.hpp"

namespace talcite::net {

// Obietnica, która już biegnie. Zwykła Promise jest leniwa — czeka na go() i dopiero wtedy wrzuca
// robotę do puli. Operacji sieciowej nie da się nie zacząć: gniazdo i tak zbiera dane do bufora
// jądra, a przeskok przez pulę kosztowałby więcej niż samo zgłoszenie do centrali.
//
// Stoi **w jednym miejscu**, choć potrzebują jej poczta, strumień i brama. Powtórzona w trzech
// plikach była trzema definicjami tej samej nazwy w jednej przestrzeni — a to jest naruszenie
// zasady jednej definicji, którego konsolidator nie zgłasza: wybiera dowolną z nich i biegnie
// dalej. Dopóki były co do znaku takie same, działało; jedna rozbieżna zmiana w którymkolwiek
// z tych plików wywróciłaby program w miejscu, które z niczym się nie kojarzy. Dokładnie to
// zrobiła para `Living` w rozmówcy i przekaźniku, i kosztowało pół dnia szukania.
template <typename T>
class HandPromise : public Promise<T> {
public:
    explicit HandPromise(std::shared_ptr<Result<T>> made)
        : Promise<T>(typename Promise<T>::Callback{}, std::move(made)) {}
};

}
