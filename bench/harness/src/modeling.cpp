// Billet benchmark harness: modeling helpers.

#include "modeling.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GC_MakeSegment.hxx>
#include <GProp_GProps.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <NCollection_IndexedMap.hxx>
#include <NCollection_List.hxx>
#include <TopExp.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <stdexcept>

namespace billet
{

namespace
{

[[noreturn]] void fail(const std::string& theContext, const std::string& theWhat)
{
  throw std::runtime_error(theContext + ": " + theWhat);
}

template <class Algo>
TopoDS_Shape runBoolean(const TopoDS_Shape&              theObject,
                        const std::vector<TopoDS_Shape>& theTools,
                        const std::string&               theContext)
{
  if (theTools.empty())
  {
    fail(theContext, "no tool shapes");
  }
  NCollection_List<TopoDS_Shape> anArgs, aTools;
  anArgs.Append(theObject);
  for (const TopoDS_Shape& aTool : theTools)
  {
    aTools.Append(aTool);
  }
  Algo anAlgo;
  anAlgo.SetArguments(anArgs);
  anAlgo.SetTools(aTools);
  anAlgo.Build();
  if (!anAlgo.IsDone() || anAlgo.HasErrors())
  {
    fail(theContext, "boolean operation failed");
  }
  return anAlgo.Shape();
}

} // namespace

bool near(double theA, double theB, double theTol)
{
  return std::abs(theA - theB) <= theTol;
}

TopoDS_Wire polygonWire(const std::vector<gp_Pnt>& thePoints)
{
  BRepBuilderAPI_MakeWire aMaker;
  for (size_t i = 0; i < thePoints.size(); ++i)
  {
    const gp_Pnt& aP1 = thePoints[i];
    const gp_Pnt& aP2 = thePoints[(i + 1) % thePoints.size()];
    aMaker.Add(BRepBuilderAPI_MakeEdge(aP1, aP2).Edge());
  }
  if (!aMaker.IsDone())
  {
    fail("polygon sketch", "wire construction failed");
  }
  return aMaker.Wire();
}

TopoDS_Wire roundedRectangleWire(double theSizeX, double theSizeY, double theRadius, double theZ)
{
  const double aX = theSizeX / 2.0, aY = theSizeY / 2.0, aR = theRadius;
  // Corner arcs are built through their midpoints; c = R * (1 - 1/sqrt(2)).
  const double            aC = aR * (1.0 - 1.0 / std::sqrt(2.0));
  BRepBuilderAPI_MakeWire aMaker;
  auto                    aSegment = [&](const gp_Pnt& theP1, const gp_Pnt& theP2) {
    aMaker.Add(BRepBuilderAPI_MakeEdge(GC_MakeSegment(theP1, theP2).Value()).Edge());
  };
  auto anArc = [&](const gp_Pnt& theP1, const gp_Pnt& theMid, const gp_Pnt& theP2) {
    aMaker.Add(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(theP1, theMid, theP2).Value()).Edge());
  };
  aSegment(gp_Pnt(-aX + aR, -aY, theZ), gp_Pnt(aX - aR, -aY, theZ));
  anArc(gp_Pnt(aX - aR, -aY, theZ), gp_Pnt(aX - aC, -aY + aC, theZ), gp_Pnt(aX, -aY + aR, theZ));
  aSegment(gp_Pnt(aX, -aY + aR, theZ), gp_Pnt(aX, aY - aR, theZ));
  anArc(gp_Pnt(aX, aY - aR, theZ), gp_Pnt(aX - aC, aY - aC, theZ), gp_Pnt(aX - aR, aY, theZ));
  aSegment(gp_Pnt(aX - aR, aY, theZ), gp_Pnt(-aX + aR, aY, theZ));
  anArc(gp_Pnt(-aX + aR, aY, theZ), gp_Pnt(-aX + aC, aY - aC, theZ), gp_Pnt(-aX, aY - aR, theZ));
  aSegment(gp_Pnt(-aX, aY - aR, theZ), gp_Pnt(-aX, -aY + aR, theZ));
  anArc(gp_Pnt(-aX, -aY + aR, theZ), gp_Pnt(-aX + aC, -aY + aC, theZ), gp_Pnt(-aX + aR, -aY, theZ));
  if (!aMaker.IsDone())
  {
    fail("rounded rectangle sketch", "wire construction failed");
  }
  return aMaker.Wire();
}

TopoDS_Face planarFace(const TopoDS_Wire& theWire, const std::string& theContext)
{
  BRepBuilderAPI_MakeFace aMaker(theWire, true);
  if (!aMaker.IsDone())
  {
    fail(theContext, "face construction failed");
  }
  return aMaker.Face();
}

TopoDS_Shape makeBox(const gp_Pnt& theCorner, double theDx, double theDy, double theDz)
{
  return BRepPrimAPI_MakeBox(theCorner, theDx, theDy, theDz).Shape();
}

TopoDS_Shape makeCylinder(const gp_Pnt& theBase,
                          const gp_Dir& theAxis,
                          double        theRadius,
                          double        theHeight)
{
  return BRepPrimAPI_MakeCylinder(gp_Ax2(theBase, theAxis), theRadius, theHeight).Shape();
}

TopoDS_Shape extrude(const TopoDS_Face& theFace,
                     double             theDx,
                     double             theDy,
                     double             theDz,
                     const std::string& theContext)
{
  BRepPrimAPI_MakePrism aMaker(theFace, gp_Vec(theDx, theDy, theDz));
  if (!aMaker.IsDone())
  {
    fail(theContext, "extrusion failed");
  }
  return aMaker.Shape();
}

