#pragma once

#include <memory>
#include <optional>

#include "talcite/address.hpp"
#include "talcite/patience.hpp"
#include "talcite/promise.hpp"
#include "talcite/stream.hpp"

#include "inside.hpp"

// A named pipe in place of a local socket. Windows has AF_UNIX only since 10 1803; earlier
// versions (7, 8, 8.1, the first releases of 10) and Wine do not. A local address stays a path,
// and the listener, the stream and the hub stay the same, but underneath is a pipe
// \\.\pipe\talcite-... with a name computed from the path. The choice is made once, in the
// library: the caller does not know about it.
//
// Pipe handles are ordinary hub handles (`Handle`); a registry says which handle is a pipe and on
// which side, because closing, naming and the peer work differently for a pipe than for a socket.
namespace talcite::net::pipes {

// True where a local address goes through a pipe: Windows without AF_UNIX, or a build with
// `TALCITE_PIPES`.
[[nodiscard]] bool chosen();

[[nodiscard]] bool piped(Handle one);

// The server end: the one that came from a gate.
[[nodiscard]] bool served(Handle one);

[[nodiscard]] Address named(Handle one, bool peer);

// The process ID on the other side, or nothing.
[[nodiscard]] std::optional<unsigned long> facing(Handle one);

void closed(Handle one);

// A pipe gate: the name is taken by the first instance, and after that there is always exactly
// one listening instance (a free one or the one an accept is waiting on), so that the name does
// not vanish between accepts.
struct Gate;

[[nodiscard]] std::shared_ptr<Gate> gated(const Address& where);

// An instance to wait for a client on (the free one or a new one). After shutting, none.
[[nodiscard]] Handle spare(Gate& gate);

// The accept on `done` has finished (with any outcome): the gate has a free instance again.
void refill(Gate& gate, Handle done);

void shut(Gate& gate);

[[nodiscard]] Promise<Stream> dialed(Address where, Patience patience);

}
