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
#include <BinTools.hxx>
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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

const char* const kHarnessVersion = "0.2.0";

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

#ifdef _WIN32
// Layout of SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION (winternl.h), one entry per logical CPU,
// as returned by NtQuerySystemInformation(SystemProcessorPerformanceInformation).
struct ProcessorTimes
{
  LARGE_INTEGER Idle;
  LARGE_INTEGER Kernel; // includes idle, DPC and interrupt time
  LARGE_INTEGER User;
  LARGE_INTEGER Dpc;
  LARGE_INTEGER Interrupt;
  ULONG         InterruptCount;
};
#endif

// CPU time of this process and busy time of every logical CPU (all processes), in seconds.
struct CpuSample
{
  double              processUser   = 0.0;
  double              processKernel = 0.0;
  long long           pageFaults    = 0;
  std::vector<double> processorBusy;
  double              dpcInterrupt = 0.0; // all CPUs together
};

CpuSample sampleCpu(bool theWithProcessors)
{
  CpuSample aSample;
#ifdef _WIN32
  auto aSeconds = [](const FILETIME& theTime) {
    return static_cast<double>((static_cast<unsigned long long>(theTime.dwHighDateTime) << 32)
                               | theTime.dwLowDateTime)
           * 1e-7;
  };
  FILETIME aCreation, anExit, aKernel, aUser;
  if (GetProcessTimes(GetCurrentProcess(), &aCreation, &anExit, &aKernel, &aUser))
  {
    aSample.processUser   = aSeconds(aUser);
    aSample.processKernel = aSeconds(aKernel);
  }
  PROCESS_MEMORY_COUNTERS aCounters = {};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &aCounters, sizeof(aCounters)))
  {
    aSample.pageFaults = static_cast<long long>(aCounters.PageFaultCount);
  }
  if (!theWithProcessors)
  {
    return aSample;
  }
  using QueryFunction               = LONG(WINAPI*)(int, void*, ULONG, ULONG*);
  static const QueryFunction aQuery = reinterpret_cast<QueryFunction>(
    GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
  const int                   aProcessorPerformanceClass = 8;
  std::vector<ProcessorTimes> aTimes(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
  ULONG                       aLength = 0;
  if (aQuery != nullptr
      && aQuery(aProcessorPerformanceClass,
                aTimes.data(),
                static_cast<ULONG>(aTimes.size() * sizeof(ProcessorTimes)),
                &aLength)
           == 0)
  {
    for (size_t i = 0; i < aLength / sizeof(ProcessorTimes); ++i)
    {
      aSample.processorBusy.push_back(static_cast<double>(aTimes[i].Kernel.QuadPart
                                                          - aTimes[i].Idle.QuadPart
                                                          + aTimes[i].User.QuadPart)
                                      * 1e-7);
      aSample.dpcInterrupt +=
        static_cast<double>(aTimes[i].Dpc.QuadPart + aTimes[i].Interrupt.QuadPart) * 1e-7;
    }
  }
#else
  (void)theWithProcessors;
#endif
  return aSample;
}

// Returns the other logical CPU on the same physical core as theCpu (its SMT sibling), or -1.
int smtSibling(int theCpu)
{
#ifdef _WIN32
  if (theCpu < 0 || theCpu >= 64)
  {
    return -1;
  }
  DWORD aLength = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &aLength);
  std::vector<char> aBuffer(aLength);
  if (aLength == 0
      || !GetLogicalProcessorInformationEx(
        RelationProcessorCore,
        reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(aBuffer.data()),
        &aLength))
  {
    return -1;
  }
  for (DWORD anOffset = 0; anOffset < aLength;)
  {
    const auto* anInfo =
      reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(aBuffer.data() + anOffset);
    const KAFFINITY aMask = anInfo->Processor.GroupMask[0].Mask;
    if ((aMask & (static_cast<KAFFINITY>(1) << theCpu)) != 0)
    {
      for (int i = 0; i < 64; ++i)
      {
        if (i != theCpu && (aMask & (static_cast<KAFFINITY>(1) << i)) != 0)
        {
          return i;
        }
      }
      return -1;
    }
    anOffset += anInfo->Size;
  }
#else
  (void)theCpu;
#endif
  return -1;
}

