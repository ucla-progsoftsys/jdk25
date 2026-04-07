/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_OOPS_PORTABLEMDO_HPP
#define SHARE_OOPS_PORTABLEMDO_HPP

#include "memory/allocation.hpp"
#include "utilities/globalDefinitions.hpp"

// PortableMDO — Portable MethodData serialization format
//
// This file defines the on-disk binary format for exporting and importing
// MethodData (MDO) profiling information across JVM instances of the same
// JDK version.
//
// Design principles:
//   - All metaspace pointers (Klass*, Method*) are serialized as symbolic
//     references (class name + classloader identity, method name + signature)
//     stored in shared reference tables.
//   - Profile data is serialized at the LOGICAL level (tag + bci + counters +
//     symbolic refs), NOT as raw DataLayout binary cells. This decouples the
//     format from flag-dependent layout (TypeProfileWidth, BciProfileWidth, etc.).
//   - Data displacements (used by the interpreter's mdp) are NOT serialized.
//     They are recomputed from bytecodes during import via the normal
//     MethodData::initialize() + post_initialize() path.
//   - Hidden/anonymous classes (lambdas, invokedynamic-generated classes) are
//     intentionally discarded during export. The overflow count is preserved
//     so the compiler still knows the site is hot.
//   - Deoptimization history is exported with configurable decay to avoid
//     overly conservative compilation in the target JVM.
//   - Escape analysis fields are NOT exported (zeroed). EA recomputes cheaply
//     during the first C2 compilation.

// --------------------------------------------------------------------------
// Constants
// --------------------------------------------------------------------------

class PortableMDO : AllStatic {
public:
  // File magic number: "JMDO" in ASCII
  static constexpr uint32_t MAGIC = 0x4A4D444F;

  // Format version — increment when the on-disk layout changes
  static constexpr uint16_t FORMAT_VERSION = 1;

  // Maximum lengths for sanity checking during parsing
  static constexpr uint16_t MAX_UTF8_LENGTH       = 4096;
  static constexpr uint16_t MAX_RECORD_COUNT       = UINT16_MAX;
  static constexpr uint16_t MAX_EXTRA_RECORD_COUNT = UINT16_MAX;
  static constexpr uint16_t MAX_KLASS_REF_COUNT    = UINT16_MAX;
  static constexpr uint16_t MAX_METHOD_REF_COUNT   = UINT16_MAX;
  static constexpr uint32_t MAX_MDO_ENTRY_COUNT    = 1000000;
  static constexpr uint8_t  MAX_RECEIVER_ROWS      = 16;
  static constexpr uint8_t  MAX_RET_ROWS           = 16;
  static constexpr uint16_t MAX_SWITCH_CASES       = 65535;
  static constexpr uint8_t  MAX_TYPE_ARGS          = 255;
  static constexpr uint16_t MAX_ARG_MODIFIED       = 256;
  static constexpr uint16_t MAX_EXCEPTION_HANDLERS = 65535;
  static constexpr uint16_t MAX_PARAM_TYPES        = 256;

  // Sentinel value for "no klass" or "discarded hidden class"
  static constexpr int16_t NULL_KLASS_REF   = -1;

  // Sentinel value for "no method"
  static constexpr int16_t NULL_METHOD_REF  = -1;

  // Maximum trap history array size — must accommodate
  // Deoptimization::Reason_TRAP_HISTORY_LENGTH (and 2x for JVMCI OSR split).
  // We use a fixed-size array in the portable format large enough for any
  // configuration. The actual number of valid entries is recorded in the header.
  static constexpr uint8_t MAX_TRAP_HIST_LENGTH = 64;

  // Compute a CRC32 fingerprint over a method's bytecodes.
  // Used to detect stale profiles when method bodies change between runs.
  static uint32_t compute_bytecode_fingerprint(const Method* method);

  // Export all mature MDO profiles to a binary file.
  // deopt_decay: multiplier (0.0–1.0) applied to deoptimization counts to
  //   avoid overly conservative compilation in the target JVM.
  static bool export_all_to_file(const char* filepath, float deopt_decay = 0.5f);

  // Called from before_exit() to export if ExportMDOFile is set.
  // Executes the export at a safepoint via VM_ExportMDO.
  static void export_on_shutdown();

  // --- Import API ---

  // Initialize the import subsystem. Loads and parses the file, builds
  // the in-memory lookup map. Called once during VM startup.
  static void initialize_import(const char* filepath);

