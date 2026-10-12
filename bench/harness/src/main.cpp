// Billet benchmark harness: command-line entry point.
//
// One process runs one job (operation x model x thread mode): setup, warm-up runs, timed runs,
// then the output fingerprint. The runner in bench/runner starts one process per job, so that
// peak memory is measured per job.

#include "fingerprint.hpp"
#include "json_writer.hpp"
#include "operations.hpp"
#include "parts.hpp"
#include "probe.hpp"

#include <BOPAlgo_Options.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_PrinterOStream.hxx>
#include <NCollection_IndexedMap.hxx>
#include <OSD_Parallel.hxx>
#include <OSD_ThreadPool.hxx>
#include <STEPControl_Reader.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>
#include <TopExp.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <psapi.h>
#endif

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

const char* const kHarnessVersion = "0.1.0";

struct MemorySample
{
  long long workingSet     = -1;
  long long peakWorkingSet = -1;
  long long peakCommit     = -1;
};

MemorySample sampleMemory()
{
  MemorySample aSample;
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS aCounters = {};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &aCounters, sizeof(aCounters)))
  {
    aSample.workingSet     = static_cast<long long>(aCounters.WorkingSetSize);
    aSample.peakWorkingSet = static_cast<long long>(aCounters.PeakWorkingSetSize);
    aSample.peakCommit     = static_cast<long long>(aCounters.PeakPagefileUsage);
  }
#endif
  return aSample;
}

// Single-threaded mode turns off the parallel flags, routes OSD_Parallel to a one-thread OCCT pool
// and pins the process to one logical CPU. All-cores mode turns the parallel flags on.
void configureThreads(bool theParallel, int theCpu)
{
  BOPAlgo_Options::SetParallelMode(theParallel);
  if (!theParallel)
  {
    OSD_Parallel::SetUseOcctThreads(true);
    OSD_ThreadPool::DefaultPool(1);
#ifdef _WIN32
    if (theCpu >= 0)
    {
      SetProcessAffinityMask(GetCurrentProcess(), static_cast<DWORD_PTR>(1) << theCpu);
    }
#else
    (void)theCpu;
#endif
  }
}

double median(std::vector<double> theValues)
{
  if (theValues.empty())
  {
    return 0.0;
  }
  std::sort(theValues.begin(), theValues.end());
  const size_t aMid = theValues.size() / 2;
  return theValues.size() % 2 == 1 ? theValues[aMid]
                                   : 0.5 * (theValues[aMid - 1] + theValues[aMid]);
}

void printUsage()
{
  std::cerr << "Usage:\n"
               "  billet-bench run --op <id> --model <file|part-id> [--threads 1|all] [--warmup N] "
               "[--runs N]\n"
               "                   [--cpu N] [--param key=value]... [--save-output <file>] "
               "[--scratch <dir>]\n"
               "                   [--no-validity] [--out <json>]\n"
               "  billet-bench parts            list scripted parts for the rebuild operation\n"
               "  billet-bench ops              list operations\n"
               "  billet-bench edges --model <file> [--out <json>]\n"
               "  billet-bench probe --op fillet|shell --model <file> [--param key=value]... "
               "[--out <json>]\n"
               "                   fillet: radius=R [count=12] [select=largest-solid] [tries=200]\n"
               "                   shell:  thickness=T [select=largest-solid] [tries=20]\n"
               "  billet-bench version\n";
}

void emit(const std::string& theText, const std::string& theOutFile)
{
  if (theOutFile.empty())
  {
    std::cout << theText << std::endl;
    return;
  }
  std::ofstream aStream(theOutFile, std::ios::binary);
  aStream << theText << "\n";
  if (!aStream)
  {
    throw std::runtime_error("cannot write " + theOutFile);
  }
}

std::string describeException()
{
  try
  {
    throw;
  }
  catch (const Standard_Failure& theFailure)
  {
    return std::string(theFailure.ExceptionType()) + ": " + theFailure.what();
  }
  catch (const std::exception& theError)
  {
    return theError.what();
  }
  catch (...)
  {
    return "unknown exception";
  }
}

