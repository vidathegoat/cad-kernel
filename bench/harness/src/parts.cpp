// Billet benchmark harness: scripted parts for the "rebuild" operation.

#include "parts.hpp"

#include "modeling.hpp"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepLib.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRep_Builder.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GC_MakeSegment.hxx>
#include <GC_MakeSegment2d.hxx>
#include <Geom2d_Ellipse.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax2d.hxx>
#include <gp_Trsf.hxx>

#include <cmath>
#include <stdexcept>

namespace billet
{

namespace
{

const gp_Dir kDirX(1.0, 0.0, 0.0);
const gp_Dir kDirY(0.0, 1.0, 0.0);
const gp_Dir kDirZ(0.0, 0.0, 1.0);

// The classic OCCT tutorial bottle: profile, mirror, extrude, fillet all edges, fuse the neck,
// shell with the neck top open, and a lofted thread.
TopoDS_Shape makeBottle()
{
  const double aWidth = 50.0, aHeight = 70.0, aThickness = 30.0;

  const gp_Pnt aPnt1(-aWidth / 2.0, 0.0, 0.0);
  const gp_Pnt aPnt2(-aWidth / 2.0, -aThickness / 4.0, 0.0);
  const gp_Pnt aPnt3(0.0, -aThickness / 2.0, 0.0);
  const gp_Pnt aPnt4(aWidth / 2.0, -aThickness / 4.0, 0.0);
  const gp_Pnt aPnt5(aWidth / 2.0, 0.0, 0.0);

  const TopoDS_Edge anEdge1 = BRepBuilderAPI_MakeEdge(GC_MakeSegment(aPnt1, aPnt2).Value()).Edge();
  const TopoDS_Edge anEdge2 =
    BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(aPnt2, aPnt3, aPnt4).Value()).Edge();
  const TopoDS_Edge anEdge3 = BRepBuilderAPI_MakeEdge(GC_MakeSegment(aPnt4, aPnt5).Value()).Edge();
  const TopoDS_Wire aHalf   = BRepBuilderAPI_MakeWire(anEdge1, anEdge2, anEdge3).Wire();

  gp_Trsf aMirror;
  aMirror.SetMirror(gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), kDirX));
  const TopoDS_Wire aMirrored = TopoDS::Wire(BRepBuilderAPI_Transform(aHalf, aMirror).Shape());

  BRepBuilderAPI_MakeWire aProfile;
  aProfile.Add(aHalf);
  aProfile.Add(aMirrored);
  TopoDS_Shape aBody =
    extrude(planarFace(aProfile.Wire(), "bottle sketch"), 0.0, 0.0, aHeight, "bottle extrude");

  aBody = filletEdges(aBody,
                      aThickness / 12.0,
                      selectEdges(aBody, [](const TopoDS_Edge&) { return true; }),
                      "bottle body fillet");

  const double aNeckRadius = aThickness / 4.0, aNeckHeight = aHeight / 10.0;
  const gp_Ax2 aNeckAxis(gp_Pnt(0.0, 0.0, aHeight), kDirZ);
  aBody = fuseAll(aBody,
                  {makeCylinder(aNeckAxis.Location(), kDirZ, aNeckRadius, aNeckHeight)},
                  "bottle neck");

  aBody = thickSolid(aBody,
                     {planarFaceAtZ(aBody, aHeight + aNeckHeight, "bottle neck top")},
                     -aThickness / 50.0,
                     "bottle shell");

  // Thread: two elliptic arcs on cylinders around the neck, lofted into a solid.
  occ::handle<Geom_CylindricalSurface> aCyl1 =
    new Geom_CylindricalSurface(aNeckAxis, aNeckRadius * 0.99);
  occ::handle<Geom_CylindricalSurface> aCyl2 =
    new Geom_CylindricalSurface(aNeckAxis, aNeckRadius * 1.05);
  const gp_Ax2d               aThreadAxis(gp_Pnt2d(2.0 * kPi, aNeckHeight / 2.0),
                            gp_Dir2d(2.0 * kPi, aNeckHeight / 4.0));
  const double                aMajor = 2.0 * kPi, aMinor = aNeckHeight / 10.0;
  occ::handle<Geom2d_Ellipse> anEllipse1  = new Geom2d_Ellipse(aThreadAxis, aMajor, aMinor);
  occ::handle<Geom2d_Ellipse> anEllipse2  = new Geom2d_Ellipse(aThreadAxis, aMajor, aMinor / 4.0);
  occ::handle<Geom2d_TrimmedCurve> anArc1 = new Geom2d_TrimmedCurve(anEllipse1, 0.0, kPi);
  occ::handle<Geom2d_TrimmedCurve> anArc2 = new Geom2d_TrimmedCurve(anEllipse2, 0.0, kPi);
  occ::handle<Geom2d_TrimmedCurve> aSegment =
    GC_MakeSegment2d(anEllipse1->Value(0.0), anEllipse1->Value(kPi)).Value();

  TopoDS_Wire aThreadWire1 =
    BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(anArc1, aCyl1).Edge(),
                            BRepBuilderAPI_MakeEdge(aSegment, aCyl1).Edge())
      .Wire();
  TopoDS_Wire aThreadWire2 =
    BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(anArc2, aCyl2).Edge(),
                            BRepBuilderAPI_MakeEdge(aSegment, aCyl2).Edge())
      .Wire();
  BRepLib::BuildCurves3d(aThreadWire1);
  BRepLib::BuildCurves3d(aThreadWire2);

  BRepOffsetAPI_ThruSections aLoft(true);
  aLoft.AddWire(aThreadWire1);
  aLoft.AddWire(aThreadWire2);
  aLoft.CheckCompatibility(false);
  aLoft.Build();
  if (!aLoft.IsDone())
  {
    throw std::runtime_error("bottle thread: loft failed");
  }

  TopoDS_Compound aResult;
  BRep_Builder    aBuilder;
  aBuilder.MakeCompound(aResult);
  aBuilder.Add(aResult, aBody);
  aBuilder.Add(aResult, aLoft.Shape());
  return aResult;
}

