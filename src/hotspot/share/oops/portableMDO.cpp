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

#include "classfile/classLoader.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/symbolTable.hpp"
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/klass.hpp"
#include "oops/method.hpp"
#include "oops/methodData.hpp"
#include "oops/portableMDO.hpp"
#include "runtime/globals.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/vm_version.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/resourceHash.hpp"

// --------------------------------------------------------------------------
// PortableMDOWriter — binary stream writer
// --------------------------------------------------------------------------

class PortableMDOWriter : public StackObj {
  FILE* _file;
  bool  _error;
  size_t _bytes_written;

public:
  PortableMDOWriter(FILE* file) : _file(file), _error(false), _bytes_written(0) {}

  bool error() const { return _error; }
  size_t bytes_written() const { return _bytes_written; }

  void write_raw(const void* data, size_t len) {
    if (_error) return;
    size_t written = fwrite(data, 1, len, _file);
    if (written != len) {
      _error = true;
    }
    _bytes_written += written;
  }

  void write_u1(uint8_t v)  { write_raw(&v, sizeof(v)); }
  void write_u2(uint16_t v) { write_raw(&v, sizeof(v)); }
  void write_u4(uint32_t v) { write_raw(&v, sizeof(v)); }
  void write_i2(int16_t v)  { write_raw(&v, sizeof(v)); }
  void write_i4(int32_t v)  { write_raw(&v, sizeof(v)); }

  void write_struct(const void* s, size_t len) { write_raw(s, len); }

  void write_utf8(const char* str, uint16_t len) {
    write_u2(len);
    write_raw(str, len);
  }

  // Pad to 4-byte alignment
  void align4() {
    size_t rem = _bytes_written % 4;
    if (rem != 0) {
      uint8_t pad[4] = {0};
      write_raw(pad, 4 - rem);
    }
  }

  size_t position() const { return _bytes_written; }

  // Seek back to a position and write, then return to current position
  void patch_u4_at(size_t offset, uint32_t value) {
    if (_error) return;
    long current = ftell(_file);
    if (fseek(_file, (long)offset, SEEK_SET) != 0) {
      _error = true;
      return;
    }
    fwrite(&value, sizeof(value), 1, _file);
    if (fseek(_file, current, SEEK_SET) != 0) {
      _error = true;
    }
  }
};

// --------------------------------------------------------------------------
// Reference table builders
// --------------------------------------------------------------------------

class KlassRefTableBuilder : public StackObj {
  struct Entry {
    const Symbol* name;
    PortableClassLoaderTag loader_tag;
  };

  GrowableArray<Entry> _entries;

  // Simple linear search is fine — tables are small (hundreds of entries max)
  int find(const Symbol* name, PortableClassLoaderTag tag) const {
    for (int i = 0; i < _entries.length(); i++) {
      if (_entries.at(i).name == name && _entries.at(i).loader_tag == tag) {
        return i;
      }
    }
    return -1;
  }

public:
  KlassRefTableBuilder() : _entries() {}

  static PortableClassLoaderTag classloader_tag(const Klass* k) {
    ClassLoaderData* cld = k->class_loader_data();
    // is_the_null_class_loader_data() covers the boot classloader
    if (cld == nullptr || cld->is_the_null_class_loader_data()) {
      return PortableClassLoaderTag::BOOT;
    }
    if (cld->is_platform_class_loader_data()) {
      return PortableClassLoaderTag::PLATFORM;
    }
    if (cld->is_system_class_loader_data()) {
      return PortableClassLoaderTag::APP;
    }
    return PortableClassLoaderTag::CUSTOM;
  }

  // Returns index into table, or PortableMDO::NULL_KLASS_REF if discarded.
  int16_t add_or_discard(const Klass* k) {
    if (k == nullptr) return PortableMDO::NULL_KLASS_REF;
    if (k->is_hidden()) return PortableMDO::NULL_KLASS_REF;
    if (k->class_loader_data() == nullptr || !k->class_loader_data()->is_alive()) {
      return PortableMDO::NULL_KLASS_REF;
    }

    PortableClassLoaderTag tag = classloader_tag(k);
    const Symbol* name = k->name();

    int idx = find(name, tag);
    if (idx >= 0) return (int16_t)idx;

    if (_entries.length() >= PortableMDO::MAX_KLASS_REF_COUNT) {
      return PortableMDO::NULL_KLASS_REF;
    }

    Entry e;
    e.name = name;
    e.loader_tag = tag;
    idx = _entries.length();
    _entries.append(e);
    return (int16_t)idx;
  }

