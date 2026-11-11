#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "laminar/status.h"

namespace laminar::projection {
std::string SequenceHex(uint64_t sequence);
std::string IdentityHex(const std::array<char, 16>& id);

// Flat RESP2 replies only; a fresh bounded-deadline Unix connection per command.
Result<std::optional<std::string>> Command(std::string_view socket_path,
                                           std::span<const std::string_view> args);

enum class Point : uint8_t { Script, Sent, RebuildRow, BeforeReady };
struct Fault {
  bool script_error{};
  bool lose_reply{};
};
using FaultHook = Fault (*)(Point) noexcept;
extern thread_local FaultHook fault_hook;
Fault At(Point point) noexcept;
} // namespace laminar::projection