// Background load during the timed runs. background_cpus is the CPU time not used by this process
// (other processes, interrupts and DPCs), as an average number of busy logical CPUs, and
// dpc_interrupt_cpus the interrupt and DPC part of it. For single-threaded jobs, cpu_other_share
// is the share of wall time the pinned CPU spent on other work, and sibling_busy_share the share
// its SMT sibling was busy.
void writeLoad(billet::JsonWriter& theJson,
               const CpuSample&    theStart,
               const CpuSample&    theEnd,
               double              theSeconds,
               int                 thePinnedCpu)
{
  const size_t aCount = std::min(theStart.processorBusy.size(), theEnd.processorBusy.size());
  if (aCount == 0 || theSeconds <= 0.0)
  {
    return;
  }
  const double anOwn =
    (theEnd.processUser + theEnd.processKernel) - (theStart.processUser + theStart.processKernel);
  double aBusy = 0.0;
  for (size_t i = 0; i < aCount; ++i)
  {
    aBusy += theEnd.processorBusy[i] - theStart.processorBusy[i];
  }
  theJson.key("load");
  theJson.beginObject();
  theJson.key("seconds").value(theSeconds);
  theJson.key("background_cpus").value(std::max(0.0, (aBusy - anOwn) / theSeconds));
  theJson.key("dpc_interrupt_cpus")
    .value((theEnd.dpcInterrupt - theStart.dpcInterrupt) / theSeconds);
  if (thePinnedCpu >= 0 && static_cast<size_t>(thePinnedCpu) < aCount)
  {
    const double aPinned =
      theEnd.processorBusy[thePinnedCpu] - theStart.processorBusy[thePinnedCpu];
    theJson.key("cpu").value(thePinnedCpu);
    theJson.key("cpu_other_share").value(std::max(0.0, (aPinned - anOwn) / theSeconds));
    const int aSibling = smtSibling(thePinnedCpu);
    if (aSibling >= 0 && static_cast<size_t>(aSibling) < aCount)
    {
      theJson.key("sibling_cpu").value(aSibling);
      theJson.key("sibling_busy_share")
        .value((theEnd.processorBusy[aSibling] - theStart.processorBusy[aSibling]) / theSeconds);
    }
  }
  theJson.endObject();
}

// Default CPU for single-threaded jobs: the first logical CPU of the last physical core on SMT
// machines. Windows runs most background threads and interrupts on the first cores.
int defaultCpu()
{
#ifdef _WIN32
  return std::max(0, static_cast<int>(GetActiveProcessorCount(0)) - 2);
#else
  return 0;
#endif
}

