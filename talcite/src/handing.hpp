#pragma once

#include <memory>
#include <optional>
#include <utility>

#include "talcite/promise.hpp"

#include "inside.hpp"

namespace talcite::net {

// A network request that is at the same time the producer of its promise: the hub completes it
// (`done`, `late`), and it settles the promise. When the promise's owners leave, `dropped` runs at
// once, inside the cancelling call: the request has to leave the hub and free its direction before
// the caller asks for the next one. It is library code only and takes nothing but the library's own
// locks.
//
// It lives in one place, although datagrams, streams and listeners all need it. Repeated in each of
// their files it would be several definitions of the same name in one namespace, which violates the
// one-definition rule without the linker reporting it: the linker picks any one of them and moves
// on. As long as they are identical to the character this works; one diverging change in any of
// those files would break the program at a place that seems unrelated.
template <typename T>
struct Handing : Doing {
    std::optional<Settle<T>> settle;

    // Takes the request back when the promise is given up; nothing by default.
    virtual void dropped() {}
};

// The promise of a request: made here, settled by the request. The request is not started here; the
// caller submits it after this, so that a request cancelled at once is still withdrawn properly.
template <typename T, typename R>
[[nodiscard]] Promise<T> handed(const std::shared_ptr<R>& one) {
    auto [made, settle] = promised<T>();

    one->settle.emplace(std::move(settle));

    (void)detail::stopping(one->settle->stop(), [weak = std::weak_ptr<R>{one}] {
        if (const std::shared_ptr<R> mine = weak.lock()) mine->dropped();
    });

    return std::move(made);
}

// A promise refused at once with `Trouble` carrying the message, code and address.
template <typename T>
[[nodiscard]] Promise<T> refused(const std::string& said, int code = 0, Address where = {}) {
    return failed<T>(std::make_exception_ptr(Trouble{said, code, std::move(where)}));
}

}
