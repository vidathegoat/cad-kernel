// Billet benchmark harness: benchmarked operations.

#include "operations.hpp"

#include "fingerprint.hpp"
#include "modeling.hpp"
#include "parts.hpp"

#include <APIHeaderSection_MakeHeader.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <IMeshTools_Parameters.hxx>
#include <NCollection_IndexedMap.hxx>
#include <NCollection_List.hxx>
#include <NCollection_Sequence.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <StepData_StepModel.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TDF_Label.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace billet
{

namespace
{

using ShapeMap = NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;

std::string paramString(const JobOptions&  theOptions,
                        const std::string& theKey,
                        const std::string& theDefault)
{
  const auto anIter = theOptions.params.find(theKey);
  return anIter == theOptions.params.end() ? theDefault : anIter->second;
}

double paramDouble(const JobOptions& theOptions, const std::string& theKey)
{
  const auto anIter = theOptions.params.find(theKey);
  if (anIter == theOptions.params.end())
  {
    throw std::runtime_error("missing parameter: " + theKey);
  }
  return std::stod(anIter->second);
}

std::vector<double> parseNumbers(const std::string& theText)
{
  std::vector<double> aResult;
  std::stringstream   aStream(theText);
  std::string         anItem;
  while (std::getline(aStream, anItem, ','))
  {
    if (!anItem.empty())
    {
      aResult.push_back(std::stod(anItem));
    }
  }
  return aResult;
}

std::vector<int> parseIndices(const std::string& theText)
{
  std::vector<int> aResult;
  for (double aValue : parseNumbers(theText))
  {
    aResult.push_back(static_cast<int>(aValue));
  }
  return aResult;
}

TopoDS_Shape loadStepShape(const std::string& thePath)
{
  if (!std::filesystem::exists(thePath))
  {
    throw std::runtime_error("model file not found: " + thePath);
  }
  STEPControl_Reader aReader;
  if (aReader.ReadFile(thePath.c_str()) != IFSelect_RetDone)
  {
    throw std::runtime_error("cannot read STEP file: " + thePath);
  }
  aReader.TransferRoots();
  TopoDS_Shape aShape = aReader.OneShape();
  if (aShape.IsNull())
  {
    throw std::runtime_error("STEP file contains no shape: " + thePath);
  }
  return aShape;
}

// "all" (default), "largest-solid", or "solid:<n>" (1-based, in TopExp::MapShapes order).
TopoDS_Shape selectShape(const TopoDS_Shape& theShape, const std::string& theSelection)
{
  if (theSelection.empty() || theSelection == "all")
  {
    return theShape;
  }
  ShapeMap aSolids;
  TopExp::MapShapes(theShape, TopAbs_SOLID, aSolids);
  if (aSolids.IsEmpty())
  {
    throw std::runtime_error("selection '" + theSelection + "': model has no solids");
  }
  if (theSelection == "largest-solid")
  {
    int    aBest       = 1;
    double aBestVolume = -1.0;
    for (int i = 1; i <= aSolids.Extent(); ++i)
    {
      GProp_GProps aProps;
      BRepGProp::VolumeProperties(aSolids(i), aProps);
      if (aProps.Mass() > aBestVolume)
      {
        aBestVolume = aProps.Mass();
        aBest       = i;
      }
    }
    return aSolids(aBest);
  }
  if (theSelection.rfind("solid:", 0) == 0)
  {
    const int anIndex = std::stoi(theSelection.substr(6));
    if (anIndex < 1 || anIndex > aSolids.Extent())
    {
      throw std::runtime_error("selection '" + theSelection + "' is out of range");
    }
    return aSolids(anIndex);
  }
  throw std::runtime_error("unknown selection: " + theSelection);
}

TopoDS_Shape loadInput(const JobOptions& theOptions, const std::string& theDefaultSelection)
{
  return selectShape(loadStepShape(theOptions.model),
                     paramString(theOptions, "select", theDefaultSelection));
}

double boxDiagonal(const TopoDS_Shape& theShape)
{
  Bnd_Box aBox;
  BRepBndLib::Add(theShape, aBox, false);
  if (aBox.IsVoid())
  {
    throw std::runtime_error("empty bounding box");
  }
  return std::sqrt(aBox.SquareExtent());
}

// ---------------------------------------------------------------------------

class RebuildOperation : public Operation
{
public:
  void setup(const JobOptions& theOptions) override
  {
    myPartId = theOptions.model;
    for (const PartInfo& aPart : scriptedParts())
    {
      if (aPart.id == myPartId)
      {
        return;
      }
    }
    throw std::runtime_error("unknown scripted part: " + myPartId);
  }

  void run() override { myResult = buildPart(myPartId); }

  TopoDS_Shape result() const override { return myResult; }

  void describe(JsonWriter& theWriter) const override { theWriter.key("part").value(myPartId); }

private:
  std::string  myPartId;
  TopoDS_Shape myResult;
};

class StepReadOperation : public Operation
{
public:
  void setup(const JobOptions& theOptions) override
  {
    myPath = theOptions.model;
    if (!std::filesystem::exists(myPath))
    {
      throw std::runtime_error("model file not found: " + myPath);
    }
    myApp = XCAFApp_Application::GetApplication();
  }

  void prepareRun() override { closeDocument(); }

  void run() override
  {
    myApp->NewDocument("MDTV-XCAF", myDoc);
    STEPCAFControl_Reader aReader;
    aReader.SetColorMode(true);
    aReader.SetNameMode(true);
    aReader.SetLayerMode(true);
    aReader.SetPropsMode(true);
    if (aReader.ReadFile(myPath.c_str()) != IFSelect_RetDone)
    {
      throw std::runtime_error("cannot read STEP file: " + myPath);
    }
    if (!aReader.Transfer(myDoc))
    {
      throw std::runtime_error("STEP transfer to XDE document failed: " + myPath);
    }
  }

  TopoDS_Shape result() const override
  {
    NCollection_Sequence<TDF_Label> aLabels;
    XCAFDoc_DocumentTool::ShapeTool(myDoc->Main())->GetFreeShapes(aLabels);
    if (aLabels.Length() == 1)
    {
      return XCAFDoc_ShapeTool::GetShape(aLabels.First());
    }
    TopoDS_Compound aCompound;
    BRep_Builder    aBuilder;
    aBuilder.MakeCompound(aCompound);
    for (const TDF_Label& aLabel : aLabels)
    {
      aBuilder.Add(aCompound, XCAFDoc_ShapeTool::GetShape(aLabel));
    }
    return aCompound;
  }

  void describe(JsonWriter& theWriter) const override
  {
    NCollection_Sequence<TDF_Label>      aFree, aAll;
    const occ::handle<XCAFDoc_ShapeTool> aTool = XCAFDoc_DocumentTool::ShapeTool(myDoc->Main());
    aTool->GetFreeShapes(aFree);
    aTool->GetShapes(aAll);
    theWriter.key("free_shapes").value(aFree.Length());
    theWriter.key("shape_labels").value(aAll.Length());
  }

  ~StepReadOperation() override { closeDocument(); }

private:
  void closeDocument()
  {
    if (!myDoc.IsNull())
    {
      myApp->Close(myDoc);
      myDoc.Nullify();
    }
  }

  std::string                      myPath;
  occ::handle<XCAFApp_Application> myApp;
  occ::handle<TDocStd_Document>    myDoc;
};

class StepWriteOperation : public Operation
{
public:
  void setup(const JobOptions& theOptions) override
  {
    myShape = loadInput(theOptions, "all");
    myPath  = theOptions.outputFile;
    if (myPath.empty())
    {
      const std::filesystem::path aScratch = theOptions.scratchDir.empty()
                                               ? std::filesystem::temp_directory_path()
                                               : theOptions.scratchDir;
      myPath                               = (aScratch / "billet-step-write.step").string();
    }
  }

  void run() override
  {
    STEPControl_Writer aWriter;
    if (aWriter.Transfer(myShape, STEPControl_AsIs) != IFSelect_RetDone)
    {
      throw std::runtime_error("STEP transfer failed");
    }
    // A fixed time stamp makes repeated exports byte-identical.
    APIHeaderSection_MakeHeader aHeader(aWriter.Model());
    aHeader.SetTimeStamp(new TCollection_HAsciiString("2000-01-01T00:00:00"));
    if (aWriter.Write(myPath.c_str()) != IFSelect_RetDone)
    {
      throw std::runtime_error("cannot write STEP file: " + myPath);
    }
    myEntities = aWriter.Model()->NbEntities();
  }

  TopoDS_Shape result() const override { return myShape; }

  bool checkResult() const override { return false; }

  long long saveOutput(const JobOptions&) const override
  {
    std::error_code anError;
    const auto      aSize = std::filesystem::file_size(myPath, anError);
    return anError ? -1 : static_cast<long long>(aSize);
  }

  void describe(JsonWriter& theWriter) const override
  {
    theWriter.key("step_entities").value(myEntities);
    theWriter.key("step_file").value(myPath);
  }

private:
  TopoDS_Shape myShape;
  std::string  myPath;
  int          myEntities = 0;
};

template <class Algo>
class BooleanOperation : public Operation
{
public:
  void setup(const JobOptions& theOptions) override
  {
    myParallel = theOptions.parallel;
    myObject   = loadInput(theOptions, "all");

    Bnd_Box aBox;
    BRepBndLib::Add(myObject, aBox, false);
    double aXmin, aYmin, aZmin, aXmax, aYmax, aZmax;
    aBox.Get(aXmin, aYmin, aZmin, aXmax, aYmax, aZmax);
    std::vector<double> aFractions =
      parseNumbers(paramString(theOptions, "translate", "0.1,0.07,0.05"));
    if (aFractions.size() != 3)
    {
      throw std::runtime_error("parameter 'translate' needs three comma-separated fractions");
    }
    myShift = gp_Vec(aFractions[0] * (aXmax - aXmin),
                     aFractions[1] * (aYmax - aYmin),
                     aFractions[2] * (aZmax - aZmin));
    gp_Trsf aTrsf;
    aTrsf.SetTranslation(myShift);
    myTool = BRepBuilderAPI_Transform(myObject, aTrsf, true).Shape();
  }

  void run() override
  {
    NCollection_List<TopoDS_Shape> anArgs, aTools;
    anArgs.Append(myObject);
    aTools.Append(myTool);
    Algo anAlgo;
    anAlgo.SetArguments(anArgs);
    anAlgo.SetTools(aTools);
    anAlgo.SetRunParallel(myParallel);
    anAlgo.Build();
    if (!anAlgo.IsDone() || anAlgo.HasErrors())
    {
      throw std::runtime_error("boolean operation failed");
    }
    myResult = anAlgo.Shape();
  }

  TopoDS_Shape result() const override { return myResult; }

  void describe(JsonWriter& theWriter) const override
  {
    theWriter.key("tool_translation")
      .value(std::vector<double>{myShift.X(), myShift.Y(), myShift.Z()});
  }

private:
  bool         myParallel = false;
  TopoDS_Shape myObject, myTool, myResult;
  gp_Vec       myShift;
};

class FilletOperation : public Operation
{
public:
  void setup(const JobOptions& theOptions) override
  {
    myShape  = loadInput(theOptions, "largest-solid");
    myRadius = paramDouble(theOptions, "radius");
    ShapeMap anEdges;
    TopExp::MapShapes(myShape, TopAbs_EDGE, anEdges);
    for (int anIndex : parseIndices(paramString(theOptions, "edges", "")))
    {
      if (anIndex < 1 || anIndex > anEdges.Extent())
      {
        throw std::runtime_error("fillet edge index out of range: " + std::to_string(anIndex));
      }
      myEdges.push_back(TopoDS::Edge(anEdges(anIndex)));
    }
    if (myEdges.empty())
    {
      throw std::runtime_error("parameter 'edges' lists no edges");
    }
  }

  void run() override
  {
    BRepFilletAPI_MakeFillet aMaker(myShape);
    for (const TopoDS_Edge& anEdge : myEdges)
    {
      aMaker.Add(myRadius, anEdge);
    }
    aMaker.Build();
    if (!aMaker.IsDone())
    {
      throw std::runtime_error("fillet failed");
    }
    myResult = aMaker.Shape();
  }

  TopoDS_Shape result() const override { return myResult; }

  void describe(JsonWriter& theWriter) const override
  {
    theWriter.key("radius").value(myRadius);
    theWriter.key("edge_count").value(myEdges.size());
  }

private:
  TopoDS_Shape             myShape, myResult;
  double                   myRadius = 0.0;
  std::vector<TopoDS_Edge> myEdges;
};

class ShellOperation : public Operation
{
public:
  void setup(const JobOptions& theOptions) override
  {
    myShape     = loadInput(theOptions, "largest-solid");
    myThickness = paramDouble(theOptions, "thickness");
    ShapeMap aFaces;
    TopExp::MapShapes(myShape, TopAbs_FACE, aFaces);
    for (int anIndex : parseIndices(paramString(theOptions, "faces", "")))
    {
      if (anIndex < 1 || anIndex > aFaces.Extent())
      {
        throw std::runtime_error("shell face index out of range: " + std::to_string(anIndex));
      }
      myOpenFaces.push_back(TopoDS::Face(aFaces(anIndex)));
    }
  }

  void run() override { myResult = thickSolid(myShape, myOpenFaces, myThickness, "shell"); }

  TopoDS_Shape result() const override { return myResult; }

  void describe(JsonWriter& theWriter) const override
  {
    theWriter.key("thickness").value(myThickness);
    theWriter.key("open_faces").value(myOpenFaces.size());
  }

private:
  TopoDS_Shape             myShape, myResult;
  double                   myThickness = 0.0;
  std::vector<TopoDS_Face> myOpenFaces;
};

class MeshOperation : public Operation
{
public:
  void setup(const JobOptions& theOptions) override
  {
    myShape                     = loadInput(theOptions, "all");
    const std::string aPreset   = paramString(theOptions, "deflection", "coarse");
    const double      aDiagonal = boxDiagonal(myShape);
    myParams.InParallel         = theOptions.parallel;
    if (aPreset == "coarse")
    {
      myParams.Deflection = 1.0e-3 * aDiagonal;
      myParams.Angle      = 0.5;
    }
    else if (aPreset == "fine")
    {
      myParams.Deflection = 2.0e-4 * aDiagonal;
      myParams.Angle      = 0.2;
    }
    else
    {
      throw std::runtime_error("parameter 'deflection' must be 'coarse' or 'fine'");
    }
    myPreset = aPreset;
  }

  void prepareRun() override { BRepTools::Clean(myShape); }

  void run() override
  {
    BRepMesh_IncrementalMesh aMesher(myShape, myParams);
    if (!aMesher.IsDone())
    {
      throw std::runtime_error("meshing failed");
    }
    myStatus = aMesher.GetStatusFlags();
  }

  TopoDS_Shape result() const override { return myShape; }

  bool checkResult() const override { return false; }

  long long saveOutput(const JobOptions& theOptions) const override
  {
    return writeBrep(myShape, theOptions.outputFile, true);
  }

  void describe(JsonWriter& theWriter) const override
  {
    theWriter.key("preset").value(myPreset);
    theWriter.key("linear_deflection").value(myParams.Deflection);
    theWriter.key("angular_deflection").value(myParams.Angle);
    theWriter.key("mesher_status").value(myStatus);
  }

private:
  TopoDS_Shape          myShape;
  IMeshTools_Parameters myParams;
  std::string           myPreset;
  int                   myStatus = 0;
};

class CheckOperation : public Operation
{
public:
  void setup(const JobOptions& theOptions) override
  {
    myShape    = loadInput(theOptions, "all");
    myParallel = theOptions.parallel;
  }

  void run() override
  {
    BRepCheck_Analyzer anAnalyzer(myShape, true, myParallel);
    myValid = anAnalyzer.IsValid();
  }

  TopoDS_Shape result() const override { return myShape; }

  bool checkResult() const override { return false; }

  void describe(JsonWriter& theWriter) const override { theWriter.key("valid").value(myValid); }

private:
  TopoDS_Shape myShape;
  bool         myParallel = false;
  bool         myValid    = false;
};

} // namespace

long long Operation::saveOutput(const JobOptions& theOptions) const
{
  return writeBrep(result(), theOptions.outputFile, false);
}

TopoDS_Shape loadModelShape(const std::string& thePath, const std::string& theSelection)
{
  return selectShape(loadStepShape(thePath), theSelection);
}

const std::vector<std::string>& operationIds()
{
  static const std::vector<std::string> kIds = {"rebuild",
                                                "shell",
                                                "step-read",
                                                "step-write",
                                                "bool-fuse",
                                                "bool-cut",
                                                "bool-common",
                                                "fillet",
                                                "mesh",
                                                "check"};
  return kIds;
}

std::unique_ptr<Operation> createOperation(const std::string& theId)
{
  if (theId == "rebuild")
    return std::make_unique<RebuildOperation>();
  if (theId == "shell")
    return std::make_unique<ShellOperation>();
  if (theId == "step-read")
    return std::make_unique<StepReadOperation>();
  if (theId == "step-write")
    return std::make_unique<StepWriteOperation>();
  if (theId == "bool-fuse")
    return std::make_unique<BooleanOperation<BRepAlgoAPI_Fuse>>();
  if (theId == "bool-cut")
    return std::make_unique<BooleanOperation<BRepAlgoAPI_Cut>>();
  if (theId == "bool-common")
    return std::make_unique<BooleanOperation<BRepAlgoAPI_Common>>();
  if (theId == "fillet")
    return std::make_unique<FilletOperation>();
  if (theId == "mesh")
    return std::make_unique<MeshOperation>();
  if (theId == "check")
    return std::make_unique<CheckOperation>();
  throw std::runtime_error("unknown operation: " + theId);
}

} // namespace billet
