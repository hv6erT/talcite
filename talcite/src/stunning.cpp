#include "stunning.hpp"

#include <cstring>

#include "talcite/digest.hpp"
#include "talcite/random.hpp"

// Building and reading STUN messages (RFC 5389), with the TURN attributes and ChannelData framing.
//
// A `Number` is a 12-byte transaction ID. `Sort` is the message class (request, success response,
// error response, indication). `Task` is the method, what the message is meant to do. The message
// type is made of the method and the class, and the two are interleaved bit by bit inside the
// number, hence a separate enumeration instead of bare numbers. `Message` is everything we read
// from a message: the class, the method, the transaction ID, the reflexive address (`seen`), the
// relayed address (`lent`), the peer address, the data, the error code and reason (`said`), the
// lifetime, the realm and the nonce.
namespace talcite::net {

namespace {

// The magic cookie is fixed, and it is what tells STUN apart from the older form of STUN. It is
// also the mask of the XOR-ed address, so without it the address cannot be decoded.
constexpr std::uint32_t cookie = 0x2112A442u;

// The big-endian 16-bit number at `at`.
std::uint16_t whole(const Bytes& what, std::size_t at) {
    return static_cast<std::uint16_t>((std::to_integer<unsigned>(what[at]) << 8) |
                                      std::to_integer<unsigned>(what[at + 1]));
}

// The big-endian 32-bit number at `at`.
std::uint32_t wider(const Bytes& what, std::size_t at) {
    return (static_cast<std::uint32_t>(whole(what, at)) << 16) | whole(what, at + 2);
}

// Appends a big-endian 16-bit number.
void pushed(Bytes& into, std::uint16_t what) {
    into.push_back(static_cast<std::byte>(what >> 8));
    into.push_back(static_cast<std::byte>(what & 0xFF));
}

// Writes the length field, which sits in the third and fourth bytes and counts everything after the
// 20-byte header.
void written(Bytes& into, std::uint16_t much) {
    into[2] = static_cast<std::byte>(much >> 8);
    into[3] = static_cast<std::byte>(much & 0xFF);
}

// Updates the length field to the message as built so far.
void minded(Bytes& into) { written(into, static_cast<std::uint16_t>(into.size() - 20)); }

// Reads an address attribute of `much` bytes at `at`; an empty address when it is malformed. An
// address in STUN has an empty byte, a family byte, the port, and as many bytes as the family
// requires. In the XOR-ed form the port is XOR-ed with the upper half of the cookie, the address
// with the whole cookie and, for IPv6, also with the transaction ID, because sixteen bytes need
// sixteen bytes of mask.
Address placed(const Bytes& what, std::size_t at, std::uint16_t much, const Number& number,
               bool xored) {
    if (much < 4) return {};

    const unsigned family = std::to_integer<unsigned>(what[at + 1]);

    std::uint16_t port = whole(what, at + 2);

    if (xored) port = static_cast<std::uint16_t>(port ^ (cookie >> 16));

    const std::size_t wide = family == 0x01 ? 4u : 16u;

    if (family != 0x01 && family != 0x02) return {};

    if (much < 4 + wide) return {};

    std::array<std::byte, 16> raw{};

    std::byte mask[16]{};

    for (std::size_t one = 0; one < 4; ++one)
        mask[one] = static_cast<std::byte>((cookie >> (24 - 8 * one)) & 0xFF);

    for (std::size_t one = 0; one < 12; ++one) mask[4 + one] = number[one];

    for (std::size_t one = 0; one < wide; ++one)
        raw[one] = xored ? (what[at + 4 + one] ^ mask[one]) : what[at + 4 + one];

    return Address{family == 0x01 ? Family::four : Family::six, raw, port};
}

}

// A new transaction ID. It is the only proof of a response's identity, so it must be impossible to
// guess for someone on the side; it comes straight from the system's random generator.
Number numbered() {
    const Bytes given = random(12);

    Number made{};

    for (std::size_t at = 0; at < made.size(); ++at) made[at] = given[at];

    return made;
}

// The message type: the method and the class interleaved bit by bit, the method split into three
// pieces and the class squeezed into bits 4 and 8. It was designed this way so that the two zero
// bits at the front still tell the messages apart.
std::uint16_t merged(Task task, Sort sort) {
    const std::uint16_t one = static_cast<std::uint16_t>(task);

    const std::uint16_t kind = static_cast<std::uint16_t>(((one & 0x0F80) << 2) |
                                                          ((one & 0x0070) << 1) | (one & 0x000F));

    switch (sort) {
        case Sort::told: return kind | 0x0100;
        case Sort::amiss: return kind | 0x0110;
        case Sort::telling: return kind | 0x0010;
        default: return kind;
    }
}

// And back: the class from its two bits, the method from the pieces put together.
void parted(std::uint16_t kind, Message& made) {
    const std::uint16_t one = static_cast<std::uint16_t>(((kind & 0x3E00) >> 2) |
                                                          ((kind & 0x00E0) >> 1) | (kind & 0x000F));

    made.task = static_cast<Task>(one);

    switch (kind & 0x0110) {
        case 0x0100: made.sort = Sort::told; break;
        case 0x0110: made.sort = Sort::amiss; break;
        case 0x0010: made.sort = Sort::telling; break;
        default: made.sort = Sort::asking; break;
    }
}

// A message header with zero length: type, length, cookie and transaction ID.
Bytes begun(Task task, Sort sort, const Number& number) {
    Bytes made;

    made.reserve(20);

    pushed(made, merged(task, sort));
    pushed(made, 0);
    pushed(made, static_cast<std::uint16_t>(cookie >> 16));
    pushed(made, static_cast<std::uint16_t>(cookie & 0xFFFF));

    for (const std::byte one : number) made.push_back(one);

    return made;
}

// Appends an address attribute, XOR-ed or plain depending on what the attribute requires; the mask
// takes the transaction ID from the header already in `made`.
void placing(Bytes& made, std::uint16_t kind, const Address& where, bool xored) {
    const bool four = where.family() == Family::four;

    const std::size_t wide = four ? 4u : 16u;

    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(static_cast<std::byte>(four ? 1 : 2));

    pushed(value, xored ? static_cast<std::uint16_t>(where.port() ^ (cookie >> 16)) : where.port());

    std::byte mask[16]{};

    for (std::size_t one = 0; one < 4; ++one)
        mask[one] = static_cast<std::byte>((cookie >> (24 - 8 * one)) & 0xFF);

    for (std::size_t one = 0; one < 12; ++one) mask[4 + one] = made[8 + one];

    for (std::size_t one = 0; one < wide; ++one)
        value.push_back(xored ? (where.bytes()[one] ^ mask[one]) : where.bytes()[one]);

    pushed(made, kind);
    pushed(made, static_cast<std::uint16_t>(value.size()));

    for (const std::byte one : value) made.push_back(one);

    minded(made);
}

// Appends a 32-bit attribute, such as LIFETIME or REQUESTED-TRANSPORT.
void wording(Bytes& made, std::uint16_t kind, std::uint32_t what) {
    pushed(made, kind);
    pushed(made, 4);

    pushed(made, static_cast<std::uint16_t>(what >> 16));
    pushed(made, static_cast<std::uint16_t>(what & 0xFFFF));

    minded(made);
}

// Appends a DATA attribute (0x0013), padded to four bytes.
void giving(Bytes& made, const Bytes& what) {
    pushed(made, 0x0013);
    pushed(made, static_cast<std::uint16_t>(what.size()));

    for (const std::byte one : what) made.push_back(one);

    while (made.size() % 4 != 0) made.push_back(std::byte{0});

    minded(made);
}

// Wraps data for a channel: the same content in four bytes of overhead instead of thirty-six, a
// channel number instead of the STUN header, cookie, transaction ID and peer address attribute.
// The address lives in the number: the server knows whom it assigned that number to.
//
// The two top bits of the first byte tell what has arrived: zeros mean STUN, 01 means a channel.
// That is why channel numbers start at 0x4000. There is no padding to four bytes over UDP: the
// datagram itself marks the end of the content, and every byte is saved on purpose here.
Bytes wrapped(std::uint16_t channel, const Bytes& what) {
    Bytes made;

    made.reserve(4 + what.size());

    pushed(made, channel);
    pushed(made, static_cast<std::uint16_t>(what.size()));

    for (const std::byte one : what) made.push_back(one);

    return made;
}

// Reads channel data into `channel` and `data`; false when it is not channel data or is cut short.
bool unwrapped(const Bytes& what, std::uint16_t& channel, Bytes& data) {
    if (what.size() < 4) return false;

    // These two bits are the range of channel numbers: 01 at the front is exactly 0x4000 to
    // 0x7FFF. A separate range check would be dead code.
    if ((std::to_integer<unsigned>(what[0]) & 0xC0) != 0x40) return false;

    const std::uint16_t much = whole(what, 2);

    if (what.size() < 4u + much) return false;

    channel = whole(what, 0);

    data.assign(what.begin() + 4, what.begin() + 4 + much);

    return true;
}

// A Binding request with no attributes.
Bytes asked(const Number& number) {
    Bytes made;

    made.reserve(20);

    pushed(made, 0x0001);
    pushed(made, 0);
    pushed(made, static_cast<std::uint16_t>(cookie >> 16));
    pushed(made, static_cast<std::uint16_t>(cookie & 0xFFFF));

    for (const std::byte one : number) made.push_back(one);

    return made;
}

// The answer to a connectivity check: the same transaction ID as in the request and one attribute,
// the XOR-ed address the request came from. That is enough for the other side to know the hole in
// the NAT is open, and to learn its external address on the way.
Bytes told(const Number& number, const Address& seen) {
    const bool four = seen.family() == Family::four;

    const std::size_t wide = four ? 4u : 16u;

    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(static_cast<std::byte>(four ? 1 : 2));

    pushed(value, static_cast<std::uint16_t>(seen.port() ^ (cookie >> 16)));

    std::byte mask[16]{};

    for (std::size_t one = 0; one < 4; ++one)
        mask[one] = static_cast<std::byte>((cookie >> (24 - 8 * one)) & 0xFF);

    for (std::size_t one = 0; one < 12; ++one) mask[4 + one] = number[one];

    for (std::size_t one = 0; one < wide; ++one) value.push_back(seen.bytes()[one] ^ mask[one]);

    Bytes made;

    made.reserve(20 + 4 + value.size());

    pushed(made, 0x0101);
    pushed(made, static_cast<std::uint16_t>(4 + value.size()));
    pushed(made, static_cast<std::uint16_t>(cookie >> 16));
    pushed(made, static_cast<std::uint16_t>(cookie & 0xFFFF));

    for (const std::byte one : number) made.push_back(one);

    pushed(made, 0x0020);
    pushed(made, static_cast<std::uint16_t>(value.size()));

    for (const std::byte one : value) made.push_back(one);

    return made;
}

// Reads a STUN message into `made`; false when it is not one. Unknown attributes are skipped.
bool understood(const Bytes& what, Message& made) {
    if (what.size() < 20) return false;

    // The two top bits are zero: by this, and only this, STUN is recognized on a shared socket.
    if ((std::to_integer<unsigned>(what[0]) & 0xC0) != 0) return false;

    if (wider(what, 4) != cookie) return false;

    const std::uint16_t much = whole(what, 2);

    if (what.size() < 20u + much) return false;

    parted(whole(what, 0), made);

    for (std::size_t at = 0; at < 12; ++at) made.number[at] = what[8 + at];

    made.seen = {};
    made.lent = {};
    made.peer = {};
    made.data.clear();
    made.code = 0;
    made.lifetime = 0;
    made.said.clear();
    made.realm.clear();
    made.nonce.clear();

    bool xored = false;

    std::size_t at = 20;

    while (at + 4 <= 20u + much) {
        const std::uint16_t kind = whole(what, at);
        const std::uint16_t wide = whole(what, at + 2);

        at += 4;

        // An attribute must fit in the declared message, not just in the buffer. Otherwise a
        // sender could declare a length of 4 and make us read further, beyond what it named.
        if (at + wide > 20u + much) break;

        if (at + wide > what.size()) break;

        if (kind == 0x0020) {
            // A broken attribute does not erase a good one: with an unconditional assignment, one
            // bad family byte after a valid address would empty the answer and also shut out the
            // old attribute.
            if (const Address one = placed(what, at, wide, made.number, true); one.held()) {
                made.seen = one;
                xored = true;
            }
        } else if (kind == 0x0001 && !xored) {
            // The old attribute is taken only when the new one was absent; some servers send both.
            if (const Address one = placed(what, at, wide, made.number, false); one.held())
                made.seen = one;
        } else if (kind == 0x0016) {
            // The address lent by the relay: strangers write to us at this one.
            made.lent = placed(what, at, wide, made.number, true);
        } else if (kind == 0x0012) {
            made.peer = placed(what, at, wide, made.number, true);
        } else if (kind == 0x0013) {
            made.data.assign(what.begin() + static_cast<std::ptrdiff_t>(at),
                             what.begin() + static_cast<std::ptrdiff_t>(at + wide));
        } else if (kind == 0x000D && wide >= 4) {
            made.lifetime = static_cast<int>(wider(what, at));
        } else if (kind == 0x0014 || kind == 0x0015) {
            std::string said;

            for (std::size_t one = 0; one < wide && said.size() < 768; ++one) {
                const unsigned sign = std::to_integer<unsigned>(what[at + one]);

                said += sign < 0x20 || sign == 0x7F ? '?' : static_cast<char>(sign);
            }

            // The realm and the nonce come from the server and go back to it unchanged in
            // long-term authentication. They are text from a stranger too, so they also lose their
            // control characters.
            (kind == 0x0014 ? made.realm : made.nonce) = said;
        } else if (kind == 0x0009 && wide >= 4) {
            const unsigned sort = std::to_integer<unsigned>(what[at + 2]) & 0x07;

            made.code = static_cast<int>(sort * 100 + std::to_integer<unsigned>(what[at + 3]) % 100);

            // A stranger writes the reason, and it must not carry control characters: the sentence
            // goes straight into a message and from there to the terminal. Escape sequences from
            // someone else's network could repaint the screen or replace the window title. We also
            // take only a bounded amount.
            for (std::size_t one = 4; one < wide && made.said.size() < 128; ++one) {
                const unsigned sign = std::to_integer<unsigned>(what[at + one]);

                made.said += sign < 0x20 || sign == 0x7F ? '?' : static_cast<char>(sign);
            }
        }

        at += wide;
        at += (4 - wide % 4) % 4;
    }

    return true;
}


// CRC-32 with the IEEE polynomial over the first `much` bytes. This is not cryptography but a
// checksum, the same as in archives, and we write it ourselves because no system exposes it; here
// it is needed only to tell a STUN message apart from anything else on the same socket.
std::uint32_t tallied(const Bytes& what, std::size_t much) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> made{};

