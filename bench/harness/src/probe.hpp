// Billet benchmark harness: helpers for choosing per-model parameters for the corpus manifest.

#pragma once

#include "operations.hpp"

#include <string>

namespace billet
{

// Greedily collects edges that fillet together with the given radius into a valid solid.
// Parameters: select (default largest-solid), radius or radius_frac (of the bounding box diagonal),
// count (default 12), tries (default 200).
std::string probeFillet(const JobOptions& theOptions);

// Finds a planar face to open so that shelling with the given thickness gives a valid solid.
// Parameters: select (default largest-solid), thickness or thickness_frac (negative is inward),
// tries (default 20).
std::string probeShell(const JobOptions& theOptions);

} // namespace billet
