#pragma once

#include "talcite/style.hpp"
#include "talcite/veil.hpp"

namespace talcite::ui {

class Dialog : public Veil {
public:
    explicit Dialog(VeilStyle style = {});

protected:
    bool react(KeyEvent& what) override;
};

}
