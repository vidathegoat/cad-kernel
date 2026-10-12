// Billet benchmark harness: output fingerprint used to prove that results are unchanged.

#pragma once

#include "json_writer.hpp"

#include <TopoDS_Shape.hxx>

#include <string>

namespace billet
{

struct Fingerprint
{
  double    volume   = 0.0;
  double    area     = 0.0;
  double    bbox[6]  = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0}; // xmin, ymin, zmin, xmax, ymax, zmax
  bool      bboxVoid = true;
  int       solids = 0, shells = 0, faces = 0, edges = 0, vertices = 0;
  long long triangles = 0, nodes = 0;
  bool      hasValidity = false;
  bool      valid       = false;
};

// Computes geometric properties and topology counts. Validity (BRepCheck) is computed only when
// requested, because it can take long on large outputs.
Fingerprint computeFingerprint(const TopoDS_Shape& theShape,
                               bool                theCheckValidity,
                               bool                theParallelCheck);

void writeFingerprint(JsonWriter& theWriter, const Fingerprint& theFingerprint);

// Writes the shape as a BREP file (triangulation included only when requested). Returns the file
// size, or -1 on failure.
long long writeBrep(const TopoDS_Shape& theShape,
                    const std::string&  thePath,
                    bool                theWithTriangles);

} // namespace billet