  int length() const { return _entries.length(); }

  void write_to(PortableMDOWriter* writer) const {
    for (int i = 0; i < _entries.length(); i++) {
      const Entry& e = _entries.at(i);
      PortableKlassRefEntry hdr;
      hdr.loader_tag = e.loader_tag;
      hdr._padding0 = 0;
      hdr.name_length = (uint16_t)e.name->utf8_length();
      writer->write_struct(&hdr, sizeof(hdr));
      writer->write_raw((const char*)e.name->bytes(), hdr.name_length);
      writer->align4();
    }
  }
};

class MethodRefTableBuilder : public StackObj {
  struct Entry {
    int16_t klass_ref_index;
    const Symbol* name;
    const Symbol* sig;
  };

  GrowableArray<Entry> _entries;

  int find(int16_t klass_idx, const Symbol* name, const Symbol* sig) const {
    for (int i = 0; i < _entries.length(); i++) {
      if (_entries.at(i).klass_ref_index == klass_idx &&
          _entries.at(i).name == name &&
          _entries.at(i).sig == sig) {
        return i;
      }
    }
    return -1;
  }

public:
  MethodRefTableBuilder() : _entries() {}

  int16_t add_or_discard(const Method* m, KlassRefTableBuilder* klass_table) {
    if (m == nullptr) return PortableMDO::NULL_METHOD_REF;

    int16_t klass_idx = klass_table->add_or_discard(m->method_holder());
    if (klass_idx == PortableMDO::NULL_KLASS_REF) {
      return PortableMDO::NULL_METHOD_REF;
    }

    const Symbol* name = m->name();
    const Symbol* sig = m->signature();

    int idx = find(klass_idx, name, sig);
    if (idx >= 0) return (int16_t)idx;

    if (_entries.length() >= PortableMDO::MAX_METHOD_REF_COUNT) {
      return PortableMDO::NULL_METHOD_REF;
    }

    Entry e;
    e.klass_ref_index = klass_idx;
    e.name = name;
    e.sig = sig;
    idx = _entries.length();
    _entries.append(e);
    return (int16_t)idx;
  }

  int length() const { return _entries.length(); }

  void write_to(PortableMDOWriter* writer) const {
    for (int i = 0; i < _entries.length(); i++) {
      const Entry& e = _entries.at(i);
      PortableMethodRefEntry hdr;
      hdr.klass_ref_index = (uint16_t)e.klass_ref_index;
      hdr.name_length = (uint16_t)e.name->utf8_length();
      hdr.sig_length = (uint16_t)e.sig->utf8_length();
      hdr._padding0 = 0;
      writer->write_struct(&hdr, sizeof(hdr));
      writer->write_raw((const char*)e.name->bytes(), hdr.name_length);
      writer->write_raw((const char*)e.sig->bytes(), hdr.sig_length);
      writer->align4();
    }
  }
};

// --------------------------------------------------------------------------
// Bytecode fingerprint
// --------------------------------------------------------------------------

static uint32_t compute_bytecode_fingerprint(const Method* method) {
  int code_size = method->code_size();
  if (code_size == 0) return 0;
  address code_base = method->constMethod()->code_base();
  return (uint32_t)ClassLoader::crc32(0, (const char*)code_base, code_size);
}

// --------------------------------------------------------------------------
// Type entry export helpers
// --------------------------------------------------------------------------

static void export_symbolic_type_entry(PortableMDOWriter* writer,
                                       intptr_t raw_type,
                                       uint16_t stack_slot,
                                       KlassRefTableBuilder* klass_table) {
  PortableSymbolicTypeEntry entry;
  entry.stack_slot = stack_slot;
  entry.null_seen = TypeEntries::was_null_seen(raw_type) ? 1 : 0;
  entry.type_unknown = TypeEntries::is_type_unknown(raw_type) ? 1 : 0;
  entry._padding0 = 0;

  Klass* k = TypeEntries::valid_klass(raw_type);
  entry.klass_ref_index = klass_table->add_or_discard(k);

  writer->write_struct(&entry, sizeof(entry));
}

