# Portable MDO Export/Import — Implementation Design

## Overview

Export `MethodData` (MDO) profiling information from one JVM instance and import it
into another instance of the same JDK version, enabling warmup-free optimized compilation.

---

## Phase 1: Portable Serialization Format

### 1.1 File-Level Structure

```
┌─────────────────────────────────────┐
│         GlobalFileHeader            │
│  - magic: "JMDO"                   │
│  - format_version: uint16           │
│  - jdk_version_hash: uint32         │
│  - pointer_size: uint8 (4 or 8)    │
│  - endianness: uint8               │
│  - flags_snapshot:                  │
│      TypeProfileWidth: uint16       │
│      MethodProfileWidth: uint16     │
│      BciProfileWidth: uint16        │
│      ProfileTraps: bool             │
│      ProfileExceptionHandlers: bool │
│  - klass_ref_table_offset: uint32   │
│  - method_ref_table_offset: uint32  │
│  - mdo_entry_count: uint32          │
├─────────────────────────────────────┤
│         KlassRefTable               │
│  Array of:                          │
│  - utf8_length: uint16              │
│  - class_name: utf8 bytes           │
│  - classloader_tag: uint8           │
│    (0=boot, 1=platform, 2=app)     │
│  - is_hidden: bool (always false —  │
│    hidden classes are not stored)   │
├─────────────────────────────────────┤
│         MethodRefTable              │
│  Array of:                          │
│  - klass_ref_index: uint16          │
│  - name_utf8_length: uint16         │
│  - name: utf8 bytes                 │
│  - sig_utf8_length: uint16          │
│  - sig: utf8 bytes                  │
├─────────────────────────────────────┤
│     PortableMDOEntry[0]             │
│     PortableMDOEntry[1]             │
│     ...                             │
│     PortableMDOEntry[N-1]           │
└─────────────────────────────────────┘
```

### 1.2 PortableMDOEntry

Each entry represents one method's complete profile.

```
┌──────────────────────────────────────────┐
│ MethodIdentity                           │
│  - klass_ref_index: uint16               │
│  - method_name_utf8: length-prefixed     │
│  - method_sig_utf8: length-prefixed      │
│  - bytecode_fingerprint: uint32 (CRC32)  │
├──────────────────────────────────────────┤
│ HeaderFields                             │
│  - invocation_count: int32               │
│  - backedge_count: int32                 │
│  - nof_decompiles: uint16 (capped)       │
│  - nof_overflow_recompiles: uint16       │
│  - nof_overflow_traps: uint16            │
│  - trap_hist: uint8[Reason_LIMIT]        │
│  - tenure_traps: uint16                  │
│  - num_loops: int16                      │
│  - num_blocks: int16                     │
│  - would_profile: uint8                  │
│  - arg_modified: uint8[] (ArgInfoData)   │
├──────────────────────────────────────────┤
│ ProfileRecords                           │
│  - record_count: uint16                  │
│  - records: PortableProfileRecord[]      │
├──────────────────────────────────────────┤
│ ExtraDataRecords                         │
│  - extra_count: uint16                   │
│  - extras: PortableExtraRecord[]         │
├──────────────────────────────────────────┤
│ ParameterTypeData (optional)             │
│  - param_count: uint16                   │
│  - param_types: SymbolicTypeEntry[]      │
├──────────────────────────────────────────┤
│ ExceptionHandlerData (optional)          │
│  - handler_count: uint16                 │
│  - handlers: { bci: uint16,             │
│                entered: bool }[]         │
└──────────────────────────────────────────┘
```

### 1.3 PortableProfileRecord (per-BCI)

Tagged union, discriminated by `tag` field:

