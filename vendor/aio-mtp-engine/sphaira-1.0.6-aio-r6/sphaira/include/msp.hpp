#pragma once

#include <switch.h>

namespace sphaira::ui {
struct ProgressBox;
}

namespace sphaira::yati::source {
struct Base;
}

namespace sphaira::msp {

Result Install(ui::ProgressBox* pbox, yati::source::Base* source, s64 source_size = -1);

} // namespace sphaira::msp