        for (std::uint32_t at = 0; at < 256; ++at) {
            std::uint32_t one = at;

            for (int turn = 0; turn < 8; ++turn)
                one = (one & 1) != 0 ? 0xEDB88320u ^ (one >> 1) : one >> 1;

            made[at] = one;
        }

        return made;
    }();

    std::uint32_t made = 0xFFFFFFFFu;

    for (std::size_t at = 0; at < much && at < what.size(); ++at)
        made = table[(made ^ std::to_integer<unsigned>(what[at])) & 0xFF] ^ (made >> 8);

    return made ^ 0xFFFFFFFFu;
}

// Appends a text attribute (USERNAME, REALM, NONCE), padded to four bytes.
void texting(Bytes& made, std::uint16_t kind, const std::string& said) {
    pushed(made, kind);
    pushed(made, static_cast<std::uint16_t>(said.size()));

    for (const char one : said) made.push_back(static_cast<std::byte>(one));

    while (made.size() % 4 != 0) made.push_back(std::byte{0});

    minded(made);
}

// Appends MESSAGE-INTEGRITY, the HMAC-SHA1 of the message under `key`. It is computed over content
// whose length field already pretends the integrity attribute is there. This is where
// implementations go wrong most often: computed with the length from before appending, the other
// side would get something else and reject every message.
void stamping(Bytes& made, const Bytes& key) {
    written(made, static_cast<std::uint16_t>(made.size() - 20 + 24));

    const Bytes sign = stamped(Sum::sha1, key, made);

    pushed(made, 0x0008);
    pushed(made, 20);

    for (const std::byte one : sign) made.push_back(one);
}