```
Common header:
  - tag: uint8 (matches DataLayout tag enum)
  - bci: uint16
  - trap_state: uint32
  - flags: uint8

Tag-specific payloads:

BitData (tag=1):
  (no extra fields — all info is in flags/trap_state)

CounterData (tag=2):
  - count: int32

JumpData (tag=3):
  - taken: uint32
  (displacement is NOT serialized — recomputed on import)

ReceiverTypeData (tag=4):
  - count: uint32 (overflow/total counter)
  - num_rows: uint8
  - rows[]:
    - klass_ref_index: int16 (-1 if null/discarded)
    - count: uint32

VirtualCallData (tag=5):
  (same layout as ReceiverTypeData)

RetData (tag=6):
  - count: uint32
  - num_rows: uint8
  - rows[]:
    - bci: int16
    - count: uint32
  (displacement NOT serialized)

BranchData (tag=7):
  - taken: uint32
  - not_taken: uint32
  (displacement NOT serialized)

MultiBranchData (tag=8):
  - default_count: uint32
  - num_cases: uint16
  - cases[]:
    - count: uint32
  (displacements NOT serialized)

CallTypeData (tag=10):
  - count: int32
  - num_args: uint8
  - args[]:
    - stack_slot: uint16
    - type_klass_ref_index: int16 (-1 if null)
    - null_seen: bool
    - type_unknown: bool
  - has_return: bool
  - return_type:
    - klass_ref_index: int16
    - null_seen: bool
    - type_unknown: bool

VirtualCallTypeData (tag=11):
  (ReceiverTypeData fields + CallTypeData arg/return fields)

ArgInfoData (tag=9):
  (serialized in HeaderFields.arg_modified instead)
```

### 1.4 PortableExtraRecord

```
SpeculativeTrapData:
  - bci: uint16
  - trap_state: uint32
  - method_ref_index: int16 (-1 to discard)

BitData (stray trap):
  - bci: uint16
  - trap_state: uint32
  - flags: uint8
```

### 1.5 SymbolicTypeEntry (for ParametersTypeData)

```
  - klass_ref_index: int16 (-1 if null)
  - null_seen: bool
  - type_unknown: bool
  - stack_slot: uint16
```

---

## Phase 2: MDO Exporter

### New files:
- `src/hotspot/share/oops/portableMDO.hpp` — format structs, constants
- `src/hotspot/share/oops/portableMDO.cpp` — export/import implementation

### 2.1 Bytecode Fingerprint

```cpp
// In portableMDO.cpp
uint32_t PortableMDO::compute_bytecode_fingerprint(const Method* method) {
  uint32_t crc = 0;
  const address code_base = method->code_base();
  const int code_size = method->code_size();
  // CRC32 over raw bytecodes (including operands)
  crc = ClassLoader::crc32(crc, (const char*)code_base, code_size);
  return crc;
}
```

Uses the existing `ClassLoader::crc32()` utility already in the JDK.

### 2.2 Klass* Serialization Logic

```
For each Klass* encountered in any profile record:
  1. If klass == nullptr → write klass_ref_index = -1
  2. If klass->is_hidden() → write klass_ref_index = -1
     (discard hidden/lambda classes; preserve the row's count value
      so the overflow counter remains accurate)
  3. Otherwise → look up or insert into KlassRefTable,
     write the table index
```

### 2.3 Deopt History Decay on Export

```cpp
void PortableMDO::export_compiler_counters(
    const MethodData* mdo, PortableMDOHeaderFields* out, float decay) {
  // Trap histogram: apply decay
  for (uint i = 0; i < MethodData::trap_reason_limit(); i++) {
    uint raw = mdo->trap_count(i);
    out->trap_hist[i] = (uint8_t)MIN2((uint)(raw * decay), (uint)255);
  }
  // Cap decompile count at PerMethodRecompilationCutoff / 4
  out->nof_decompiles = MIN2(mdo->decompile_count(),
                             (uint)(PerMethodRecompilationCutoff / 4));
  // Reset overflow counters — less meaningful across runs
  out->nof_overflow_recompiles = 0;
  out->nof_overflow_traps = 0;
}
```

### 2.4 Escape Analysis Fields

Do NOT export `_eflags`, `_arg_local`, `_arg_stack`, `_arg_returned`.
Write zeros. EA recomputes during the first C2 compilation in the target JVM.
Cost: one compilation without EA hints, negligible.

### 2.5 Hidden Class Handling (invokedynamic / Lambda)

During per-BCI export of ReceiverTypeData / VirtualCallData / TypeEntries:

```cpp
int16_t PortableMDO::export_klass_ref(Klass* k, KlassRefTable* table) {
  if (k == nullptr) return -1;
  if (k->is_hidden()) {
    // Lambda/anonymous class — non-deterministic name across runs.
    // Discard the type, but the caller preserves the count.
    return -1;
  }
  // Check loader is alive (concurrent unloading safety)
  if (!k->is_loader_present_and_alive()) return -1;
  // Add to or find in table
  return table->add_or_find(k->name(), classloader_tag(k));
}
```

