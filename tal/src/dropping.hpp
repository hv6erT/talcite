#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "tal/node.hpp"
#include "tal/window.hpp"

namespace tal::dropping {

class Drop : public Window {
public:
    Drop(Node& user, std::vector<std::string> rows, std::size_t current, std::size_t shown, std::string kind);

    bool dropped();

protected:
    virtual void taken(Node& owner, std::size_t row) = 0;

private:
    class Rows;
    class Entries;
    class Holder;

    [[nodiscard]] Node* owner() const;

    void chosen(std::size_t row);

    std::weak_ptr<Node> owner_;
    Hooks heard_;
};

}
