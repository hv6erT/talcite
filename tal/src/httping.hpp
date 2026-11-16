#pragma once

#include <optional>
#include <string_view>

#include "tal/http.hpp"
#include "tal/stream.hpp"
#include "tal/task.hpp"

namespace tal::net::httping {

[[nodiscard]] Task<Response> answered(Stream line, Request request, Limits most);

[[nodiscard]] Proxy windowsy(std::string_view proxy, std::string_view bypass);

[[nodiscard]] std::optional<Proxy> systemic();

}
