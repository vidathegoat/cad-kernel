// Billet benchmark harness: output fingerprint.

#include "fingerprint.hpp"

#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <NCollection_IndexedMap.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>

#include <filesystem>

namespace billet
{

namespace
{

int countShapes(const TopoDS_Shape& theShape, TopAbs_ShapeEnum theType)
{
  NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> aMap;
  TopExp::MapShapes(theShape, theType, aMap);
  return aMap.Extent();
}

} // namespace

Fingerprint computeFingerprint(const TopoDS_Shape& theShape,
                               bool                theCheckValidity,
                               bool                theParallelCheck)
{
  Fingerprint aResult;
  if (theShape.IsNull())
  {
    return aResult;
  }

  GProp_GProps aVolume, anArea;
  BRepGProp::VolumeProperties(theShape, aVolume);
  BRepGProp::SurfaceProperties(theShape, anArea);
  aResult.volume = aVolume.Mass();
  aResult.area   = anArea.Mass();

  Bnd_Box aBox;
  BRepBndLib::Add(theShape, aBox, false);
  aResult.bboxVoid = aBox.IsVoid();
  if (!aResult.bboxVoid)
  {
    aBox.Get(aResult.bbox[0],
             aResult.bbox[1],
             aResult.bbox[2],
             aResult.bbox[3],
             aResult.bbox[4],
             aResult.bbox[5]);
  }

  aResult.solids   = countShapes(theShape, TopAbs_SOLID);
  aResult.shells   = countShapes(theShape, TopAbs_SHELL);
  aResult.faces    = countShapes(theShape, TopAbs_FACE);
  aResult.edges    = countShapes(theShape, TopAbs_EDGE);
  aResult.vertices = countShapes(theShape, TopAbs_VERTEX);

  NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> aFaces;
  TopExp::MapShapes(theShape, TopAbs_FACE, aFaces);
  for (int i = 1; i <= aFaces.Extent(); ++i)
  {
    TopLoc_Location                        aLocation;
    const occ::handle<Poly_Triangulation>& aTriangulation =
      BRep_Tool::Triangulation(TopoDS::Face(aFaces(i)), aLocation);
    if (!aTriangulation.IsNull())
    {
      aResult.triangles += aTriangulation->NbTriangles();
      aResult.nodes += aTriangulation->NbNodes();
    }
  }

  if (theCheckValidity)
  {
    BRepCheck_Analyzer anAnalyzer(theShape, true, theParallelCheck);
    aResult.hasValidity = true;
    aResult.valid       = anAnalyzer.IsValid();
  }
  return aResult;
}

void writeFingerprint(JsonWriter& theWriter, const Fingerprint& theFingerprint)
{
  theWriter.beginObject();
  theWriter.key("volume").value(theFingerprint.volume);
  theWriter.key("area").value(theFingerprint.area);
  theWriter.key("bbox");
  if (theFingerprint.bboxVoid)
  {
    theWriter.value(std::vector<double>());
  }
  else
  {
    theWriter.value(std::vector<double>(theFingerprint.bbox, theFingerprint.bbox + 6));
  }
  theWriter.key("solids").value(theFingerprint.solids);
  theWriter.key("shells").value(theFingerprint.shells);
  theWriter.key("faces").value(theFingerprint.faces);
  theWriter.key("edges").value(theFingerprint.edges);
  theWriter.key("vertices").value(theFingerprint.vertices);
  theWriter.key("triangles").value(theFingerprint.triangles);
  theWriter.key("nodes").value(theFingerprint.nodes);
  if (theFingerprint.hasValidity)
  {
    theWriter.key("valid").value(theFingerprint.valid);
  }
  theWriter.endObject();
}

long long writeBrep(const TopoDS_Shape& theShape, const std::string& thePath, bool theWithTriangles)
{
  if (!BRepTools::Write(theShape,
                        thePath.c_str(),
                        theWithTriangles,
                        false,
                        TopTools_FormatVersion_CURRENT))
  {
    return -1;
  }
  std::error_code anError;
  const auto      aSize = std::filesystem::file_size(thePath, anError);
  return anError ? -1 : static_cast<long long>(aSize);
}

} // namespace billet