static void export_type_stack_slot_entries(PortableMDOWriter* writer,
                                           const TypeStackSlotEntries* args,
                                           KlassRefTableBuilder* klass_table) {
  for (int i = 0; i < args->number_of_entries(); i++) {
    export_symbolic_type_entry(writer, args->type(i),
                               (uint16_t)args->stack_slot(i), klass_table);
  }
}

static void export_return_type_entry(PortableMDOWriter* writer,
                                      const ReturnTypeEntry* ret,
                                      KlassRefTableBuilder* klass_table) {
  export_symbolic_type_entry(writer, ret->type(), 0, klass_table);
}

// --------------------------------------------------------------------------
// Per-BCI profile record export
// --------------------------------------------------------------------------

static void export_profile_record(PortableMDOWriter* writer,
                                   ProfileData* data,
                                   KlassRefTableBuilder* klass_table) {
  // Write common header
  DataLayout* dl = (DataLayout*)data->dp();
  PortableProfileRecordHeader hdr;
  hdr.tag = dl->tag();
  hdr.flags = dl->flags();
  hdr.bci = (uint16_t)data->bci();
  hdr.trap_state = data->trap_state();
  writer->write_struct(&hdr, sizeof(hdr));

  switch (hdr.tag) {
    case DataLayout::bit_data_tag: {
      // No payload beyond header
      break;
    }

    case DataLayout::counter_data_tag: {
      CounterData* cd = data->as_CounterData();
      PortableCounterDataPayload payload;
      payload.count = (int32_t)cd->count();
      writer->write_struct(&payload, sizeof(payload));
      break;
    }

    case DataLayout::jump_data_tag: {
      JumpData* jd = data->as_JumpData();
      PortableJumpDataPayload payload;
      payload.taken = jd->taken();
      writer->write_struct(&payload, sizeof(payload));
      break;
    }

    case DataLayout::receiver_type_data_tag:
    case DataLayout::virtual_call_data_tag: {
      ReceiverTypeData* rtd = data->as_ReceiverTypeData();
      uint rows = rtd->row_limit();
      uint actual_rows = 0;
      // Count non-null rows (but we write all rows for positional stability)
      for (uint r = 0; r < rows; r++) {
        if (rtd->receiver(r) != nullptr) actual_rows++;
      }
      (void)actual_rows; // used only for diagnostics

      PortableReceiverTypeDataPayload payload;
      payload.count = rtd->count();
      payload.num_rows = (uint8_t)rows;
      payload._padding0[0] = payload._padding0[1] = payload._padding0[2] = 0;
      writer->write_struct(&payload, sizeof(payload));

      for (uint r = 0; r < rows; r++) {
        PortableReceiverRow row;
        Klass* k = rtd->receiver(r);
        row.klass_ref_index = klass_table->add_or_discard(k);
        row._padding0 = 0;
        row.count = rtd->receiver_count(r);
        // If klass was discarded (hidden class), zero the per-row count
        // but preserve the overflow count in the payload header
        if (k != nullptr && row.klass_ref_index == PortableMDO::NULL_KLASS_REF) {
          row.count = 0;
        }
        writer->write_struct(&row, sizeof(row));
      }

      // If this is VirtualCallTypeData, also export arg/return types
      if (hdr.tag == DataLayout::virtual_call_type_data_tag) {
        // Handled below in virtual_call_type_data_tag case
      }
      break;
    }

    case DataLayout::ret_data_tag: {
      RetData* rd = data->as_RetData();
      PortableRetDataPayload payload;
      payload.count = rd->count();
      uint rows = RetData::row_limit();
      payload.num_rows = (uint8_t)rows;
      payload._padding0[0] = payload._padding0[1] = payload._padding0[2] = 0;
      writer->write_struct(&payload, sizeof(payload));

      for (uint r = 0; r < rows; r++) {
        PortableRetRow row;
        row.target_bci = (int16_t)rd->bci(r);
        row._padding0 = 0;
        row.count = rd->bci_count(r);
        writer->write_struct(&row, sizeof(row));
      }
      break;
    }

    case DataLayout::branch_data_tag: {
      BranchData* bd = data->as_BranchData();
      PortableBranchDataPayload payload;
      payload.taken = bd->taken();
      payload.not_taken = bd->not_taken();
      writer->write_struct(&payload, sizeof(payload));
      break;
    }

    case DataLayout::multi_branch_data_tag: {
      MultiBranchData* mbd = data->as_MultiBranchData();
      int ncases = mbd->number_of_cases();
      PortableMultiBranchDataPayload payload;
      payload.default_count = mbd->default_count();
      payload.num_cases = (uint16_t)ncases;
      payload._padding0 = 0;
      writer->write_struct(&payload, sizeof(payload));

      for (int i = 0; i < ncases; i++) {
        uint32_t cnt = mbd->count_at(i);
        writer->write_u4(cnt);
      }
      break;
    }

    case DataLayout::call_type_data_tag: {
      CallTypeData* ctd = data->as_CallTypeData();
      PortableCallTypeDataPayload payload;
      payload.count = (int32_t)ctd->count();
      payload.num_args = ctd->has_arguments() ? (uint8_t)ctd->number_of_arguments() : 0;
      payload.has_return = ctd->has_return() ? 1 : 0;
      payload._padding0 = 0;
      writer->write_struct(&payload, sizeof(payload));

      if (ctd->has_arguments()) {
        export_type_stack_slot_entries(writer, ctd->args(), klass_table);
      }
      if (ctd->has_return()) {
        export_return_type_entry(writer, ctd->ret(), klass_table);
      }
      break;
    }

    case DataLayout::virtual_call_type_data_tag: {
      VirtualCallTypeData* vctd = data->as_VirtualCallTypeData();

      // Receiver type portion
      uint rows = vctd->row_limit();
      PortableVirtualCallTypeDataPayload payload;
      payload.count = vctd->count();
      payload.num_receiver_rows = (uint8_t)rows;
      payload.num_args = vctd->has_arguments() ? (uint8_t)vctd->number_of_arguments() : 0;
      payload.has_return = vctd->has_return() ? 1 : 0;
      payload._padding0 = 0;
      writer->write_struct(&payload, sizeof(payload));

      for (uint r = 0; r < rows; r++) {
        PortableReceiverRow row;
        Klass* k = vctd->receiver(r);
        row.klass_ref_index = klass_table->add_or_discard(k);
        row._padding0 = 0;
        row.count = vctd->receiver_count(r);
        if (k != nullptr && row.klass_ref_index == PortableMDO::NULL_KLASS_REF) {
          row.count = 0;
        }
        writer->write_struct(&row, sizeof(row));
      }

      if (vctd->has_arguments()) {
        export_type_stack_slot_entries(writer, vctd->args(), klass_table);
      }
      if (vctd->has_return()) {
        export_return_type_entry(writer, vctd->ret(), klass_table);
      }
      break;
    }

    case DataLayout::parameters_type_data_tag: {
      // Parameters are handled separately in export_parameters
      // This tag shouldn't appear in the main data walk
      break;
    }

    default: {
      // Unknown tag — skip (write nothing beyond the header)
      break;
    }
  }
}

