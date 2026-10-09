#pragma once

#include "talcite/shape.hpp"
#include "talcite/modal.hpp"

namespace talcite::ui {

class Dialog : public Modal {
public:
    explicit Dialog(ModalShape shape = {});

protected:
    bool handle(KeyEvent& what) override;
};

}
