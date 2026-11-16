#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/http.hpp"
#include "tal/stream.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

#include "httping.hpp"

namespace {

// A connection whose other side answers with the bytes given, in pieces as long as the first byte
// says, and then ends; what is written to it goes nowhere.
class Answering final : public tal::Channel {
public:
    // The answer `given`, in pieces of `piece` bytes.
    Answering(std::span<const std::byte> given, std::size_t piece) : given_(given), piece_(piece) {}

    // The next piece, or nothing at the end.
    tal::Task<tal::Bytes> read(std::size_t most) override {
        const std::size_t much = std::min({most, piece_, given_.size()});
        tal::Bytes made(given_.begin(), given_.begin() + static_cast<std::ptrdiff_t>(much));

        given_ = given_.subspan(much);

        co_return made;
    }

    // Takes all.
    tal::Task<void> write(tal::Bytes what) override {
        (void)what;

        co_return;
    }

    // Nothing to close.
    void close() override {}

private:
    std::span<const std::byte> given_;
    std::size_t piece_;
};

}

// One answer of the fuzzer's bytes to a GET (or a HEAD, by the second byte), read as the client
// reads it within small limits: it gives a response or fails as `Trouble`, and never anything
// else.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) return 0;

    const std::span<const std::byte> all{reinterpret_cast<const std::byte*>(data), size};
    tal::Loop loop;
    tal::net::Request request;
    tal::net::Limits most;

    request.url = *tal::net::Url::parsed("http://fuzz.test/");
    request.method = (data[1] & 1) != 0 ? "HEAD" : "GET";
    most.head = 4096;
    most.body = 65536;
    most.quiet = std::chrono::seconds{5};

    try {
        (void)tal::wait(loop, tal::net::httping::answered(tal::Stream{std::make_shared<Answering>(all.subspan(2), std::size_t{data[0]} + 1)}, request, most));
    } catch (const tal::Trouble&) {
    }

    return 0;
}