// --------------------------------------------------------------------------
// Extra data export
// --------------------------------------------------------------------------

static int export_extra_data(PortableMDOWriter* writer,
                              MethodData* mdo,
                              KlassRefTableBuilder* klass_table,
                              MethodRefTableBuilder* method_table) {
  int count = 0;
  Mutex* lock = mdo->extra_data_lock();
  MutexLocker ml(lock, Mutex::_no_safepoint_check_flag);

  DataLayout* dp = mdo->extra_data_base();
  DataLayout* end = mdo->extra_data_limit();

  for (; dp < end; dp = MethodData::next_extra(dp)) {
    if (dp->tag() == DataLayout::no_tag ||
        dp->tag() == DataLayout::arg_info_data_tag) {
      break;
    }

    PortableExtraRecordHeader ehdr;
    ehdr.bci = (uint16_t)dp->bci();
    ehdr.trap_state = dp->trap_state();
    ehdr.flags = dp->flags();

    if (dp->tag() == DataLayout::speculative_trap_data_tag) {
      SpeculativeTrapData* data = new SpeculativeTrapData(dp);
      Method* m = data->method();
      int16_t method_idx = method_table->add_or_discard(m, klass_table);

      ehdr.tag = PortableExtraTag::SPECULATIVE_TRAP;
      writer->write_struct(&ehdr, sizeof(ehdr));

      PortableSpeculativeTrapPayload payload;
      payload.method_ref_index = method_idx;
      payload._padding0 = 0;
      writer->write_struct(&payload, sizeof(payload));
      count++;
    } else if (dp->tag() == DataLayout::bit_data_tag) {
      ehdr.tag = PortableExtraTag::BIT_DATA;
      writer->write_struct(&ehdr, sizeof(ehdr));
      count++;
    }
  }
  return count;
}