// L-bracket: extruded L profile, concentric bend fillets, hole cuts with a 2 x 3 pattern,
// chamfered base holes and rounded base ends.
TopoDS_Shape makeBracket()
{
  const double      aLength = 80.0, aHeight = 60.0, aThick = 8.0, aWidth = 40.0;
  const TopoDS_Face aSketch = planarFace(polygonWire({gp_Pnt(0.0, 0.0, 0.0),
                                                      gp_Pnt(aLength, 0.0, 0.0),
                                                      gp_Pnt(aLength, 0.0, aThick),
                                                      gp_Pnt(aThick, 0.0, aThick),
                                                      gp_Pnt(aThick, 0.0, aHeight),
                                                      gp_Pnt(0.0, 0.0, aHeight)}),
                                         "bracket sketch");
  TopoDS_Shape      aBody   = extrude(aSketch, 0.0, aWidth, 0.0, "bracket extrude");

  auto aCornerAt = [&](double theX, double theZ) {
    return selectEdges(aBody, [&](const TopoDS_Edge& theEdge) {
      const gp_Pnt aMid = edgeMidpoint(theEdge);
      return isLineAlong(theEdge, kDirY) && near(aMid.X(), theX) && near(aMid.Z(), theZ);
    });
  };
  aBody = filletEdges(aBody, 6.0, aCornerAt(aThick, aThick), "bracket inner bend fillet");
  aBody = filletEdges(aBody, 14.0, aCornerAt(0.0, 0.0), "bracket outer bend fillet");

  std::vector<TopoDS_Shape> aHoles;
  for (double aX : {35.0, 65.0})
  {
    aHoles.push_back(makeCylinder(gp_Pnt(aX, aWidth / 2.0, -1.0), kDirZ, 4.0, aThick + 2.0));
  }
  for (double aY : {12.0, 28.0})
  {
    for (double aZ : {26.0, 38.0, 50.0})
    {
      aHoles.push_back(makeCylinder(gp_Pnt(-1.0, aY, aZ), kDirX, 2.5, aThick + 2.0));
    }
  }
  aBody = cutAll(aBody, aHoles, "bracket holes");

  aBody = chamferEdges(aBody,
                       0.8,
                       selectEdges(aBody,
                                   [&](const TopoDS_Edge& theEdge) {
                                     return isCircle(theEdge, 4.0)
                                            && near(circleCenter(theEdge).Z(), aThick);
                                   }),
                       "bracket hole chamfers");

  aBody = filletEdges(aBody,
                      3.0,
                      selectEdges(aBody,
                                  [&](const TopoDS_Edge& theEdge) {
                                    return isLineAlong(theEdge, kDirZ)
                                           && near(edgeMidpoint(theEdge).X(), aLength);
                                  }),
                      "bracket end fillets");
  return aBody;
}