// Single-threaded mode turns off the parallel flags, routes OSD_Parallel to a one-thread OCCT pool
// and pins the process to one logical CPU. All-cores mode turns the parallel flags on.
void configureThreads(bool theParallel, int theCpu, bool thePinMainThread)
{
  BOPAlgo_Options::SetParallelMode(theParallel);
  if (theParallel)
  {
#ifdef _WIN32
    // Keeps the calling thread on one CPU; worker threads stay free to use all CPUs.
    if (thePinMainThread && theCpu >= 0)
    {
      SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << theCpu);
    }
#else
    (void)thePinMainThread;
#endif
  }
  else
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
  std::cerr
    << "Usage:\n"
       "  billet-bench run --op <id> --model <file|part-id> [--threads 1|all] [--warmup N] "
       "[--runs N]\n"
       "                   [--cpu N] [--param key=value]... [--save-output <file>] "
       "[--scratch <dir>]\n"
       "                   [--input <brep|bbrep>] [--pin-main-thread] [--no-validity] [--out "
       "<json>]\n"
       "  billet-bench convert --model <file> --save-output <brep|bbrep> [--param select=...] "
       "[--out <json>]\n"
       "  billet-bench parts            list scripted parts for the rebuild operation\n"
       "  billet-bench ops              list operations\n"
       "  billet-bench edges --model <file> [--out <json>]\n"
       "  billet-bench probe --op fillet|shell --model <file> [--param key=value]... "
       "[--out <json>]\n"
       "                   fillet: radius=R | radius_frac=F [count=12] [select=largest-solid] "
       "[tries=200]\n"
       "                   shell:  thickness=T | thickness_frac=F [select=largest-solid] "
       "[tries=20]\n"
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
           bool                      thePinMainThread,
           bool                      theValidity,
           const std::string&        theOutFile)
{
  using Clock = std::chrono::steady_clock;
  configureThreads(theOptions.parallel, theCpu, thePinMainThread);

  billet::JsonWriter aJson;
  aJson.beginObject();
  aJson.key("harness_version").value(kHarnessVersion);
  aJson.key("occt_version").value(OCC_VERSION_COMPLETE);
  aJson.key("op").value(theOptions.op);
  aJson.key("model").value(theOptions.model);
  aJson.key("threads").value(theOptions.parallel ? "all" : "1");
  aJson.key("cpu").value(theCpu);
  aJson.key("pin_main_thread").value(theOptions.parallel && thePinMainThread);
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
  std::vector<double>                aTimes, aUserTimes, aKernelTimes;
  std::vector<long long>             aPageFaults;
  CpuSample                          aLoadStart, aLoadEnd;
  double                             aLoadSeconds = 0.0;
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

    aPhase                             = "run";
    aLoadStart                         = sampleCpu(true);
    const Clock::time_point aRunsStart = Clock::now();
    for (int i = 0; i < theOptions.runs; ++i)
    {
      anOperation->prepareRun();
      const CpuSample         aBefore = sampleCpu(false);
      const Clock::time_point aStart  = Clock::now();
      anOperation->run();
      const Clock::time_point anEnd   = Clock::now();
      const CpuSample         anAfter = sampleCpu(false);
      aTimes.push_back(std::chrono::duration<double>(anEnd - aStart).count());
      aUserTimes.push_back(anAfter.processUser - aBefore.processUser);
      aKernelTimes.push_back(anAfter.processKernel - aBefore.processKernel);
      aPageFaults.push_back(anAfter.pageFaults - aBefore.pageFaults);
    }
    aLoadSeconds = std::chrono::duration<double>(Clock::now() - aRunsStart).count();
    aLoadEnd     = sampleCpu(true);
    anOk         = true;
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
  aJson.key("cpu_user_s").value(aUserTimes);
  aJson.key("cpu_kernel_s").value(aKernelTimes);
  aJson.key("page_faults").value(aPageFaults);
  writeLoad(aJson, aLoadStart, aLoadEnd, aLoadSeconds, theOptions.parallel ? -1 : theCpu);
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
  int                aCpu      = defaultCpu();
  bool               aValidity = true;
  bool               aPinMain  = false;
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
      else if (anArg == "--input")
        anOptions.inputFile = aNext();
      else if (anArg == "--no-validity")
        aValidity = false;
      else if (anArg == "--pin-main-thread")
        aPinMain = true;
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
      return runJob(anOptions, aCpu, aPinMain, aValidity, anOutFile);
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
    if (aCommand == "convert")
    {
      if (anOptions.model.empty() || anOptions.outputFile.empty())
      {
        throw std::runtime_error("convert needs --model <step> and --save-output <brep>");
      }
      const auto        aStart  = std::chrono::steady_clock::now();
      const auto        aSelect = anOptions.params.find("select");
      const std::string aSelection =
        aSelect == anOptions.params.end() ? std::string("all") : aSelect->second;
      const TopoDS_Shape aShape = billet::loadModelShape(anOptions.model, aSelection);
      const double       aReadSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - aStart).count();
      // Binary BREP stores doubles exactly; text BREP rounds them to 15 significant digits.
      long long aBytes = -1;
      if (std::filesystem::path(anOptions.outputFile).extension() == ".bbrep")
      {
        if (BinTools::Write(aShape,
                            anOptions.outputFile.c_str(),
                            false,
                            false,
                            BinTools_FormatVersion_CURRENT))
        {
          aBytes = static_cast<long long>(std::filesystem::file_size(anOptions.outputFile));
        }
      }
      else
      {
        aBytes = billet::writeBrep(aShape, anOptions.outputFile, false);
      }
      if (aBytes < 0)
      {
        throw std::runtime_error("cannot write " + anOptions.outputFile);
      }
      const billet::Fingerprint aPrint = billet::computeFingerprint(aShape, false, false);
      billet::JsonWriter        aJson;
      aJson.beginObject();
      aJson.key("model").value(anOptions.model);
      aJson.key("brep").value(anOptions.outputFile);
      aJson.key("bytes").value(aBytes);
      aJson.key("read_s").value(aReadSeconds);
      aJson.key("fingerprint");
      billet::writeFingerprint(aJson, aPrint);
      aJson.endObject();
      emit(aJson.text(), anOutFile);
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
