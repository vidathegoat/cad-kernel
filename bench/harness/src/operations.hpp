// Billet benchmark harness: benchmarked operations.

#pragma once

#include "json_writer.hpp"

#include <TopoDS_Shape.hxx>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace billet
{

struct JobOptions
{
  std::string                        op;    // operation id, for example "bool-fuse"
  std::string                        model; // STEP file, or a scripted part id for "rebuild"
  bool                               parallel = false;
  int                                warmup   = 1;
  int                                runs     = 5;
  std::map<std::string, std::string> params; // operation parameters, from --param key=value
  std::string outputFile; // where to save the result (BREP, or STEP for step-write)
  std::string scratchDir; // folder for temporary files
  std::string inputFile;  // optional BREP copy of the model, read instead of the STEP file in setup
};

// One benchmarked operation. setup() and prepareRun() are not timed; run() is.
class Operation
{
public:
  virtual ~Operation() = default;

  virtual void setup(const JobOptions& theOptions) = 0;

  virtual void prepareRun() {}

  virtual void run() = 0;

  // Shape produced by the last run, used for the fingerprint.
  virtual TopoDS_Shape result() const = 0;

  // Saves the output of the last run to theOptions.outputFile and returns its size (-1 on failure).
  virtual long long saveOutput(const JobOptions& theOptions) const;

  // Operation-specific details for the JSON report (resolved parameters, counts).
  virtual void describe(JsonWriter& theWriter) const { (void)theWriter; }

  // Whether the result should be checked with BRepCheck_Analyzer for the fingerprint.
  virtual bool checkResult() const { return true; }
};

// Ids of all supported operations.
const std::vector<std::string>& operationIds();

// Reads a STEP or BREP file (by extension) and applies a selection: "all", "largest-solid" or
// "solid:<n>" (1-based).
TopoDS_Shape loadModelShape(const std::string& thePath, const std::string& theSelection);

// Reads a STEP file, or a BREP file when the extension is .brep (text) or .bbrep (binary, exact).
TopoDS_Shape loadShapeFile(const std::string& thePath);

// Creates the operation, or throws std::runtime_error for an unknown id.
std::unique_ptr<Operation> createOperation(const std::string& theId);

} // namespace billet