// Appends FINGERPRINT with the same length trick, except that the print is a checksum rather than a
// signature. The fingerprint goes at the very end, after the integrity attribute, because it covers
// that too.
void printing(Bytes& made) {
    written(made, static_cast<std::uint16_t>(made.size() - 20 + 8));

    const std::uint32_t sum = tallied(made, made.size()) ^ 0x5354554Eu;

    pushed(made, 0x8028);
    pushed(made, 4);

    pushed(made, static_cast<std::uint16_t>(sum >> 16));
    pushed(made, static_cast<std::uint16_t>(sum & 0xFFFF));
}

// Whether the message carries a valid MESSAGE-INTEGRITY under `key`. Checking it requires
// rebuilding exactly the content the other side computed it over: the message cut before the
// integrity attribute, with a length pretending the attribute is already there.
bool trusted(const Bytes& what, const Bytes& key) {
    if (what.size() < 20) return false;

    const std::uint16_t much = whole(what, 2);

    if (what.size() < 20u + much) return false;

    std::size_t at = 20;

    while (at + 4 <= 20u + much) {
        const std::uint16_t kind = whole(what, at);
        const std::uint16_t wide = whole(what, at + 2);

        if (kind == 0x0008) {
            if (wide != 20 || at + 4 + 20 > what.size()) return false;

            Bytes over{what.begin(), what.begin() + static_cast<std::ptrdiff_t>(at)};

            written(over, static_cast<std::uint16_t>(at - 20 + 24));

            const Bytes sign = stamped(Sum::sha1, key, over);

            if (sign.size() != 20) return false;

            // The comparison runs to the end, without stopping at the first difference. Leaving
            // early could tell a prober how many bytes it guessed.
            unsigned apart = 0;

            for (std::size_t one = 0; one < 20; ++one)
                apart |= std::to_integer<unsigned>(sign[one] ^ what[at + 4 + one]);

            return apart == 0;
        }

        at += 4;
        at += wide;
        at += (4 - wide % 4) % 4;
    }

    return false;
}

// The long-term authentication key is the MD5 of the user name, realm and password joined by
// colons. The RFC says so, and that is how TURN servers compute it.
Bytes keyed(const std::string& user, const std::string& realm, const std::string& word) {
    const std::string said = user + ":" + realm + ":" + word;

    Bytes what(said.size());

    for (std::size_t at = 0; at < said.size(); ++at) what[at] = static_cast<std::byte>(said[at]);

    return summed(Sum::md5, what);
}

}
