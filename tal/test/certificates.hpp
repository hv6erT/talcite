#pragma once

#include <string_view>

namespace tal::testing::certificates {

extern const std::string_view authority_pem;

extern const std::string_view stranger_pem;

extern const std::string_view identity_hex;

extern const std::string_view expired_hex;

extern const std::string_view client_hex;

extern const std::string_view chained_hex;

extern const std::string_view single_hex;

extern const std::string_view foreign_hex;

extern const std::string_view stranger_bare_hex;

extern const std::string_view kept_hex;

}
