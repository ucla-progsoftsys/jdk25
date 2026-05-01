#ifndef SHARE_SERVICES_PROFILECHECKPOINT_GLOBALS_HPP
#define SHARE_SERVICES_PROFILECHECKPOINT_GLOBALS_HPP

#include "runtime/globals_shared.hpp"

// Defines profile checkpoint (binary MDO) flags. Flags reuse existing names.
#define PROFILECHECKPOINT_FLAGS(develop,                                      \
                                develop_pd,                                   \
                                product,                                      \
                                product_pd,                                   \
                                range,                                        \
                                constraint)                                   \
                                                                               \
  product(ccstr, MDOReplayDumpFile, nullptr, DIAGNOSTIC,                      \
          "File for exporting profiles (binary)")                           \
                                                                               \
  product(ccstr, MDOReplayLoadFile, nullptr, DIAGNOSTIC,                      \
          "File for importing profiles (binary)")                           \
                                                                               \
  product(bool, DumpMDOAtExit, false, DIAGNOSTIC,                             \
          "Dump MDOs to MDOReplayDumpFile at VM exit (binary)")             \
                                                                               \
  product(bool, LoadMDOAtStartup, false, DIAGNOSTIC,                          \
          "Load MDOs from MDOReplayLoadFile at VM startup (binary)")          \
                                                                               \
  product(bool, PrintMDOAtDump, false, DIAGNOSTIC,                            \
          "Debug: print MethodData/MethodCounters for each dumped method")    \
                                                                               \
  product(bool, PrintMDOAfterLoad, false, DIAGNOSTIC,                         \
          "Debug: print MethodData/MethodCounters after loading each method")  \
                                                                               \
  product(bool, EagerCompileAfterLoad, false, DIAGNOSTIC,                      \
          "After loading MDOs at startup, run eager compilation")

DECLARE_FLAGS(PROFILECHECKPOINT_FLAGS)

#endif // SHARE_SERVICES_PROFILECHECKPOINT_GLOBALS_HPP


