// ===========================================================================
// md5.hpp  --  MD5 hash function (needed because Ketama is defined using MD5)
// ===========================================================================
// MD5 turns any text into 16 bytes that look random but are always the same
// for the same input. (It is NOT safe for security any more, but it is great
// for spreading keys evenly, which is all we use it for.)
#pragma once
#include <array>
#include <cstdint>
#include <string>

std::array<uint8_t, 16> md5(const std::string& input);
std::string to_hex(const std::array<uint8_t, 16>& digest);