TopoDS_Shape revolveFull(const TopoDS_Face& theFace, const std::string& theContext)
{
  BRepPrimAPI_MakeRevol aMaker(theFace, gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)));
  if (!aMaker.IsDone())
  {
    fail(theContext, "revolution failed");
  }
  return aMaker.Shape();
}

TopoDS_Shape fuseAll(const TopoDS_Shape&              theObject,
                     const std::vector<TopoDS_Shape>& theTools,
                     const std::string&               theContext)
{
  return runBoolean<BRepAlgoAPI_Fuse>(theObject, theTools, theContext);
}

TopoDS_Shape cutAll(const TopoDS_Shape&              theObject,
                    const std::vector<TopoDS_Shape>& theTools,
                    const std::string&               theContext)
{
  return runBoolean<BRepAlgoAPI_Cut>(theObject, theTools, theContext);
}

TopoDS_Shape filletEdges(const TopoDS_Shape&             theShape,
                         double                          theRadius,
                         const std::vector<TopoDS_Edge>& theEdges,
                         const std::string&              theContext)
{
  if (theEdges.empty())
  {
    fail(theContext, "no edges selected");
  }
  BRepFilletAPI_MakeFillet aMaker(theShape);
  for (const TopoDS_Edge& anEdge : theEdges)
  {
    aMaker.Add(theRadius, anEdge);
  }
  aMaker.Build();
  if (!aMaker.IsDone())
  {
    fail(theContext, "fillet failed");
  }
  return aMaker.Shape();
}

TopoDS_Shape chamferEdges(const TopoDS_Shape&             theShape,
                          double                          theDistance,
                          const std::vector<TopoDS_Edge>& theEdges,
                          const std::string&              theContext)
{
  if (theEdges.empty())
  {
    fail(theContext, "no edges selected");
  }
  BRepFilletAPI_MakeChamfer aMaker(theShape);
  for (const TopoDS_Edge& anEdge : theEdges)
  {
    aMaker.Add(theDistance, anEdge);
  }
  aMaker.Build();
  if (!aMaker.IsDone())
  {
    fail(theContext, "chamfer failed");
  }
  return aMaker.Shape();
}

TopoDS_Shape thickSolid(const TopoDS_Shape&             theShape,
                        const std::vector<TopoDS_Face>& theOpenFaces,
                        double                          theOffset,
                        const std::string&              theContext)
{
  NCollection_List<TopoDS_Shape> aFaces;
  for (const TopoDS_Face& aFace : theOpenFaces)
  {
    aFaces.Append(aFace);
  }
  BRepOffsetAPI_MakeThickSolid aMaker;
  aMaker.MakeThickSolidByJoin(theShape, aFaces, theOffset, 1.0e-3);
  aMaker.Build();
  if (!aMaker.IsDone())
  {
    fail(theContext, "shell (thick solid) failed");
  }
  return aMaker.Shape();
}

std::vector<TopoDS_Edge> selectEdges(const TopoDS_Shape&  theShape,
                                     const EdgePredicate& thePredicate)
{
  NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> anEdges;
  TopExp::MapShapes(theShape, TopAbs_EDGE, anEdges);
  std::vector<TopoDS_Edge> aResult;
  for (int i = 1; i <= anEdges.Extent(); ++i)
  {
    const TopoDS_Edge& anEdge = TopoDS::Edge(anEdges(i));
    if (!BRep_Tool::Degenerated(anEdge) && thePredicate(anEdge))
    {
      aResult.push_back(anEdge);
    }
  }
  return aResult;
}

bool isLineAlong(const TopoDS_Edge& theEdge, const gp_Dir& theDir)
{
  BRepAdaptor_Curve aCurve(theEdge);
  return aCurve.GetType() == GeomAbs_Line && aCurve.Line().Direction().IsParallel(theDir, 1.0e-6);
}

bool isCircle(const TopoDS_Edge& theEdge, double theRadius)
{
  BRepAdaptor_Curve aCurve(theEdge);
  return aCurve.GetType() == GeomAbs_Circle && near(aCurve.Circle().Radius(), theRadius);
}

gp_Pnt edgeMidpoint(const TopoDS_Edge& theEdge)
{
  BRepAdaptor_Curve aCurve(theEdge);
  return aCurve.Value(0.5 * (aCurve.FirstParameter() + aCurve.LastParameter()));
}

gp_Pnt circleCenter(const TopoDS_Edge& theEdge)
{
  BRepAdaptor_Curve aCurve(theEdge);
  return aCurve.Circle().Location();
}

TopoDS_Face planarFaceAtZ(const TopoDS_Shape& theShape, double theZ, const std::string& theContext)
{
  NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> aFaces;
  TopExp::MapShapes(theShape, TopAbs_FACE, aFaces);
  TopoDS_Face aBest;
  double      aBestArea = -1.0;
  for (int i = 1; i <= aFaces.Extent(); ++i)
  {
    const TopoDS_Face&  aFace = TopoDS::Face(aFaces(i));
    BRepAdaptor_Surface aSurface(aFace, false);
    if (aSurface.GetType() != GeomAbs_Plane)
    {
      continue;
    }
    const gp_Pln aPlane = aSurface.Plane();
    if (!aPlane.Axis().Direction().IsParallel(gp_Dir(0.0, 0.0, 1.0), 1.0e-6)
        || !near(aPlane.Location().Z(), theZ))
    {
      continue;
    }
    GProp_GProps aProps;
    BRepGProp::SurfaceProperties(aFace, aProps);
    if (aProps.Mass() > aBestArea)
    {
      aBestArea = aProps.Mass();
      aBest     = aFace;
    }
  }
  if (aBest.IsNull())
  {
    fail(theContext, "no planar face at the requested height");
  }
  return aBest;
}

} // namespace billet