int runJob(const billet::JobOptions& theOptions,
           int                       theCpu,
           bool                      theValidity,
           const std::string&        theOutFile)
{
  using Clock = std::chrono::steady_clock;
  configureThreads(theOptions.parallel, theCpu);

  billet::JsonWriter aJson;
  aJson.beginObject();
  aJson.key("harness_version").value(kHarnessVersion);
  aJson.key("occt_version").value(OCC_VERSION_COMPLETE);
  aJson.key("op").value(theOptions.op);
  aJson.key("model").value(theOptions.model);
  aJson.key("threads").value(theOptions.parallel ? "all" : "1");
  aJson.key("params");
  aJson.beginObject();
  for (const auto& aParam : theOptions.params)
  {
    aJson.key(aParam.first).value(aParam.second);
  }
  aJson.endObject();
  aJson.key("warmup").value(theOptions.warmup);
  aJson.key("runs").value(theOptions.runs);

  std::string                        aPhase = "setup";
  std::vector<double>                aTimes;
  MemorySample                       aSetupMemory;
  bool                               anOk = false;
  std::string                        anError;
  std::unique_ptr<billet::Operation> anOperation;
  try
  {
    anOperation = billet::createOperation(theOptions.op);
    anOperation->setup(theOptions);
    aSetupMemory = sampleMemory();

    aPhase = "warmup";
    for (int i = 0; i < theOptions.warmup; ++i)
    {
      anOperation->prepareRun();
      anOperation->run();
    }

    aPhase = "run";
    for (int i = 0; i < theOptions.runs; ++i)
    {
      anOperation->prepareRun();
      const Clock::time_point aStart = Clock::now();
      anOperation->run();
      const Clock::time_point anEnd = Clock::now();
      aTimes.push_back(std::chrono::duration<double>(anEnd - aStart).count());
    }
    anOk = true;
  }
  catch (...)
  {
    anError = describeException();
  }
  // Sampled before the fingerprint, so that validity checks do not count toward the job's peak.
  const MemorySample aPeakMemory = sampleMemory();

  billet::Fingerprint aPrint;
  const bool          aHasOutput = !theOptions.outputFile.empty() || theOptions.op == "step-write";
  long long           anOutputSize = -1;
  if (anOk)
  {
    aPhase = "fingerprint";
    try
    {
      aPrint = billet::computeFingerprint(anOperation->result(),
                                          theValidity && anOperation->checkResult(),
                                          theOptions.parallel);
      if (aHasOutput)
      {
        anOutputSize = anOperation->saveOutput(theOptions);
      }
    }
    catch (...)
    {
      anOk    = false;
      anError = describeException();
    }
  }

  aJson.key("status").value(anOk ? "ok" : "error");
  if (!anOk)
  {
    aJson.key("phase").value(aPhase);
    aJson.key("error").value(anError);
  }
  aJson.key("times_s").value(aTimes);
  aJson.key("median_s").value(median(aTimes));
  aJson.key("memory");
  aJson.beginObject();
  aJson.key("setup_working_set_bytes").value(aSetupMemory.workingSet);
  aJson.key("peak_working_set_bytes").value(aPeakMemory.peakWorkingSet);
  aJson.key("peak_commit_bytes").value(aPeakMemory.peakCommit);
  aJson.endObject();
  if (anOk)
  {
    aJson.key("details");
    aJson.beginObject();
    anOperation->describe(aJson);
    aJson.endObject();
    aJson.key("fingerprint");
    billet::writeFingerprint(aJson, aPrint);
    if (aHasOutput)
    {
      aJson.key("output");
      aJson.beginObject();
      aJson.key("file").value(theOptions.outputFile);
      aJson.key("bytes").value(anOutputSize);
      aJson.endObject();
    }
  }
  aJson.endObject();
  emit(aJson.text(), theOutFile);
  return anOk ? 0 : 2;
}

