// Billet benchmark harness: helpers for choosing per-model parameters for the corpus manifest.

#include "probe.hpp"

#include "json_writer.hpp"
#include "modeling.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <GProp_GProps.hxx>
#include <NCollection_IndexedDataMap.hxx>
#include <NCollection_IndexedMap.hxx>
#include <NCollection_List.hxx>
#include <TopExp.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace billet
{

namespace
{

std::string param(const JobOptions&  theOptions,
                  const std::string& theKey,
                  const std::string& theDefault)
{
  const auto anIter = theOptions.params.find(theKey);
  if (anIter != theOptions.params.end())
  {
    return anIter->second;
  }
  if (theDefault.empty())
  {
    throw std::runtime_error("missing parameter: " + theKey);
  }
  return theDefault;
}

std::string joinIndices(const std::vector<int>& theIndices)
{
  std::string aText;
  for (int anIndex : theIndices)
  {
    aText += (aText.empty() ? "" : ",") + std::to_string(anIndex);
  }
  return aText;
}

bool isValidSolid(const TopoDS_Shape& theShape)
{
  return !theShape.IsNull() && BRepCheck_Analyzer(theShape).IsValid();
}

// Absolute value from "<key>", or "<key>_frac" times the bounding box diagonal of the shape.
double sizeParam(const JobOptions&   theOptions,
                 const std::string&  theKey,
                 const TopoDS_Shape& theShape)
{
  if (theOptions.params.count(theKey) != 0)
  {
    return std::stod(theOptions.params.at(theKey));
  }
  const auto aFrac = theOptions.params.find(theKey + "_frac");
  if (aFrac == theOptions.params.end())
  {
    throw std::runtime_error("missing parameter: " + theKey + " or " + theKey + "_frac");
  }
  Bnd_Box aBox;
  BRepBndLib::Add(theShape, aBox, false);
  return std::stod(aFrac->second) * std::sqrt(aBox.SquareExtent());
}

} // namespace

std::string probeFillet(const JobOptions& theOptions)
{
  const std::string  aSelect = param(theOptions, "select", "largest-solid");
  const int          aCount  = std::stoi(param(theOptions, "count", "12"));
  const int          aTries  = std::stoi(param(theOptions, "tries", "200"));
  const TopoDS_Shape aShape  = loadModelShape(theOptions.model, aSelect);
  const double       aRadius = sizeParam(theOptions, "radius", aShape);

  NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> anEdges;
  TopExp::MapShapes(aShape, TopAbs_EDGE, anEdges);
  NCollection_IndexedDataMap<TopoDS_Shape, NCollection_List<TopoDS_Shape>, TopTools_ShapeMapHasher>
    anEdgeFaces;
  TopExp::MapShapesAndUniqueAncestors(aShape, TopAbs_EDGE, TopAbs_FACE, anEdgeFaces);

  // Candidates: manifold edges between two different faces, long enough for the radius; longest
  // first.
  std::vector<std::pair<double, int>> aCandidates;
  for (int i = 1; i <= anEdges.Extent(); ++i)
  {
    const TopoDS_Edge& anEdge = TopoDS::Edge(anEdges(i));
    if (BRep_Tool::Degenerated(anEdge))
    {
      continue;
    }
    const NCollection_List<TopoDS_Shape>* aFaces = anEdgeFaces.Seek(anEdge);
    if (aFaces == nullptr || aFaces->Extent() != 2)
    {
      continue;
    }
    BRepAdaptor_Curve aCurve(anEdge);
    const double      aLength = GCPnts_AbscissaPoint::Length(aCurve);
    if (aLength >= 4.0 * aRadius)
    {
      aCandidates.emplace_back(aLength, i);
    }
  }
  std::stable_sort(aCandidates.begin(), aCandidates.end(), [](const auto& theA, const auto& theB) {
    return theA.first > theB.first;
  });

  std::vector<int> anAccepted;
  int              aTried = 0;
  for (const auto& aCandidate : aCandidates)
  {
    if (static_cast<int>(anAccepted.size()) >= aCount || aTried >= aTries)
    {
      break;
    }
    ++aTried;
    std::vector<int> aTrial = anAccepted;
    aTrial.push_back(aCandidate.second);
    try
    {
      BRepFilletAPI_MakeFillet aMaker(aShape);
      for (int anIndex : aTrial)
      {
        aMaker.Add(aRadius, TopoDS::Edge(anEdges(anIndex)));
      }
      aMaker.Build();
      if (aMaker.IsDone() && isValidSolid(aMaker.Shape()))
      {
        anAccepted = aTrial;
      }
    }
    catch (const std::exception&)
    {
      // A failing candidate is skipped.
    }
  }
  std::sort(anAccepted.begin(), anAccepted.end());

  JsonWriter aJson;
  aJson.beginObject();
  aJson.key("op").value("fillet");
  aJson.key("select").value(aSelect);
  aJson.key("radius").value(aRadius);
  aJson.key("edges").value(joinIndices(anAccepted));
  aJson.key("count").value(anAccepted.size());
  aJson.key("candidates").value(aCandidates.size());
  aJson.key("tried").value(aTried);
  aJson.endObject();
  return aJson.text();
}

std::string probeShell(const JobOptions& theOptions)
{
  const std::string  aSelect    = param(theOptions, "select", "largest-solid");
  const int          aTries     = std::stoi(param(theOptions, "tries", "20"));
  const TopoDS_Shape aShape     = loadModelShape(theOptions.model, aSelect);
  const double       aThickness = sizeParam(theOptions, "thickness", aShape);

  NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> aFaces;
  TopExp::MapShapes(aShape, TopAbs_FACE, aFaces);
  std::vector<std::pair<double, int>> aPlanar;
  for (int i = 1; i <= aFaces.Extent(); ++i)
  {
    const TopoDS_Face& aFace = TopoDS::Face(aFaces(i));
    if (BRepAdaptor_Surface(aFace, false).GetType() == GeomAbs_Plane)
    {
      GProp_GProps aProps;
      BRepGProp::SurfaceProperties(aFace, aProps);
      aPlanar.emplace_back(aProps.Mass(), i);
    }
  }
  std::stable_sort(aPlanar.begin(), aPlanar.end(), [](const auto& theA, const auto& theB) {
    return theA.first > theB.first;
  });

  int         aTried = 0;
  std::string aFound;
  bool        aClosed = false;
  for (const auto& aCandidate : aPlanar)
  {
    if (aTried >= aTries)
    {
      break;
    }
    ++aTried;
    try
    {
      if (isValidSolid(
            thickSolid(aShape, {TopoDS::Face(aFaces(aCandidate.second))}, aThickness, "probe")))
      {
        aFound = std::to_string(aCandidate.second);
        break;
      }
    }
    catch (const std::exception&)
    {
    }
  }
  if (aFound.empty())
  {
    try
    {
      aClosed = isValidSolid(thickSolid(aShape, {}, aThickness, "probe"));
    }
    catch (const std::exception&)
    {
    }
  }

  JsonWriter aJson;
  aJson.beginObject();
  aJson.key("op").value("shell");
  aJson.key("select").value(aSelect);
  aJson.key("thickness").value(aThickness);
  aJson.key("faces").value(aFound);
  aJson.key("found").value(!aFound.empty() || aClosed);
  aJson.key("closed_shell").value(aFound.empty() && aClosed);
  aJson.key("tried").value(aTried);
  aJson.endObject();
  return aJson.text();
}

} // namespace billet
