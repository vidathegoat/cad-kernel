// Billet benchmark harness: scripted parts for the "rebuild" operation.
// Each part replays a sketch-and-feature sequence from scratch, as a CAD rebuild would.

#pragma once

#include <TopoDS_Shape.hxx>

#include <string>
#include <vector>

namespace billet
{

struct PartInfo
{
  std::string id;
  std::string description;
};

// Ids and descriptions of all scripted parts, in a fixed order.
const std::vector<PartInfo>& scriptedParts();

// Builds the part with the given id. Throws std::runtime_error for an unknown id or a failed
// feature.
TopoDS_Shape buildPart(const std::string& theId);

} // namespace billet