  // Returns true if import data has been loaded and is available.
  static bool has_import_data();

  // Try to import an MDO for the given method. Returns a fully patched
  // MethodData* allocated in the method holder's ClassLoaderData metaspace,
  // or nullptr if no matching entry exists or validation fails.
  // The caller is responsible for installing it via Atomic::replace_if_null.
  static MethodData* try_import(const methodHandle& method, TRAPS);

  // Cleanup — release import data structures. Called during VM shutdown.
  static void shutdown_import();
};

// --------------------------------------------------------------------------
// Classloader identity tags
// --------------------------------------------------------------------------

enum class PortableClassLoaderTag : uint8_t {
  BOOT     = 0,   // null / bootstrap classloader
  PLATFORM = 1,   // platform (ext) classloader
  APP      = 2,   // application (system) classloader
  CUSTOM   = 3    // user-defined classloader (identified by name hash)
};

// --------------------------------------------------------------------------
// Counter normalization policy
// --------------------------------------------------------------------------

enum class PortableMDOImportPolicy : uint8_t {
  RAW    = 0,   // Use imported counters as-is
  SCALED = 1,   // Scale to a target invocation count, preserving ratios
  BINARY = 2    // Collapse to hot/cold thresholds
};

// --------------------------------------------------------------------------
// Extra data record types
// --------------------------------------------------------------------------

enum class PortableExtraTag : uint8_t {
  BIT_DATA            = 0,   // Stray trap at a BCI without dedicated profile data
  SPECULATIVE_TRAP    = 1    // SpeculativeTrapData — failed type speculation record
};

// --------------------------------------------------------------------------
// Global file header (written once at the start of the file)
// --------------------------------------------------------------------------

struct PortableMDOFileHeader {
  uint32_t magic;                  // Must be PortableMDO::MAGIC
  uint16_t format_version;         // Must be PortableMDO::FORMAT_VERSION
  uint16_t _padding0;

  // JDK identity — used to reject profiles from a different build
  uint32_t jdk_version_hash;       // Hash of JDK version string

  // Platform characteristics
  uint8_t  pointer_size;           // sizeof(void*): 4 or 8
  uint8_t  endianness;             // 0 = little-endian, 1 = big-endian
  uint16_t _padding1;

  // VM flag snapshot at export time.
  // These are recorded for diagnostics. Because the format is logical
  // (not binary DataLayout cells), different flag values between export
  // and import are handled gracefully — rows are capped to the importing
  // VM's configured width.
  uint16_t type_profile_width;     // TypeProfileWidth at export
  uint16_t method_profile_width;   // MethodProfileWidth at export
  uint16_t bci_profile_width;      // BciProfileWidth at export
  uint8_t  profile_traps;          // ProfileTraps at export (0 or 1)
  uint8_t  profile_exception_handlers; // ProfileExceptionHandlers at export

  // Table offsets (byte offsets from start of file)
  uint32_t klass_ref_table_offset;
  uint32_t klass_ref_count;
  uint32_t method_ref_table_offset;
  uint32_t method_ref_count;

  // MDO entries
  uint32_t mdo_entries_offset;
  uint32_t mdo_entry_count;

  // Trap history dimension — actual number of valid entries in trap_hist arrays
  uint8_t  trap_hist_length;       // Deoptimization::Reason_TRAP_HISTORY_LENGTH
  uint8_t  _padding2[3];
};

// --------------------------------------------------------------------------
// Klass reference table entry
//
// Symbolic representation of a Klass*. Hidden classes are never stored;
// they are discarded at export time and the referencing profile slot is
// set to NULL_KLASS_REF.
// --------------------------------------------------------------------------

struct PortableKlassRefEntry {
  PortableClassLoaderTag loader_tag;
  uint8_t  _padding0;
  uint16_t name_length;            // Length of class name in bytes (modified UTF-8)
  // Followed by: uint8_t name[name_length]
  // Name is in internal form: "java/util/HashMap"
};

// --------------------------------------------------------------------------
// Method reference table entry
//
// Symbolic representation of a Method*. Used by SpeculativeTrapData
// to identify the compilation root that caused the failed speculation.
// --------------------------------------------------------------------------

struct PortableMethodRefEntry {
  uint16_t klass_ref_index;        // Index into KlassRefTable for the holder
  uint16_t name_length;            // Method name length
  uint16_t sig_length;             // Method signature length
  uint16_t _padding0;
  // Followed by: uint8_t name[name_length]
  // Followed by: uint8_t sig[sig_length]
};

