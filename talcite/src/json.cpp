#include "talcite/json.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace talcite::json {

// Wartość jest danymi, jak drzewo w `xml.cpp`: rekurencyjny wariant bez parsowania, które należy
// do przejściówki. Sześć typów ze specyfikacji, z jedną różnicą — liczba jest rozdzielona na
// całkowitą i rzeczywistą. JSON ma jeden typ „number", ale plik ustawień z `"columns": 2` musi
// wrócić na dysk jako `2`, nie `2.0`, a int64 ponad 2^53 nie mieści się w double bez straty.
// Tylko przejściówka widzi, czy w tekście była kropka albo wykładnik, więc tylko ona może to
// rozstrzygnąć; wartość ma to jedynie przechować.
//
// `Array` to wektor typu w chwili deklaracji niekompletnego — to jest legalne od C++17, a wariant
// w środku klasy potrzebuje jedynie kompletności samego wektora, nie jego elementu. `Member` jest
// osobną strukturą, a nie `std::pair`, bo para z niekompletnym `Value` w wektorze wewnątrz `Value`
// dawała się tak zbudować tylko za łaską implementacji biblioteki standardowej.

Value::Value(std::nullptr_t) : held_(nullptr) {}

Value::Value(bool given) : held_(given) {}

Value::Value(std::string given) : held_(std::move(given)) {}

// Osobno od napisu, bo bez tego `Value{"x"}` poszłoby konstruktorem od bool: wskaźnik → bool
// to konwersja standardowa, wskaźnik → std::string to konwersja użytkownika, i pierwsza wygrywa.
Value::Value(const char* given) : held_(std::string{given}) {}

Value::Value(Array given) : held_(std::move(given)) {}

Value::Value(Object given) : held_(std::move(given)) {}

// Numer alternatywy w wariancie i numer w `Type` są tym samym porządkiem — celowo, żeby
// rozróżnialnik był jednym rzutem, a nie drabinką `holds_alternative`.
Type Value::type() const { return static_cast<Type>(held_.index()); }

// Dostęp jak w xml: wskaźnik, `nullptr` znaczy „to nie ten typ". Bez wyjątków, bo wartość nie ma
// czym zawieść; czy brak liczby tam, gdzie jej oczekiwano, jest błędem, rozstrzyga wołający.
const bool* Value::boolean() const { return std::get_if<bool>(&held_); }

const std::int64_t* Value::integer() const { return std::get_if<std::int64_t>(&held_); }

const double* Value::real() const { return std::get_if<double>(&held_); }

const std::string* Value::string() const { return std::get_if<std::string>(&held_); }

const Array* Value::array() const { return std::get_if<Array>(&held_); }

const Object* Value::object() const { return std::get_if<Object>(&held_); }

std::string* Value::string() { return std::get_if<std::string>(&held_); }

Array* Value::array() { return std::get_if<Array>(&held_); }

Object* Value::object() { return std::get_if<Object>(&held_); }

// Liczba niezależnie od zapisu — dla wołających, którym zapis jest obojętny. Dla całkowitej to
// konwersja (z możliwą stratą ponad 2^53, o której wołający wie, skoro pyta o double).
std::optional<double> Value::number() const {
    if (const double* one = real(); one != nullptr) return *one;

    if (const std::int64_t* one = integer(); one != nullptr) return static_cast<double>(*one);

    return std::nullopt;
}

// Klucze w obiekcie są jednoznaczne — pilnuje tego przejściówka przy dekodowaniu i `insert` przy
// budowaniu — więc pierwsze trafienie jest jedynym.
Value* member(Value& one, std::string_view name) {
    Object* object = one.object();

    if (object == nullptr) return nullptr;

    for (Member& each : *object)
        if (each.name == name) return &each.value;

    return nullptr;
}

const Value* member(const Value& one, std::string_view name) {
    return member(const_cast<Value&>(one), name);
}

// Wstawienie albo dostęp: istniejący wpis wraca, brakujący staje na końcu jako null. Na innym
// typie rzuca, bo to pomyłka programisty, nie danych — zamiana wartości na obiekt jest decyzją
// wołającego (Store w twa robi ją jawnie, gdy nowy kształt tego chce).
Value& insert(Value& one, std::string name) {
    Object* object = one.object();

    if (object == nullptr) throw std::logic_error{"Not an object."};

    for (Member& each : *object)
        if (each.name == name) return each.value;

    object->push_back(Member{std::move(name), Value{}});

    return object->back().value;
}

bool erase(Value& one, std::string_view name) {
    Object* object = one.object();

    if (object == nullptr) return false;

    const auto found = std::find_if(object->begin(), object->end(),
                                    [name](const Member& each) { return each.name == name; });

    if (found == object->end()) return false;

    object->erase(found);

    return true;
}

}
