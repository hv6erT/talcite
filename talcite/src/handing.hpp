#pragma once

#include <memory>
#include <utility>

#include "talcite/promise.hpp"
#include "talcite/detail/result.hpp"

namespace talcite::net {

// A promise that is already running. A plain Promise is lazy: it waits for start() and only then
// puts the work into the pool. A network operation cannot be left unstarted: the socket collects
// data into the kernel buffer anyway, and a hop through the pool would cost more than the
// registration with the dispatcher itself.
//
// It lives in one place, although datagrams, streams and listeners all need it. Repeated in each
// of their files it would be several definitions of the same name in one namespace, which violates
// the one-definition rule without the linker reporting it: the linker picks any one of them and
// moves on. As long as they are identical to the character this works; one diverging change in any of
// those files would break the program at a place that seems unrelated.
template <typename T>
class HandPromise : public Promise<T> {
public:
    explicit HandPromise(std::shared_ptr<detail::Result<T>> made)
        : Promise<T>(typename Promise<T>::Callback{}, std::move(made)) {}
};

}