// --------------------------------------------------------------------------
// Per-method MDO entry header
// --------------------------------------------------------------------------

struct PortableMDOMethodIdentity {
  uint16_t klass_ref_index;        // Index into KlassRefTable for the holder
  uint16_t name_length;            // Method name length (modified UTF-8)
  uint16_t sig_length;             // Method signature length (modified UTF-8)
  uint16_t _padding0;
  uint32_t bytecode_fingerprint;   // CRC32 over the method's bytecodes
  // Followed by: uint8_t name[name_length]
  // Followed by: uint8_t sig[sig_length]
};

// --------------------------------------------------------------------------
// MDO header fields — scalar profiling metadata for the whole method
// --------------------------------------------------------------------------

struct PortableMDOHeaderFields {
  // Invocation / backedge counters (already normalized/scaled at export)
  int32_t  invocation_count;
  int32_t  backedge_count;

  // Compiler counters (with decay applied at export)
  uint16_t nof_decompiles;
  uint16_t nof_overflow_recompiles;
  uint16_t nof_overflow_traps;
  uint16_t tenure_traps;

  // Trap histogram — per deoptimization reason counts (with decay)
  // Only trap_hist_length entries are valid (from PortableMDOFileHeader).
  uint8_t  trap_hist[PortableMDO::MAX_TRAP_HIST_LENGTH];

  // Structural info (computed by C1)
  int16_t  num_loops;
  int16_t  num_blocks;

  // Would this method benefit from profiling?
  // 0 = unknown, 1 = no_profile, 2 = profile
  uint8_t  would_profile;
  uint8_t  _padding0[3];

  // ArgInfoData — per-argument modified flags
  uint16_t arg_modified_count;
  // Followed by: uint8_t arg_modified[arg_modified_count]  (after this struct)
};

// --------------------------------------------------------------------------
// Symbolic type entry — used for ParametersTypeData and CallTypeData
// argument/return type profiling
// --------------------------------------------------------------------------

struct PortableSymbolicTypeEntry {
  int16_t  klass_ref_index;        // Index into KlassRefTable, or NULL_KLASS_REF
  uint16_t stack_slot;             // Stack slot number (for arguments)
  uint8_t  null_seen;              // Whether a null reference was observed
  uint8_t  type_unknown;           // Whether conflicting types were seen
  uint16_t _padding0;
};

// --------------------------------------------------------------------------
// Exception handler coverage entry
// --------------------------------------------------------------------------

struct PortableExceptionHandlerEntry {
  uint16_t handler_bci;            // BCI of the exception handler
  uint8_t  entered;                // Whether this handler was entered (0 or 1)
  uint8_t  _padding0;
};

// --------------------------------------------------------------------------
// Receiver type row — used by ReceiverTypeData and VirtualCallData
// --------------------------------------------------------------------------

struct PortableReceiverRow {
  int16_t  klass_ref_index;        // Index into KlassRefTable, or NULL_KLASS_REF
  uint16_t _padding0;
  uint32_t count;                  // Number of times this receiver type was seen
};

// --------------------------------------------------------------------------
// Ret data row — used by RetData for jsr/ret profiling
// --------------------------------------------------------------------------

struct PortableRetRow {
  int16_t  target_bci;             // Target BCI, or -1 if unused
  uint16_t _padding0;
  uint32_t count;                  // Number of times this target was taken
  // Note: displacement is NOT stored — recomputed on import
};

// --------------------------------------------------------------------------
// Per-BCI profile record header
//
// Each record starts with this common header, followed by tag-specific
// payload data. The tag values match DataLayout::tag enum.
// --------------------------------------------------------------------------

struct PortableProfileRecordHeader {
  uint8_t  tag;                    // DataLayout tag enum value
  uint8_t  flags;                  // DataLayout flags byte
  uint16_t bci;                    // Bytecode index
  uint32_t trap_state;             // 32-bit trap state from DataLayout header
};

// Tag-specific payload structs follow the header in the serialized stream.
// The tag field determines which payload is present.

// BitData payload (tag = bit_data_tag = 1)
// No additional fields — all info is in flags and trap_state.
struct PortableBitDataPayload {
  // intentionally empty
};

// CounterData payload (tag = counter_data_tag = 2)
struct PortableCounterDataPayload {
  int32_t  count;
};

