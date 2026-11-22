// SPDX-License-Identifier: GPL-3.0-or-later
// Recompia-only 2.0.1 unified bag transport, gated by the explicit guest island building toggle.
#pragma once
#include "core/pkp_feature.h"
namespace nxrt::games::pkp::unified {
bool requested();  // [pkp] guest_island_building, default off, never in seamless coop
}
namespace nxrt::games::pkp {
extern const Feature kUnifiedInventoryFeature;
}
