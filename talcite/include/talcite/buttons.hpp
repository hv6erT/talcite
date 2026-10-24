#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace talcite::ui {

class Button;
class Form;
class Message;
class Node;

class Buttons {
public:
    Buttons(const Buttons&) = delete;
    Buttons& operator=(const Buttons&) = delete;
    Buttons(Buttons&&) = delete;
    Buttons& operator=(Buttons&&) = delete;

    std::shared_ptr<Button> add(std::string text);

    void clear();

    [[nodiscard]] std::size_t many() const { return all_.size(); }

    [[nodiscard]] bool empty() const { return all_.empty(); }

    [[nodiscard]] const std::shared_ptr<Button>& operator[](std::size_t which) const;

    [[nodiscard]] const std::vector<std::shared_ptr<Button>>& all() const { return all_; }

private:
    friend class Form;
    friend class Message;

    explicit Buttons(Node& row) : row_(&row) {}

    ~Buttons() = default;

    void role(std::string given);

    Node* row_;
    std::string role_;
    std::vector<std::shared_ptr<Button>> all_;
};

}
