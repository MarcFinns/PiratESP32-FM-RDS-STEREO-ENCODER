#pragma once

#include <cstddef>
#include <cstdint>

namespace Diagnostics
{
void verifySIMD();
int32_t findPeakAbs(const int32_t *buffer, std::size_t samples);
}