// JumpData payload (tag = jump_data_tag = 3)
// Displacement is NOT stored — recomputed on import.
struct PortableJumpDataPayload {
  uint32_t taken;
};

// ReceiverTypeData payload (tag = receiver_type_data_tag = 4)
// Also used for VirtualCallData (tag = virtual_call_data_tag = 5)
struct PortableReceiverTypeDataPayload {
  uint32_t count;                  // Overflow / total counter
  uint8_t  num_rows;               // Number of (klass, count) rows
  uint8_t  _padding0[3];
  // Followed by: PortableReceiverRow rows[num_rows]
};

// RetData payload (tag = ret_data_tag = 6)
struct PortableRetDataPayload {
  uint32_t count;                  // Total ret execution count
  uint8_t  num_rows;               // Number of (bci, count) target rows
  uint8_t  _padding0[3];
  // Followed by: PortableRetRow rows[num_rows]
};

// BranchData payload (tag = branch_data_tag = 7)
// Displacement is NOT stored — recomputed on import.
struct PortableBranchDataPayload {
  uint32_t taken;
  uint32_t not_taken;
};

// MultiBranchData payload (tag = multi_branch_data_tag = 8)
// Displacements are NOT stored — recomputed on import.
struct PortableMultiBranchDataPayload {
  uint32_t default_count;
  uint16_t num_cases;
  uint16_t _padding0;
  // Followed by: uint32_t case_counts[num_cases]
};

// CallTypeData payload (tag = call_type_data_tag = 10)
struct PortableCallTypeDataPayload {
  int32_t  count;                  // Call counter
  uint8_t  num_args;               // Number of profiled argument types
  uint8_t  has_return;             // Whether return type is profiled (0 or 1)
  uint16_t _padding0;
  // Followed by: PortableSymbolicTypeEntry args[num_args]
  // Followed by: PortableSymbolicTypeEntry return_type (if has_return)
};

// VirtualCallTypeData payload (tag = virtual_call_type_data_tag = 11)
// Combines ReceiverTypeData rows with CallTypeData argument/return types.
struct PortableVirtualCallTypeDataPayload {
  uint32_t count;                  // Overflow / total counter
  uint8_t  num_receiver_rows;      // Number of receiver (klass, count) rows
  uint8_t  num_args;               // Number of profiled argument types
  uint8_t  has_return;             // Whether return type is profiled (0 or 1)
  uint8_t  _padding0;
  // Followed by: PortableReceiverRow receiver_rows[num_receiver_rows]
  // Followed by: PortableSymbolicTypeEntry args[num_args]
  // Followed by: PortableSymbolicTypeEntry return_type (if has_return)
};

// --------------------------------------------------------------------------
// Extra data records (from the extra_data section of the MDO)
// --------------------------------------------------------------------------

struct PortableExtraRecordHeader {
  PortableExtraTag tag;
  uint8_t  flags;
  uint16_t bci;
  uint32_t trap_state;
};

// SpeculativeTrapData extra record payload
struct PortableSpeculativeTrapPayload {
  int16_t  method_ref_index;       // Index into MethodRefTable, or NULL_METHOD_REF
  uint16_t _padding0;
};

// BitData extra record payload (stray trap)
// No additional fields beyond the header.

// --------------------------------------------------------------------------
// Complete per-method MDO entry (variable length, serialized sequentially)
//
// On-disk layout of a single PortableMDOEntry:
//
//   PortableMDOMethodIdentity  (+ inline name/sig bytes)
//   PortableMDOHeaderFields    (+ inline arg_modified bytes)
//   uint16_t record_count
//   uint16_t extra_record_count
//   uint16_t param_type_count
//   uint16_t exception_handler_count
//   [record_count x (PortableProfileRecordHeader + tag-specific payload)]
//   [extra_record_count x (PortableExtraRecordHeader + tag-specific payload)]
//   [param_type_count x PortableSymbolicTypeEntry]
//   [exception_handler_count x PortableExceptionHandlerEntry]
//
// --------------------------------------------------------------------------

struct PortableMDOEntryCounts {
  uint16_t record_count;           // Number of per-BCI profile records
  uint16_t extra_record_count;     // Number of extra data records
  uint16_t param_type_count;       // Number of parameter type entries
  uint16_t exception_handler_count; // Number of exception handler entries
};

#endif // SHARE_OOPS_PORTABLEMDO_HPP
