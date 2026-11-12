#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tal {

class Step {
public:
    Step() = default;

    virtual ~Step();

    Step(const Step&) = delete;
    Step& operator=(const Step&) = delete;

    virtual void undo() = 0;

    virtual void redo() = 0;

    [[nodiscard]] virtual std::string said() const = 0;

    [[nodiscard]] virtual bool merged(const Step& later);
};

class Undo {
public:
    explicit Undo(std::size_t most = 1000);

    void push(std::unique_ptr<Step> one);

    bool undo();

    bool redo();

    void seal();

    void clear();

    [[nodiscard]] std::optional<std::string> undoing() const;

    [[nodiscard]] std::optional<std::string> redoing() const;

private:
    std::size_t most_;
    std::vector<std::unique_ptr<Step>> done_;
    std::vector<std::unique_ptr<Step>> undone_;
    bool sealed_ = true;
};

}