// Enclosure: rounded-rectangle extrusion, bottom fillet, thin-wall shell open at the top,
// fused screw bosses, screw holes, a vent slot pattern and boss root fillets.
TopoDS_Shape makeEnclosure()
{
  const double aLx = 120.0, aLy = 80.0, aH = 40.0, aWall = 2.5;
  TopoDS_Shape aBody =
    extrude(planarFace(roundedRectangleWire(aLx, aLy, 10.0, 0.0), "enclosure sketch"),
            0.0,
            0.0,
            aH,
            "enclosure extrude");

  aBody = filletEdges(
    aBody,
    4.0,
    selectEdges(aBody,
                [](const TopoDS_Edge& theEdge) { return near(edgeMidpoint(theEdge).Z(), 0.0); }),
    "enclosure bottom fillet");

  aBody =
    thickSolid(aBody, {planarFaceAtZ(aBody, aH, "enclosure top face")}, -aWall, "enclosure shell");

  const double              aBossBottom = aWall - 0.5, aBossTop = aH - 5.0;
  std::vector<TopoDS_Shape> aBosses, aScrewHoles;
  for (double aSx : {-1.0, 1.0})
  {
    for (double aSy : {-1.0, 1.0})
    {
      const double aX = aSx * (aLx / 2.0 - 12.0), aY = aSy * (aLy / 2.0 - 12.0);
      aBosses.push_back(
        makeCylinder(gp_Pnt(aX, aY, aBossBottom), kDirZ, 4.0, aBossTop - aBossBottom));
      aScrewHoles.push_back(
        makeCylinder(gp_Pnt(aX, aY, aWall + 3.0), kDirZ, 1.5, aBossTop - aWall - 2.0));
    }
  }
  aBody = fuseAll(aBody, aBosses, "enclosure bosses");
  aBody = cutAll(aBody, aScrewHoles, "enclosure screw holes");

  std::vector<TopoDS_Shape> aSlots;
  for (int i = 0; i < 8; ++i)
  {
    aSlots.push_back(
      makeBox(gp_Pnt(-40.0 + 10.0 * i, -aLy / 2.0 - 1.0, 10.0), 4.0, aWall + 2.0, 16.0));
  }
  aBody = cutAll(aBody, aSlots, "enclosure vent slots");

  aBody = filletEdges(aBody,
                      1.0,
                      selectEdges(aBody,
                                  [&](const TopoDS_Edge& theEdge) {
                                    return isCircle(theEdge, 4.0)
                                           && near(circleCenter(theEdge).Z(), aWall);
                                  }),
                      "enclosure boss root fillets");
  return aBody;
}

// Flange: revolved hub-and-disk section, hub fillet, circular bolt hole pattern,
// chamfered bore and rim.
TopoDS_Shape makeFlange()
{
  const TopoDS_Face aSketch = planarFace(polygonWire({gp_Pnt(10.0, 0.0, 0.0),
                                                      gp_Pnt(60.0, 0.0, 0.0),
                                                      gp_Pnt(60.0, 0.0, 12.0),
                                                      gp_Pnt(25.0, 0.0, 12.0),
                                                      gp_Pnt(25.0, 0.0, 40.0),
                                                      gp_Pnt(10.0, 0.0, 40.0)}),
                                         "flange sketch");
  TopoDS_Shape      aBody   = revolveFull(aSketch, "flange revolve");

  aBody = filletEdges(aBody,
                      4.0,
                      selectEdges(aBody,
                                  [](const TopoDS_Edge& theEdge) {
                                    return isCircle(theEdge, 25.0)
                                           && near(circleCenter(theEdge).Z(), 12.0);
                                  }),
                      "flange hub fillet");

  std::vector<TopoDS_Shape> aBoltHoles;
  for (int i = 0; i < 8; ++i)
  {
    const double anAngle = 2.0 * kPi * i / 8.0;
    aBoltHoles.push_back(
      makeCylinder(gp_Pnt(45.0 * std::cos(anAngle), 45.0 * std::sin(anAngle), -1.0),
                   kDirZ,
                   4.5,
                   14.0));
  }
  aBody = cutAll(aBody, aBoltHoles, "flange bolt holes");

  aBody = chamferEdges(
    aBody,
    1.0,
    selectEdges(aBody, [](const TopoDS_Edge& theEdge) { return isCircle(theEdge, 10.0); }),
    "flange bore chamfers");
  aBody = chamferEdges(aBody,
                       1.0,
                       selectEdges(aBody,
                                   [](const TopoDS_Edge& theEdge) {
                                     return isCircle(theEdge, 60.0)
                                            && near(circleCenter(theEdge).Z(), 12.0);
                                   }),
                       "flange rim chamfer");
  return aBody;
}

