#include "tal/secrets.hpp"

#include <utility>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/crypto.hpp"
#include "tal/process.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <wincred.h>

#include "platform/offload.hpp"
#endif

// The secrets of a program (keys of services, tokens), kept by the system, never in its files:
//
// - Windows: the Credential Manager, a generic credential of the name given, local to the machine.
// - Elsewhere: the Secret Service (GNOME Keyring, KWallet) through secret-tool, the secret on its
//   input, never in its arguments, which ps shows; the item is "service tal, key <name>". Without
//   secret-tool it is unsupported.
//
// Every copy of a secret that tal holds on the way is wiped once it is handed on.
namespace tal {

namespace secreting {

// The program of the Secret Service, secret-tool; the trials put their own in its place.
Path& tool() {
    static Path one{"secret-tool"};

    return one;
}

}

namespace {

#ifdef _WIN32

// The name `name` as Windows takes it.
std::wstring targeted(const std::string& name) { return text::wide(name); }

// The message of the Windows error `code`.
std::string spoken(DWORD code) { return "Windows error " + std::to_string(code) + "."; }

// The system's own store.
class Vault : public Secrets {
public:
    [[nodiscard]] std::string_view name() const override { return "credential-manager"; }

    [[nodiscard]] std::string_view store() const override { return "Windows Credential Manager"; }

    // The secret of `name`, or none.
    Task<std::optional<std::string>> read(std::string name) override {
        return platform::offloaded([name = std::move(name)]() -> std::optional<std::string> {
            PCREDENTIALW found = nullptr;
            const std::wstring target = targeted(name);

            if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &found) == FALSE) {
                const DWORD code = GetLastError();

                if (code == ERROR_NOT_FOUND) return std::nullopt;

                throw Trouble{Kind::other, spoken(code), name, static_cast<int>(code)};
            }

            std::string made(reinterpret_cast<const char*>(found->CredentialBlob), found->CredentialBlobSize);

            SecureZeroMemory(found->CredentialBlob, found->CredentialBlobSize);
            CredFree(found);

            return made;
        });
    }

    // Keeps `secret` as `name`, in place of what was kept.
    Task<void> write(std::string name, std::string secret) override {
        return platform::offloaded([name = std::move(name), secret = std::move(secret)]() mutable {
            std::wstring target = targeted(name);
            CREDENTIALW credential{};

            credential.Type = CRED_TYPE_GENERIC;
            credential.TargetName = target.data();
            credential.CredentialBlob = reinterpret_cast<LPBYTE>(secret.data());
            credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
            credential.Persist = CRED_PERSIST_LOCAL_MACHINE;

            const BOOL went = CredWriteW(&credential, 0);
            const DWORD code = went == FALSE ? GetLastError() : 0;

            crypto::wiped(secret);

            if (went == FALSE) throw Trouble{Kind::other, spoken(code), name, static_cast<int>(code)};
        });
    }

    // Forgets `name`; one not kept is no trouble.
    Task<void> remove(std::string name) override {
        return platform::offloaded([name = std::move(name)] {
            const std::wstring target = targeted(name);

            if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) == FALSE && GetLastError() != ERROR_NOT_FOUND) throw Trouble{Kind::other, spoken(GetLastError()), name};
        });
    }
};

#else

// What a run of secret-tool said: whether it started, its code, its output.
struct Said {
    bool started = false;
    int code = -1;
    std::string output;
};

// Runs secret-tool with `arguments`, `input` on its input (wiped here).
Task<Said> told(std::vector<std::string> arguments, std::string input = {}) {
    Command what;
    Bytes given = bytes(input);
    Said made;

    crypto::wiped(input);
    what.program = secreting::tool();
    what.arguments = std::move(arguments);

    try {
        Output out = co_await Process::run(std::move(what), std::move(given));

        made.started = true;
        made.code = out.exit.ended == Exit::Ended::normal ? out.exit.code : -1;
        made.output = text::of(out.output);
        crypto::wiped(out.output);
    } catch (const Trouble&) {
        made.started = false;
    }

    co_return made;
}

// The Secret Service through secret-tool.
class Vault : public Secrets {
public:
    [[nodiscard]] std::string_view name() const override { return "secret-service"; }

    [[nodiscard]] std::string_view store() const override { return "Secret Service"; }

    // The secret of `name`, or none; unsupported without secret-tool.
    Task<std::optional<std::string>> read(std::string name) override {
        std::vector<std::string> asked{"lookup", "service", "tal", "key", name};
        Said said = co_await told(std::move(asked));

        if (!said.started) throw Trouble{Kind::unsupported, "The system has no secret-tool.", name};

        if (said.code != 0) co_return std::nullopt;

        std::string made = std::move(said.output);

        if (!made.empty() && made.back() == '\n') made.pop_back();

        co_return std::optional<std::string>{std::move(made)};
    }

    // Keeps `secret` as `name`, given on the input of secret-tool.
    Task<void> write(std::string name, std::string secret) override {
        std::vector<std::string> asked{"store", "--label=" + name, "service", "tal", "key", name};
        const Said said = co_await told(std::move(asked), std::move(secret));

        if (!said.started) throw Trouble{Kind::unsupported, "The system has no secret-tool.", name};

        if (said.code != 0) throw Trouble{Kind::refused, "The Secret Service did not keep it.", name, said.code};
    }

    // Forgets `name`.
    Task<void> remove(std::string name) override {
        std::vector<std::string> asked{"clear", "service", "tal", "key", name};
        const Said said = co_await told(std::move(asked));

        if (!said.started) throw Trouble{Kind::unsupported, "The system has no secret-tool.", name};
    }
};

#endif

}

// The built-in adapter of the port: the system's store, one for the whole program.
std::shared_ptr<Secrets> Secrets::builtin() {
    static const std::shared_ptr<Secrets> made = std::make_shared<Vault>();

    return made;
}

// The secret kept as `name`, or none; refused while `refusing`.
Task<std::optional<std::string>> MemorySecrets::read(std::string name) {
    if (refusing) throw Trouble{Kind::refused, "The store refuses.", name};

    const auto found = held.find(name);

    if (found == held.end()) co_return std::nullopt;

    co_return std::optional<std::string>{found->second};
}

// Keeps `secret` as `name`; refused while `refusing`.
Task<void> MemorySecrets::write(std::string name, std::string secret) {
    if (refusing) throw Trouble{Kind::refused, "The store refuses.", name};

    held[std::move(name)] = std::move(secret);

    co_return;
}

// Forgets `name`.
Task<void> MemorySecrets::remove(std::string name) {
    held.erase(name);

    co_return;
}

}
