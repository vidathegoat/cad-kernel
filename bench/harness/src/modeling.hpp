// Billet benchmark harness: modeling helpers shared by the scripted parts and operations.
// Every helper throws std::runtime_error with the given context when an algorithm fails.

#pragma once

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <functional>
#include <string>
#include <vector>

namespace billet
{

constexpr double kPi = 3.14159265358979323846;

// Sketch helpers (wires and faces in a plane).
TopoDS_Wire polygonWire(const std::vector<gp_Pnt>& thePoints);
TopoDS_Wire roundedRectangleWire(double theSizeX, double theSizeY, double theRadius, double theZ);
TopoDS_Face planarFace(const TopoDS_Wire& theWire, const std::string& theContext);

// Primitive tool bodies.
TopoDS_Shape makeBox(const gp_Pnt& theCorner, double theDx, double theDy, double theDz);
TopoDS_Shape makeCylinder(const gp_Pnt& theBase,
                          const gp_Dir& theAxis,
                          double        theRadius,
                          double        theHeight);

// Feature operations.
TopoDS_Shape extrude(const TopoDS_Face& theFace,
                     double             theDx,
                     double             theDy,
                     double             theDz,
                     const std::string& theContext);
TopoDS_Shape revolveFull(const TopoDS_Face& theFace, const std::string& theContext);
TopoDS_Shape fuseAll(const TopoDS_Shape&              theObject,
                     const std::vector<TopoDS_Shape>& theTools,
                     const std::string&               theContext);
TopoDS_Shape cutAll(const TopoDS_Shape&              theObject,
                    const std::vector<TopoDS_Shape>& theTools,
                    const std::string&               theContext);
TopoDS_Shape filletEdges(const TopoDS_Shape&             theShape,
                         double                          theRadius,
                         const std::vector<TopoDS_Edge>& theEdges,
                         const std::string&              theContext);
TopoDS_Shape chamferEdges(const TopoDS_Shape&             theShape,
                          double                          theDistance,
                          const std::vector<TopoDS_Edge>& theEdges,
                          const std::string&              theContext);
TopoDS_Shape thickSolid(const TopoDS_Shape&             theShape,
                        const std::vector<TopoDS_Face>& theOpenFaces,
                        double                          theOffset,
                        const std::string&              theContext);

// Geometric selection. Results follow TopExp::MapShapes order, so they are deterministic.
using EdgePredicate = std::function<bool(const TopoDS_Edge&)>;
std::vector<TopoDS_Edge> selectEdges(const TopoDS_Shape&  theShape,
                                     const EdgePredicate& thePredicate);

bool   isLineAlong(const TopoDS_Edge& theEdge, const gp_Dir& theDir);
bool   isCircle(const TopoDS_Edge& theEdge, double theRadius);
gp_Pnt edgeMidpoint(const TopoDS_Edge& theEdge);
gp_Pnt circleCenter(const TopoDS_Edge& theEdge);
bool   near(double theA, double theB, double theTol = 1.0e-4);

// Planar face whose plane passes through the given Z level with a normal along Z, largest first.
TopoDS_Face planarFaceAtZ(const TopoDS_Shape& theShape, double theZ, const std::string& theContext);

} // namespace billet