// Heatsink: comb profile (base and 12 fins) extruded, fillets on 24 fin roots and 24 fin tips,
// a mounting hole pattern and a chamfered base.
TopoDS_Shape makeHeatsink()
{
  const int    aNbFins = 12;
  const double aBaseX = 100.0, aBaseZ = 6.0, aDepth = 60.0, aFinT = 1.5, aFinH = 30.0, aPitch = 8.0,
               aX0 = 5.0;

  std::vector<gp_Pnt> aPoints = {gp_Pnt(0.0, 0.0, 0.0),
                                 gp_Pnt(aBaseX, 0.0, 0.0),
                                 gp_Pnt(aBaseX, 0.0, aBaseZ)};
  for (int i = aNbFins - 1; i >= 0; --i)
  {
    const double aLeft = aX0 + i * aPitch, aRight = aLeft + aFinT;
    aPoints.push_back(gp_Pnt(aRight, 0.0, aBaseZ));
    aPoints.push_back(gp_Pnt(aRight, 0.0, aBaseZ + aFinH));
    aPoints.push_back(gp_Pnt(aLeft, 0.0, aBaseZ + aFinH));
    aPoints.push_back(gp_Pnt(aLeft, 0.0, aBaseZ));
  }
  aPoints.push_back(gp_Pnt(0.0, 0.0, aBaseZ));
  TopoDS_Shape aBody = extrude(planarFace(polygonWire(aPoints), "heatsink sketch"),
                               0.0,
                               aDepth,
                               0.0,
                               "heatsink extrude");

  const double aFinsMinX = aX0 - 0.1, aFinsMaxX = aX0 + (aNbFins - 1) * aPitch + aFinT + 0.1;
  aBody = filletEdges(aBody,
                      0.8,
                      selectEdges(aBody,
                                  [&](const TopoDS_Edge& theEdge) {
                                    const gp_Pnt aMid = edgeMidpoint(theEdge);
                                    return isLineAlong(theEdge, kDirY) && near(aMid.Z(), aBaseZ)
                                           && aMid.X() > aFinsMinX && aMid.X() < aFinsMaxX;
                                  }),
                      "heatsink fin root fillets");
  aBody = filletEdges(aBody,
                      0.5,
                      selectEdges(aBody,
                                  [&](const TopoDS_Edge& theEdge) {
                                    return isLineAlong(theEdge, kDirY)
                                           && near(edgeMidpoint(theEdge).Z(), aBaseZ + aFinH);
                                  }),
                      "heatsink fin tip fillets");

  std::vector<TopoDS_Shape> aHoles;
  for (double aX : {aX0 / 2.0, aBaseX - (aBaseX - aFinsMaxX) / 2.0})
  {
    for (double aY : {12.0, 48.0})
    {
      aHoles.push_back(makeCylinder(gp_Pnt(aX, aY, -1.0), kDirZ, 1.6, aBaseZ + 2.0));
    }
  }
  aBody = cutAll(aBody, aHoles, "heatsink mounting holes");

  aBody = chamferEdges(aBody,
                       0.5,
                       selectEdges(aBody,
                                   [](const TopoDS_Edge& theEdge) {
                                     return near(edgeMidpoint(theEdge).Z(), 0.0)
                                            && (isLineAlong(theEdge, kDirX)
                                                || isLineAlong(theEdge, kDirY));
                                   }),
                       "heatsink base chamfer");
  return aBody;
}