// --------------------------------------------------------------------------
// Parameters type data export
// --------------------------------------------------------------------------

static int export_parameters(PortableMDOWriter* writer,
                              MethodData* mdo,
                              KlassRefTableBuilder* klass_table) {
  ParametersTypeData* params = mdo->parameters_type_data();
  if (params == nullptr) return 0;

  const TypeStackSlotEntries* entries = params->parameters();
  int count = params->number_of_parameters();
  for (int i = 0; i < count; i++) {
    intptr_t raw_type = entries->type(i);
    uint16_t stack_slot = (uint16_t)entries->stack_slot(i);
    export_symbolic_type_entry(writer, raw_type, stack_slot, klass_table);
  }
  return count;
}

// --------------------------------------------------------------------------
// Exception handler data export
// --------------------------------------------------------------------------

static int export_exception_handlers(PortableMDOWriter* writer,
                                      MethodData* mdo) {
  DataLayout* dp = mdo->exception_handler_data_base();
  DataLayout* end = mdo->exception_handler_data_limit();
  // Each exception handler entry is a fixed-size BitData
  int entry_size = DataLayout::compute_size_in_bytes(BitData::static_cell_count());
  int count = 0;
  while (dp < end) {
    BitData data(dp);
    PortableExceptionHandlerEntry entry;
    entry.handler_bci = (uint16_t)dp->bci();
    entry.entered = data.exception_handler_entered() ? 1 : 0;
    entry._padding0 = 0;
    writer->write_struct(&entry, sizeof(entry));
    count++;
    dp = (DataLayout*)((address)dp + entry_size);
  }
  return count;
}

// --------------------------------------------------------------------------
// ArgInfoData export
// --------------------------------------------------------------------------

static int export_arg_modified(PortableMDOWriter* writer,
                                MethodData* mdo) {
  int count = mdo->method()->size_of_parameters();
  if (count == 0) return 0;

  for (int i = 0; i < count; i++) {
    uint8_t modified = (uint8_t)mdo->arg_modified(i);
    writer->write_u1(modified);
  }
  writer->align4();
  return count;
}

// --------------------------------------------------------------------------
// Header fields export
// --------------------------------------------------------------------------

static void export_header_fields(PortableMDOWriter* writer,
                                  MethodData* mdo,
                                  float deopt_decay) {
  PortableMDOHeaderFields hdr;
  memset(&hdr, 0, sizeof(hdr));

  // Counters
  hdr.invocation_count = (int32_t)mdo->invocation_count();
  hdr.backedge_count = (int32_t)mdo->backedge_count();

  // Compiler counters with decay
  uint raw_decompiles = mdo->decompile_count();
  hdr.nof_decompiles = (uint16_t)MIN2((uint)(raw_decompiles * deopt_decay),
                                       (uint)UINT16_MAX);
  hdr.nof_overflow_recompiles = 0; // reset — less meaningful across runs
  hdr.nof_overflow_traps = 0;      // reset
  hdr.tenure_traps = (uint16_t)MIN2(mdo->tenure_traps(), (uint)UINT16_MAX);

  // Trap histogram with decay
  for (uint i = 0; i < MethodData::trap_reason_limit(); i++) {
    if (i >= PortableMDO::MAX_TRAP_HIST_LENGTH) break;
    uint raw = mdo->trap_count(i);
    if (raw == (uint)-1) {
      // Saturated — apply decay to max
      hdr.trap_hist[i] = (uint8_t)MIN2((uint)(255 * deopt_decay), (uint)255);
    } else {
      hdr.trap_hist[i] = (uint8_t)MIN2((uint)(raw * deopt_decay), (uint)255);
    }
  }

  // Structural info
  hdr.num_loops = (int16_t)mdo->num_loops();
  hdr.num_blocks = (int16_t)mdo->num_blocks();
  hdr.would_profile = mdo->would_profile() ? 2 : 1;

  // Escape analysis fields intentionally zeroed — recomputed by C2

  // arg_modified_count is written separately after this struct
  hdr.arg_modified_count = (uint16_t)mdo->method()->size_of_parameters();

  writer->write_struct(&hdr, sizeof(hdr));
}

