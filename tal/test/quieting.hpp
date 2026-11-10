#pragma once

#include "tal/testing.hpp"

#define QUIETED(loop) UNTIL((loop), !(loop).turn())
