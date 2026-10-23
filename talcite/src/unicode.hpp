#pragma once

namespace talcite::unicode {

[[nodiscard]] bool wide(char32_t one);

[[nodiscard]] bool zero(char32_t one);

[[nodiscard]] bool mark(char32_t one);

[[nodiscard]] bool control(char32_t one);

[[nodiscard]] bool leading(char32_t one);

[[nodiscard]] bool vowel(char32_t one);

[[nodiscard]] bool trailing(char32_t one);

[[nodiscard]] bool syllable(char32_t one);

[[nodiscard]] bool open(char32_t one);

[[nodiscard]] bool pictograph(char32_t one);

[[nodiscard]] bool regional(char32_t one);

[[nodiscard]] bool modifier(char32_t one);

[[nodiscard]] bool spacing(char32_t one);

[[nodiscard]] bool prepending(char32_t one);

}