### 2.6 Export Trigger

Two modes:
- **Shutdown export:** VM_Operation at JVM exit, iterates `ClassLoaderDataGraph`,
  exports all methods with mature MDOs.
- **On-demand export:** Callable via `jcmd <pid> VM.exportMDO <filepath>`
  (diagnostic command, runs at safepoint).

### 2.7 Maturity Filter

Only export MDOs where `MethodData::is_mature()` returns true.
Immature profiles add noise and aren't worth persisting.

---

## Phase 3: MDO Importer

### 3.1 File Loading & Validation

On VM startup (if `-XX:ImportMDOFile=<path>` is set):
1. Memory-map the file
2. Validate `GlobalFileHeader`:
   - Magic number matches
   - Format version compatible
   - JDK version hash matches `VM_Version::vm_build_id()` or similar
   - Pointer size and endianness match current platform
   - **Flag snapshot does NOT need to match** — we serialize at the logical
     level, so different `TypeProfileWidth` is fine (import will create
     the correct number of rows for the *current* VM's configuration)
3. Parse `KlassRefTable` and `MethodRefTable` into memory
4. Build a `HashMap<SymbolPair, PortableMDOEntry*>` keyed by
   `(class_name, method_name, method_sig)` for O(1) lookup

### 3.2 Bytecode Fingerprint Validation

On each import attempt (when `build_profiling_method_data` is called):
```cpp
bool PortableMDO::validate_fingerprint(const Method* method,
                                        const PortableMDOEntry* entry) {
  uint32_t current = compute_bytecode_fingerprint(method);
  if (current != entry->bytecode_fingerprint) {
    log_info(mdo, import)("Rejected MDO for %s: fingerprint mismatch "
             "(expected 0x%08x, got 0x%08x)",
             method->name_and_sig_as_C_string(),
             entry->bytecode_fingerprint, current);
    return false;
  }
  return true;
}
```

### 3.3 Klass* Resolution

```cpp
Klass* PortableMDO::resolve_klass_ref(int16_t index, KlassRefTable* table) {
  if (index < 0) return nullptr;  // null or discarded hidden class

  KlassRefEntry* entry = table->at(index);
  ClassLoaderData* loader = classloader_from_tag(entry->loader_tag);

  // Attempt resolution without triggering class loading
  // (we don't want import to force-load classes)
  Symbol* name = SymbolTable::probe(entry->name, entry->name_len);
  if (name == nullptr) return nullptr;

  Klass* k = SystemDictionary::find_instance_klass(
      Thread::current(), name, Handle(), Handle());

  if (k == nullptr) {
    log_debug(mdo, import)("Unresolved klass: %s (will re-profile)",
                           entry->name);
    _unresolved_klass_count++;
  }
  return k;
}
```

Key: use `find_instance_klass` (lookup only) rather than `resolve_or_fail`
(which would trigger class loading). If a class isn't loaded yet, the
profile slot stays null and the interpreter re-profiles it naturally.

### 3.4 Counter Normalization

Three modes controlled by `-XX:MDOImportPolicy`:

**`raw`** — Use counters as-is. Good for same-app training runs.

**`scaled` (default)** — Scale to a configurable mature target:
```cpp
void PortableMDO::normalize_counters(PortableMDOEntry* entry,
                                      int target_invocations) {
  int original = entry->header.invocation_count;
  if (original <= 0) original = 1;
  double scale = (double)target_invocations / original;

  // Scale all per-BCI counters proportionally
  for (int i = 0; i < entry->record_count; i++) {
    PortableProfileRecord* rec = &entry->records[i];
    switch (rec->tag) {
      case counter_data_tag:
        rec->count = clamp_scale(rec->count, scale);
        break;
      case branch_data_tag:
        rec->taken = clamp_scale(rec->taken, scale);
        rec->not_taken = clamp_scale(rec->not_taken, scale);
        break;
      case receiver_type_data_tag:
      case virtual_call_data_tag:
        rec->overflow_count = clamp_scale(rec->overflow_count, scale);
        for (int r = 0; r < rec->num_rows; r++) {
          rec->rows[r].count = clamp_scale(rec->rows[r].count, scale);
        }
        break;
      // ... other tags ...
    }
  }
  entry->header.invocation_count = target_invocations;
  entry->header.backedge_count =
      clamp_scale(entry->header.backedge_count, scale);
}

static int clamp_scale(int value, double scale) {
  double result = value * scale;
  return (int)MIN2(result, (double)INT_MAX);
}
```

**`binary`** — Collapse to hot/cold. All counts become either 0 or
`target_invocations`. Simplest; useful when only branching decisions matter.

### 3.5 MDO Reconstruction (the core import routine)

```cpp
MethodData* PortableMDO::reconstruct(const methodHandle& method,
                                      PortableMDOEntry* entry,
                                      TRAPS) {
  // Step 1: Allocate MDO normally — this computes correct layout,
  // displacements, and initializes all cells to zero.
  ClassLoaderData* loader = method->method_holder()->class_loader_data();
  MethodData* mdo = MethodData::allocate(loader, method, CHECK_NULL);

  // Step 2: Patch header fields
  patch_header(mdo, &entry->header);

  // Step 3: Walk per-BCI data in parallel — both the freshly allocated
  // MDO (which has correct displacements) and the imported logical records
  // (which have correct counters/types).
  ProfileData* live_data = mdo->first_data();
  int import_idx = 0;

  while (mdo->is_valid(live_data) && import_idx < entry->record_count) {
    PortableProfileRecord* rec = &entry->records[import_idx];

    if (live_data->bci() == rec->bci &&
        live_data->data()->tag() == rec->tag) {
      patch_profile_record(mdo, live_data, rec);
      import_idx++;
    }
    // If BCIs don't match, the imported record may have been for a
    // bytecode that no longer has profile data (flag change).
    // Skip the live_data entry — it stays zero-initialized.
    live_data = mdo->next_data(live_data);
  }

  // Step 4: Patch extra data (trap entries)
  patch_extra_data(mdo, entry);

  // Step 5: Patch parameter type data
  if (entry->has_parameter_types && mdo->parameters_type_data() != nullptr) {
    patch_parameter_types(mdo, entry);
  }

  // Step 6: Patch exception handler data
  if (entry->has_exception_handlers) {
    patch_exception_handlers(mdo, entry);
  }

  // Step 7: Fix up runtime state
  mdo->post_import_fixup();

  return mdo;
}

void PortableMDO::patch_header(MethodData* mdo,
                                PortableMDOHeaderFields* hdr) {
  // Compiler counters (with decay already applied during export)
  mdo->set_trap_count_from_import(hdr->trap_hist);
  mdo->set_decompile_count(hdr->nof_decompiles);

  // Counters — use imported values (already normalized)
  mdo->invocation_counter()->set_count(hdr->invocation_count);
  mdo->backedge_counter()->set_count(hdr->backedge_count);
  mdo->set_invocation_counter_start(0);  // reset baselines
  mdo->set_backedge_counter_start(0);

  // Structural info
  mdo->set_num_loops(hdr->num_loops);
  mdo->set_num_blocks(hdr->num_blocks);
  mdo->set_would_profile(hdr->would_profile);

  // Escape analysis — zeroed, recomputed by C2
  // (already zero from allocation, nothing to do)

  // ArgInfoData — patch modified-arg bits
  ArgInfoData* args = mdo->arg_info();
  if (args != nullptr) {
    for (int i = 0; i < MIN2(hdr->arg_modified_count,
                              args->number_of_args()); i++) {
      args->set_arg_modified(i, hdr->arg_modified[i]);
    }
  }
}
```

### 3.6 Per-Record Patching

```cpp
void PortableMDO::patch_profile_record(MethodData* mdo,
                                        ProfileData* live,
                                        PortableProfileRecord* rec) {
  // Copy trap state (always safe — same bci)
  live->set_trap_state(rec->trap_state);

  switch (rec->tag) {
  case DataLayout::counter_data_tag: {
    CounterData* cd = live->as_CounterData();
    cd->set_count(rec->count);
    break;
  }
  case DataLayout::jump_data_tag: {
    JumpData* jd = live->as_JumpData();
    jd->set_taken(rec->taken);
    // displacement is ALREADY CORRECT from initialize() — don't touch it
    break;
  }
  case DataLayout::branch_data_tag: {
    BranchData* bd = live->as_BranchData();
    bd->set_taken(rec->taken);
    bd->set_not_taken(rec->not_taken);
    // displacement already correct
    break;
  }
  case DataLayout::receiver_type_data_tag:
  case DataLayout::virtual_call_data_tag: {
    ReceiverTypeData* rtd = live->as_ReceiverTypeData();
    rtd->set_count(rec->overflow_count);
    uint rows_to_patch = MIN2((uint)rec->num_rows, rtd->row_limit());
    for (uint r = 0; r < rows_to_patch; r++) {
      Klass* k = resolve_klass_ref(rec->rows[r].klass_ref_index,
                                    _klass_table);
      rtd->set_receiver(r, k);
      rtd->set_receiver_count(r, k != nullptr ? rec->rows[r].count : 0);
    }
    break;
  }
  case DataLayout::multi_branch_data_tag: {
    MultiBranchData* mbd = live->as_MultiBranchData();
    mbd->set_default_count(rec->default_count);
    for (int i = 0; i < rec->num_cases; i++) {
      mbd->set_count_at(i, rec->cases[i].count);
      // displacement already correct
    }
    break;
  }
  case DataLayout::ret_data_tag: {
    RetData* rd = live->as_RetData();
    // RetData rows: just patch counts, displacements recomputed
    break;
  }
  case DataLayout::call_type_data_tag:
  case DataLayout::virtual_call_type_data_tag: {
    // Patch call counter, argument types, return type
    patch_call_type_data(live, rec);
    break;
  }
  default:
    // BitData, ArgInfoData — flags/trap_state already copied above
    break;
  }
}
```

### 3.7 Runtime State Fixup

```cpp
void MethodData::post_import_fixup() {
  // Recreate the extra data lock
  _extra_data_lock = new Mutex(Mutex::nosafepoint, "MDOExtraData_lock");

  // Clear JVMCI-specific state
  JVMCI_ONLY(_failed_speculations = nullptr);
  JVMCI_ONLY(_jvmci_ir_size = 0);

  // Reset hint to beginning
  _hint_di = first_di();
}
```

---

## Phase 4: JVM Integration

### 4.1 Primary Import Hook (Option A — during class linking)

Modify `Method::build_profiling_method_data()` in `method.cpp`:

```cpp
void Method::build_profiling_method_data(const methodHandle& method, TRAPS) {
  // Existing: check CDS training data
  if (install_training_method_data(method)) {
    return;
  }

  // NEW: check for imported portable MDO
  if (PortableMDO::has_import_data()) {
    MethodData* imported = PortableMDO::try_import(method, THREAD);
    if (imported != nullptr) {
      if (Atomic::replace_if_null(&method->_method_data, imported)) {
        log_info(mdo, import)("Installed imported MDO for %s",
                               method->name_and_sig_as_C_string());
        return;
      } else {
        // Someone else installed first — free ours
        ClassLoaderData* ld = method->method_holder()->class_loader_data();
        MetadataFactory::free_metadata(ld, imported);
        return;
      }
    }
    // Import failed (fingerprint mismatch, etc.) — fall through
  }

  // ... existing allocation path ...
}
```

This is safe because `build_profiling_method_data` is called before the
method's MDO is visible to interpreting threads. The `Atomic::replace_if_null`
CAS provides the same safety guarantee as the existing path.

### 4.2 Late Import Path (Option C — for already-running methods)

```cpp
bool PortableMDO::late_import(const methodHandle& method, TRAPS) {
  PortableMDOEntry* entry = lookup(method);
  if (entry == nullptr) return false;

  MethodData* new_mdo = reconstruct(method, entry, CHECK_false);
  if (new_mdo == nullptr) return false;

  MethodData* old_mdo = method->method_data();
  if (old_mdo == nullptr) {
    // Easy case: no existing MDO
    if (!Atomic::replace_if_null(&method->_method_data, new_mdo)) {
      MetadataFactory::free_metadata(..., new_mdo);
    }
  } else {
    // Replace existing MDO.
    // The interpreter caches mdp at method entry, so after the store:
    // - New invocations see new_mdo
    // - Existing activations safely finish with old_mdo
    Atomic::store(&method->_method_data, new_mdo);
    // old_mdo must be freed after all existing activations complete.
    // Defer to next safepoint via ServiceThread or GC cycle.
    DeferredMetadataFree::enqueue(old_mdo);
  }
  return true;
}
```

### 4.3 VM Flags

```
New product flags (in globals.hpp):

  product(ccstr, ExportMDOFile, nullptr,                         \
          "Export mature MDO profiles to this file on JVM shutdown") \

  product(ccstr, ImportMDOFile, nullptr,                         \
          "Import MDO profiles from this file on JVM startup")    \

  product(intx, MDOImportScaleTarget, 10000,                     \
          "Target invocation count for scaled MDO import")        \
          range(100, 1000000)                                     \

  product(uint, MDOImportDeoptDecayPercent, 50,                  \
          "Percentage to decay deopt trap counts on import")      \
          range(0, 100)                                           \

  product(uint, MDOImportDecompileCap, 0,                        \
          "Max decompile count to import (0 = auto-cap at "       \
          "PerMethodRecompilationCutoff/4)")                      \

  develop(bool, MDOImportVerbose, false,                         \
          "Verbose logging for MDO import/export")                \
```

### 4.4 Export Trigger

```cpp
// Register during VM initialization:
// In Threads::create_vm() or similar:
if (ExportMDOFile != nullptr) {
  // Register a before_exit hook
  register_before_exit_function(PortableMDO::export_all);
}

// The export itself:
void PortableMDO::export_all() {
  ResourceMark rm;
  fileStream out(ExportMDOFile, "wb");
  if (!out.is_open()) {
    log_error(mdo, export)("Failed to open %s for writing", ExportMDOFile);
    return;
  }

  int count = 0;
  // Iterate all loaded classes and their methods
  ClassLoaderDataGraph::classes_do([&](Klass* k) {
    if (!k->is_instance_klass()) return;
    InstanceKlass* ik = InstanceKlass::cast(k);
    for (int i = 0; i < ik->methods()->length(); i++) {
      Method* m = ik->methods()->at(i);
      MethodData* mdo = m->method_data();
      if (mdo != nullptr && mdo->is_mature()) {
        export_single_mdo(mdo, &out);
        count++;
      }
    }
  });
  log_info(mdo, export)("Exported %d mature MDO profiles to %s",
                         count, ExportMDOFile);
}
```

### 4.5 Import Trigger

```cpp
// Called early in VM init, after SystemDictionary is ready:
void PortableMDO::initialize_import() {
  if (ImportMDOFile == nullptr) return;

  _import_file = os::map_memory(ImportMDOFile, ...);
  if (_import_file == nullptr) {
    log_error(mdo, import)("Failed to mmap %s", ImportMDOFile);
    return;
  }

  if (!validate_global_header()) {
    log_error(mdo, import)("Header validation failed for %s", ImportMDOFile);
    os::unmap_memory(_import_file, ...);
    _import_file = nullptr;
    return;
  }

  // Parse tables and build lookup map
  parse_klass_ref_table();
  parse_method_ref_table();
  build_entry_lookup_map();  // HashMap<Symbol*, PortableMDOEntry*>

  log_info(mdo, import)("Loaded %d MDO profiles from %s",
                         _entry_count, ImportMDOFile);
}
```

The lookup table stays in memory. As methods are linked and
`build_profiling_method_data` is called, each method checks the
table for an available imported profile. This is lazy — no class loading
is forced, no upfront resolution.

---

## Phase 5: Testing & Diagnostics

### 5.1 Logging Tags

```
-Xlog:mdo+export=info    — one line per exported method
-Xlog:mdo+export=debug   — per-BCI record details
-Xlog:mdo+import=info    — one line per imported method + rejection reasons
-Xlog:mdo+import=debug   — per-BCI patching details, unresolved klasses
```

### 5.2 WhiteBox API

```java
// In sun.hotspot.WhiteBox:
public native boolean exportMethodMDO(Executable method, String path);
public native boolean importMethodMDO(Executable method, String path);
public native int     getMethodBytecodeFingerprint(Executable method);
public native boolean hasImportedMDO(Executable method);
```

### 5.3 gtest (C++ unit tests)

File: `test/hotspot/gtest/oops/test_portableMDO.cpp`

```
TEST(PortableMDO, bytecode_fingerprint_stable)
  - Compute fingerprint twice for same method → same result
  - Redefine method → different result

TEST(PortableMDO, export_import_round_trip)
  - Create a method, build MDO, run interpreter to populate counters
  - Export to buffer
  - Allocate fresh MDO for same method
  - Import from buffer
  - Verify counters, receiver types, branch ratios match (within scaling)

TEST(PortableMDO, hidden_class_discarded)
  - Create MDO with a hidden-class receiver
  - Export → verify hidden class not in KlassRefTable
  - Import → receiver slot is null, overflow count preserved

TEST(PortableMDO, fingerprint_mismatch_rejects)
  - Export MDO for method M
  - Redefine M (different bytecodes)
  - Import → returns nullptr, log message emitted

TEST(PortableMDO, counter_scaling)
  - Export MDO with invocation_count=500
  - Import with MDOImportScaleTarget=10000
  - All counters scaled by 20x, ratios preserved within rounding

TEST(PortableMDO, deopt_decay)
  - Export MDO with trap_hist[Reason_class_check]=10, decompiles=8
  - Import with 50% decay
  - trap_hist[Reason_class_check]=5, decompiles=min(4, cutoff/4)
```

### 5.4 jtreg Tests

File: `test/hotspot/jtreg/runtime/PortableMDO/`

```
TestExportImportRoundTrip.java
  - Run app with -XX:ExportMDOFile=profile.mdo
  - Restart with -XX:ImportMDOFile=profile.mdo
  - Verify method compiles at C2 earlier (check compilation log)

TestStaleFingerprint.java
  - Export profiles for version 1 of a class
  - Load version 2 (different bytecodes) with import
  - Verify stale profiles rejected (check log output)

TestHiddenClassDiscard.java
  - Lambda-heavy workload
  - Export → import → verify no crashes, lambdas re-profile

TestConcurrentImport.java
  - Start threads executing a method
  - Concurrently trigger late MDO import via WhiteBox
  - Verify no crashes, no data corruption (run under -XX:+DeoptimizeALot)
```

### 5.5 Benchmarking

```
Metrics to collect (Renaissance / DaCapo):
  - Time to first C2 compilation (with vs without import)
  - Total compilation count in first 30 seconds
  - Peak throughput (should be equivalent)
  - Number of deoptimizations (imported deopt history should reduce these)
  - Number of unresolved klass refs (expect <5% for typical apps)
```

---

## Implementation Order

| Step | Description | Files Modified/Created | Depends On |
|------|------------|----------------------|-----------|
| 1 | Format structs & constants | NEW: `portableMDO.hpp` | — |
| 2 | Bytecode fingerprint | `portableMDO.cpp` | 1 |
| 3 | KlassRefTable + MethodRefTable | `portableMDO.cpp` | 1 |
| 4 | Per-record export (all tag types) | `portableMDO.cpp` | 1, 3 |
| 5 | Header export + deopt decay | `portableMDO.cpp` | 1 |
| 6 | Bulk export + file I/O | `portableMDO.cpp` | 4, 5 |
| 7 | VM flags | `globals.hpp` | — |
| 8 | Export trigger (shutdown hook) | `portableMDO.cpp`, `thread.cpp` | 6, 7 |
| 9 | File parser + validation | `portableMDO.cpp` | 1 |
| 10 | Klass resolution | `portableMDO.cpp` | 3, 9 |
| 11 | Counter normalization | `portableMDO.cpp` | 9 |
| 12 | MDO reconstruction + patching | `portableMDO.cpp`, `methodData.hpp` | 9, 10, 11 |
| 13 | Import hook in build_profiling_method_data | `method.cpp` | 12 |
| 14 | Import trigger (startup) | `portableMDO.cpp`, `init.cpp` | 9, 13 |
| 15 | Late import (Option C) | `portableMDO.cpp` | 12 |
| 16 | Logging | `portableMDO.cpp` | 6, 12 |
| 17 | WhiteBox API | `whitebox.cpp` | 6, 12 |
| 18 | gtests | NEW: `test_portableMDO.cpp` | 6, 12 |
| 19 | jtreg tests | NEW: `test/hotspot/jtreg/runtime/PortableMDO/` | 8, 14, 17 |
| 20 | Benchmarking | — | 14 |

Recommended: implement steps 1–8 (export path) first as a standalone
deliverable, then 9–14 (import path), then 15–20 (late import, tests, polish).