// --------------------------------------------------------------------------
// Single MDO entry export
// --------------------------------------------------------------------------

static bool export_single_mdo(PortableMDOWriter* writer,
                               MethodData* mdo,
                               KlassRefTableBuilder* klass_table,
                               MethodRefTableBuilder* method_table,
                               float deopt_decay) {
  ResourceMark rm;
  Method* method = mdo->method();

  // Write method identity
  PortableMDOMethodIdentity identity;
  identity.klass_ref_index = (uint16_t)klass_table->add_or_discard(method->method_holder());
  const Symbol* mname = method->name();
  const Symbol* msig = method->signature();
  identity.name_length = (uint16_t)mname->utf8_length();
  identity.sig_length = (uint16_t)msig->utf8_length();
  identity._padding0 = 0;
  identity.bytecode_fingerprint = compute_bytecode_fingerprint(method);
  writer->write_struct(&identity, sizeof(identity));
  writer->write_raw((const char*)mname->bytes(), identity.name_length);
  writer->write_raw((const char*)msig->bytes(), identity.sig_length);
  writer->align4();

  // Write header fields
  export_header_fields(writer, mdo, deopt_decay);

  // Write arg_modified bytes inline after header
  int arg_count = export_arg_modified(writer, mdo);
  (void)arg_count;

  // We need to write counts before the records, but we don't know them yet.
  // Reserve space for the counts struct and patch it later.
  size_t counts_offset = writer->position();
  PortableMDOEntryCounts counts;
  memset(&counts, 0, sizeof(counts));
  writer->write_struct(&counts, sizeof(counts));

  // Write per-BCI profile records
  uint16_t record_count = 0;
  for (ProfileData* data = mdo->first_data();
       mdo->is_valid(data);
       data = mdo->next_data(data)) {
    export_profile_record(writer, data, klass_table);
    record_count++;
  }

  // Write extra data records
  uint16_t extra_count = (uint16_t)export_extra_data(writer, mdo, klass_table, method_table);

  // Write parameter types
  uint16_t param_count = (uint16_t)export_parameters(writer, mdo, klass_table);

  // Write exception handler data
  uint16_t handler_count = (uint16_t)export_exception_handlers(writer, mdo);

  // Patch the counts
  counts.record_count = record_count;
  counts.extra_record_count = extra_count;
  counts.param_type_count = param_count;
  counts.exception_handler_count = handler_count;
  writer->patch_u4_at(counts_offset, *(uint32_t*)&counts);
  writer->patch_u4_at(counts_offset + 4, *((uint32_t*)&counts + 1));

  return !writer->error();
}

// --------------------------------------------------------------------------
// JDK version hash
// --------------------------------------------------------------------------

static uint32_t compute_jdk_version_hash() {
  const char* version = VM_Version::internal_vm_info_string();
  if (version == nullptr) return 0;
  return (uint32_t)ClassLoader::crc32(0, version, (int)strlen(version));
}

// --------------------------------------------------------------------------
// MDO collection helper
// --------------------------------------------------------------------------

// File-scope list pointer used by the methods_do callback below.
// Safe without synchronization because export runs at a safepoint
// where only the VM thread is active.
static GrowableArray<MethodData*>* _collect_mdo_list = nullptr;

static void collect_mature_mdo_callback(Method* m) {
  MethodData* mdo = m->method_data();
  if (mdo != nullptr && mdo->is_mature()) {
    _collect_mdo_list->append(mdo);
  }
}