int listEdges(const billet::JobOptions& theOptions, const std::string& theOutFile)
{
  STEPControl_Reader aReader;
  if (aReader.ReadFile(theOptions.model.c_str()) != IFSelect_RetDone)
  {
    throw std::runtime_error("cannot read STEP file: " + theOptions.model);
  }
  aReader.TransferRoots();
  const TopoDS_Shape aShape = aReader.OneShape();

  NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> anEdges;
  TopExp::MapShapes(aShape, TopAbs_EDGE, anEdges);
  billet::JsonWriter aJson;
  aJson.beginArray();
  for (int i = 1; i <= anEdges.Extent(); ++i)
  {
    const TopoDS_Edge& anEdge = TopoDS::Edge(anEdges(i));
    if (BRep_Tool::Degenerated(anEdge))
    {
      continue;
    }
    BRepAdaptor_Curve aCurve(anEdge);
    aJson.beginObject();
    aJson.key("index").value(i);
    aJson.key("type").value(static_cast<int>(aCurve.GetType()));
    aJson.key("length").value(GCPnts_AbscissaPoint::Length(aCurve));
    aJson.endObject();
  }
  aJson.endArray();
  emit(aJson.text(), theOutFile);
  return 0;
}

} // namespace

int main(int argc, char** argv)
{
  // Keep stdout for the JSON report: OCCT's default messenger prints to stdout.
  Message::DefaultMessenger()->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));

  if (argc < 2)
  {
    printUsage();
    return 1;
  }
  const std::string  aCommand = argv[1];
  billet::JobOptions anOptions;
  std::string        anOutFile;
  int                aCpu      = 2;
  bool               aValidity = true;
  try
  {
    for (int i = 2; i < argc; ++i)
    {
      const std::string anArg = argv[i];
      auto              aNext = [&]() -> std::string {
        if (i + 1 >= argc)
        {
          throw std::runtime_error("missing value after " + anArg);
        }
        return argv[++i];
      };
      if (anArg == "--op")
        anOptions.op = aNext();
      else if (anArg == "--model")
        anOptions.model = aNext();
      else if (anArg == "--threads")
      {
        const std::string aValue = aNext();
        if (aValue != "1" && aValue != "all")
        {
          throw std::runtime_error("--threads must be 1 or all");
        }
        anOptions.parallel = aValue == "all";
      }
      else if (anArg == "--warmup")
        anOptions.warmup = std::stoi(aNext());
      else if (anArg == "--runs")
        anOptions.runs = std::stoi(aNext());
      else if (anArg == "--cpu")
        aCpu = std::stoi(aNext());
      else if (anArg == "--param")
      {
        const std::string aPair = aNext();
        const size_t      anEq  = aPair.find('=');
        if (anEq == std::string::npos)
        {
          throw std::runtime_error("--param expects key=value: " + aPair);
        }
        anOptions.params[aPair.substr(0, anEq)] = aPair.substr(anEq + 1);
      }
      else if (anArg == "--save-output")
        anOptions.outputFile = aNext();
      else if (anArg == "--scratch")
        anOptions.scratchDir = aNext();
      else if (anArg == "--no-validity")
        aValidity = false;
      else if (anArg == "--out")
        anOutFile = aNext();
      else
        throw std::runtime_error("unknown argument: " + anArg);
    }

    if (aCommand == "run")
    {
      if (anOptions.op.empty() || anOptions.model.empty())
      {
        throw std::runtime_error("run needs --op and --model");
      }
      return runJob(anOptions, aCpu, aValidity, anOutFile);
    }
    if (aCommand == "parts")
    {
      for (const billet::PartInfo& aPart : billet::scriptedParts())
      {
        std::cout << aPart.id << "\t" << aPart.description << "\n";
      }
      return 0;
    }
    if (aCommand == "ops")
    {
      for (const std::string& anId : billet::operationIds())
      {
        std::cout << anId << "\n";
      }
      return 0;
    }
    if (aCommand == "edges")
    {
      return listEdges(anOptions, anOutFile);
    }
    if (aCommand == "probe")
    {
      if (anOptions.op != "fillet" && anOptions.op != "shell")
      {
        throw std::runtime_error("probe supports --op fillet or --op shell");
      }
      emit(anOptions.op == "fillet" ? billet::probeFillet(anOptions)
                                    : billet::probeShell(anOptions),
           anOutFile);
      return 0;
    }
    if (aCommand == "version")
    {
      std::cout << "billet-bench " << kHarnessVersion << " (OCCT " << OCC_VERSION_COMPLETE << ")\n";
      return 0;
    }
    printUsage();
    return 1;
  }
  catch (...)
  {
    std::cerr << "billet-bench: " << describeException() << "\n";
    return 1;
  }
}
