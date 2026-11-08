#pragma once

#include <cstddef>
#include <memory>
#include <utility>

#include "tal/bytes.hpp"
#include "tal/task.hpp"

namespace tal {

class Channel {
public:
    Channel() = default;
    virtual ~Channel() = default;

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    [[nodiscard]] virtual Task<Bytes> read(std::size_t most) = 0;

    [[nodiscard]] virtual Task<void> write(Bytes what) = 0;

    virtual void close() = 0;
};

class Stream {
public:
    Stream() = default;

    explicit Stream(std::shared_ptr<Channel> held);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] Task<Bytes> read(std::size_t most) const;

    [[nodiscard]] Task<void> write(Bytes what) const;

    void close() const;

private:
    std::shared_ptr<Channel> held_;
};

[[nodiscard]] Task<Bytes> drained(Stream from);

namespace detail {

[[nodiscard]] std::pair<Stream, Stream> piped();

}

}