static void collect_mature_mdos(GrowableArray<MethodData*>* list) {
  assert_at_safepoint();
  _collect_mdo_list = list;
  ClassLoaderDataGraph::methods_do(collect_mature_mdo_callback);
  _collect_mdo_list = nullptr;
}

// --------------------------------------------------------------------------
// Bulk export — public entry point
// --------------------------------------------------------------------------

bool PortableMDO::export_all_to_file(const char* filepath, float deopt_decay) {
  assert_at_safepoint();
  ResourceMark rm;

  FILE* file = os::fopen(filepath, "wb");
  if (file == nullptr) {
    log_error(aot, training)("PortableMDO: failed to open %s for writing", filepath);
    return false;
  }

  PortableMDOWriter writer(file);
  KlassRefTableBuilder klass_table;
  MethodRefTableBuilder method_table;

  // Write a placeholder file header — we'll patch offsets at the end
  size_t header_offset = writer.position();
  PortableMDOFileHeader file_header;
  memset(&file_header, 0, sizeof(file_header));
  file_header.magic = PortableMDO::MAGIC;
  file_header.format_version = PortableMDO::FORMAT_VERSION;
  file_header.jdk_version_hash = compute_jdk_version_hash();
  file_header.pointer_size = (uint8_t)sizeof(void*);
#ifdef VM_LITTLE_ENDIAN
  file_header.endianness = 0;
#else
  file_header.endianness = 1;
#endif
  file_header.type_profile_width = (uint16_t)TypeProfileWidth;
  file_header.method_profile_width = 0; // placeholder — not currently a tunable
  file_header.bci_profile_width = (uint16_t)BciProfileWidth;
  file_header.profile_traps = ProfileTraps ? 1 : 0;
  file_header.profile_exception_handlers = ProfileExceptionHandlers ? 1 : 0;
  file_header.trap_hist_length = (uint8_t)MethodData::trap_reason_limit();
  writer.write_struct(&file_header, sizeof(file_header));

  // Collect all mature MDOs into a temporary list (to avoid holding
  // classloader locks while doing I/O)
  GrowableArray<MethodData*> mdo_list;
  collect_mature_mdos(&mdo_list);

  // Phase 1: Export all MDO entries. This populates the klass/method ref
  // tables as a side effect.
  size_t mdo_entries_offset = writer.position();
  int exported_count = 0;
  for (int i = 0; i < mdo_list.length(); i++) {
    MethodData* mdo = mdo_list.at(i);
    // Double-check the method is still alive
    if (mdo->method() == nullptr) continue;
    if (export_single_mdo(&writer, mdo, &klass_table, &method_table, deopt_decay)) {
      exported_count++;
    }
    if (writer.error()) break;
  }

  if (writer.error()) {
    fclose(file);
    log_error(aot, training)("PortableMDO: I/O error writing %s", filepath);
    return false;
  }

  // Phase 2: Write klass ref table
  size_t klass_table_offset = writer.position();
  klass_table.write_to(&writer);

  // Phase 3: Write method ref table
  size_t method_table_offset = writer.position();
  method_table.write_to(&writer);

  // Phase 4: Patch the file header with correct offsets and counts
  file_header.klass_ref_table_offset = (uint32_t)klass_table_offset;
  file_header.klass_ref_count = (uint32_t)klass_table.length();
  file_header.method_ref_table_offset = (uint32_t)method_table_offset;
  file_header.method_ref_count = (uint32_t)method_table.length();
  file_header.mdo_entries_offset = (uint32_t)mdo_entries_offset;
  file_header.mdo_entry_count = (uint32_t)exported_count;

  // Rewrite the header at offset 0
  if (fseek(file, (long)header_offset, SEEK_SET) != 0) {
    fclose(file);
    log_error(aot, training)("PortableMDO: failed to seek in %s", filepath);
    return false;
  }
  if (fwrite(&file_header, sizeof(file_header), 1, file) != 1) {
    fclose(file);
    log_error(aot, training)("PortableMDO: failed to rewrite header in %s", filepath);
    return false;
  }

  fclose(file);

  log_info(aot, training)("PortableMDO: exported %d mature MDO profiles to %s "
           "(%d klass refs, %d method refs, %zu bytes)",
           exported_count, filepath,
           klass_table.length(), method_table.length(),
           writer.bytes_written());
  return true;
}
