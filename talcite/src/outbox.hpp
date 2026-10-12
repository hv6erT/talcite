#pragma once

#include <functional>
#include <memory>

#include "talcite/bytes.hpp"
#include "talcite/patience.hpp"
#include "talcite/promise.hpp"
#include "talcite/stream.hpp"

namespace talcite::net {

class Outbox {
public:
    using Source = std::function<Stream()>;

    Outbox();

    [[nodiscard]] Promise<void> queued(Bytes what, Patience patience, Source source);

    void flush();

private:
    struct State;

    static void served(const std::shared_ptr<State>& state);

    std::shared_ptr<State> state_;
};

}