// Manifold block: rounded block, intersecting bores (main, three cross bores, vertical port),
// counterbores, chamfered outer edges and counterbore rims.
TopoDS_Shape makeManifold()
{
  const double aX = 60.0, aY = 40.0, aZ = 40.0;
  TopoDS_Shape aBody = makeBox(gp_Pnt(0.0, 0.0, 0.0), aX, aY, aZ);

  aBody = filletEdges(
    aBody,
    3.0,
    selectEdges(aBody, [](const TopoDS_Edge& theEdge) { return isLineAlong(theEdge, kDirZ); }),
    "manifold corner fillets");

  std::vector<TopoDS_Shape> aBores = {
    makeCylinder(gp_Pnt(-1.0, 20.0, 20.0), kDirX, 6.0, aX + 2.0),
    makeCylinder(gp_Pnt(30.0, 20.0, 20.0), kDirZ, 5.0, aZ - 20.0 + 1.0)};
  for (double aCrossX : {15.0, 30.0, 45.0})
  {
    aBores.push_back(makeCylinder(gp_Pnt(aCrossX, -1.0, 20.0), kDirY, 4.0, 21.0));
  }
  aBody = cutAll(aBody, aBores, "manifold bores");

  aBody = cutAll(aBody,
                 {makeCylinder(gp_Pnt(-1.0, 20.0, 20.0), kDirX, 9.0, 4.0),
                  makeCylinder(gp_Pnt(aX - 3.0, 20.0, 20.0), kDirX, 9.0, 4.0),
                  makeCylinder(gp_Pnt(30.0, 20.0, aZ - 3.0), kDirZ, 8.0, 4.0)},
                 "manifold counterbores");

  aBody =
    chamferEdges(aBody,
                 1.0,
                 selectEdges(aBody,
                             [&](const TopoDS_Edge& theEdge) {
                               const double aMidZ = edgeMidpoint(theEdge).Z();
                               return (near(aMidZ, 0.0) || near(aMidZ, aZ))
                                      && (isCircle(theEdge, 3.0) || isLineAlong(theEdge, kDirX)
                                          || isLineAlong(theEdge, kDirY));
                             }),
                 "manifold outer chamfers");

  aBody = chamferEdges(aBody,
                       0.5,
                       selectEdges(aBody,
                                   [&](const TopoDS_Edge& theEdge) {
                                     if (!isCircle(theEdge, 9.0))
                                     {
                                       return false;
                                     }
                                     const double aCenterX = circleCenter(theEdge).X();
                                     return near(aCenterX, 0.0) || near(aCenterX, aX);
                                   }),
                       "manifold counterbore chamfers");
  return aBody;
}

using PartBuilder = TopoDS_Shape (*)();

struct PartEntry
{
  PartInfo    info;
  PartBuilder builder;
};

const std::vector<PartEntry>& partEntries()
{
  static const std::vector<PartEntry> kEntries = {
    {{"bottle", "OCCT tutorial bottle: extrude, fillet all edges, fuse, shell, lofted thread"},
     &makeBottle},
    {{"bracket", "L-bracket: extrude, bend fillets, hole pattern cut, chamfers, end fillets"},
     &makeBracket},
    {{"enclosure",
      "Enclosure: extrude, fillet, thin-wall shell, fused bosses, slot pattern, root fillets"},
     &makeEnclosure},
    {{"flange", "Flange: revolve, hub fillet, circular bolt pattern, bore and rim chamfers"},
     &makeFlange},
    {{"heatsink", "Heatsink: comb extrude, 48 small fillets, hole pattern, base chamfer"},
     &makeHeatsink},
    {{"manifold", "Manifold: intersecting bores, counterbores, tangent-chain and rim chamfers"},
     &makeManifold},
  };
  return kEntries;
}

} // namespace

const std::vector<PartInfo>& scriptedParts()
{
  static const std::vector<PartInfo> kInfos = [] {
    std::vector<PartInfo> aResult;
    for (const PartEntry& anEntry : partEntries())
    {
      aResult.push_back(anEntry.info);
    }
    return aResult;
  }();
  return kInfos;
}

TopoDS_Shape buildPart(const std::string& theId)
{
  for (const PartEntry& anEntry : partEntries())
  {
    if (anEntry.info.id == theId)
    {
      return anEntry.builder();
    }
  }
  throw std::runtime_error("unknown scripted part: " + theId);
}

} // namespace billet
