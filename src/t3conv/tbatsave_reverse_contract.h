#pragma once

#include <cstdint>

namespace t3conv {

inline constexpr std::uint64_t kTbatsavePostInitCallbackRva = 0x1BEF70;
inline constexpr std::uint64_t kTbatsaveBindRefSetterRva = 0x1BEEF0;
inline constexpr std::uint64_t kTbatsaveBindModeSetterRva = 0x1BEF00;

struct TbatsaveExportUiLayout {
    std::uint64_t handler_export_stack_offset = 0x70;
    std::uint64_t bind_mode_field_offset = 0x950;
    std::uint64_t bind_ref_control_offset = 0x958;
    std::uint64_t bind_ref_value_field_offset = 0xE8;
    std::uint64_t bind_ref_state_offset = 0x998;
    std::uint64_t bind_control_offset = 0xA48;
    std::uint64_t bind_state_offset = 0xA88;
    std::uint64_t insert_control_offset = 0xB40;
    std::uint64_t insert_state_offset = 0xB80;
    std::uint64_t bind_radio_value_field_offset = 0xF0;
    std::uint64_t bind_ref_control_id = 0x2715;
    std::uint64_t bind_control_id = 0x2717;
    std::uint64_t insert_control_id = 0x2718;
};

inline constexpr TbatsaveExportUiLayout kTbatsaveExportUiLayout{};

}  // namespace t3conv
