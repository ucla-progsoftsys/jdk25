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
#include "classfile/systemDictionary.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/compileTask.hpp"
#include "compiler/compilationPolicy.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/metadataFactory.hpp"
#include "memory/resourceArea.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/klass.hpp"
#include "oops/method.hpp"
#include "oops/methodCounters.hpp"
#include "oops/methodData.hpp"
#include "oops/portableMDO.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/vmOperation.hpp"
#include "runtime/vmThread.hpp"
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

  void patch_struct_at(size_t offset, const void* data, size_t len) {
    if (_error) return;
    long current = ftell(_file);
    if (fseek(_file, (long)offset, SEEK_SET) != 0) {
      _error = true;
      return;
    }
    if (fwrite(data, 1, len, _file) != len) {
      _error = true;
    }
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
    // ReceiverTypeData::receiver() returns raw cell values cast to Klass*.
    // Some cells may contain small non-pointer values (residual data from
    // cleared slots, or tagged intptr values). Reject anything that cannot
    // be a valid metaspace pointer.
    if ((uintptr_t)k < (uintptr_t)os::vm_page_size()) return PortableMDO::NULL_KLASS_REF;
    if (((uintptr_t)k & (alignof(Klass) - 1)) != 0) return PortableMDO::NULL_KLASS_REF;
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

uint32_t PortableMDO::compute_bytecode_fingerprint(const Method* method) {
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
      SpeculativeTrapData data(dp);
      Method* m = data.method();
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

// Walk extra data to find ArgInfoData. Acquires the extra data lock.
// Returns the DataLayout* for the ArgInfoData entry, or nullptr if not found.
// Caller constructs the ArgInfoData wrapper on the stack.
static DataLayout* find_arg_info_layout(MethodData* mdo) {
  Mutex* lock = mdo->extra_data_lock();
  MutexLocker ml(lock, Mutex::_no_safepoint_check_flag);
  DataLayout* dp = mdo->extra_data_base();
  DataLayout* end = mdo->args_data_limit();
  for (; dp < end; dp = MethodData::next_extra(dp)) {
    if (dp->tag() == DataLayout::arg_info_data_tag) {
      return dp;
    }
  }
  return nullptr;
}

static int export_arg_modified(PortableMDOWriter* writer,
                                MethodData* mdo) {
  int count = mdo->method()->size_of_parameters();
  if (count == 0) return 0;

  DataLayout* arg_dl = find_arg_info_layout(mdo);
  for (int i = 0; i < count; i++) {
    uint8_t modified = 0;
    if (arg_dl != nullptr) {
      ArgInfoData args(arg_dl);
      modified = (uint8_t)args.arg_modified(i);
    }
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

  // Highest compilation level this method reached
  MethodCounters* mc = mdo->method()->method_counters();
  hdr.highest_comp_level = (mc != nullptr) ? (uint8_t)mc->highest_comp_level() : 0;

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
  // NOTE: no ResourceMark here — the caller's ResourceMark (in
  // export_all_to_file) must stay alive so that the GrowableArrays
  // backing klass_table and method_table are not freed prematurely.
  Method* method = mdo->method();

  // Write method identity
  PortableMDOMethodIdentity identity;
  identity.klass_ref_index = (uint16_t)klass_table->add_or_discard(method->method_holder());
  const Symbol* mname = method->name();
  const Symbol* msig = method->signature();
  identity.name_length = (uint16_t)mname->utf8_length();
  identity.sig_length = (uint16_t)msig->utf8_length();
  identity._padding0 = 0;
  identity.bytecode_fingerprint = PortableMDO::compute_bytecode_fingerprint(method);
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
  writer->patch_struct_at(counts_offset, &counts, sizeof(counts));

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

// --------------------------------------------------------------------------
// VM_ExportMDO — safepoint VM operation for shutdown export
// --------------------------------------------------------------------------

class VM_ExportMDO : public VM_Operation {
  const char* _filepath;
  float _deopt_decay;
  bool _result;
public:
  VM_ExportMDO(const char* filepath, float deopt_decay)
    : _filepath(filepath), _deopt_decay(deopt_decay), _result(false) {}
  VMOp_Type type() const { return VMOp_ExportMDO; }
  void doit() {
    _result = PortableMDO::export_all_to_file(_filepath, _deopt_decay);
  }
  bool result() const { return _result; }
};

void PortableMDO::export_on_shutdown() {
  if (ExportMDOFile == nullptr) return;
  float decay = (float)MDOExportDeoptDecayPercent / 100.0f;
  VM_ExportMDO op(ExportMDOFile, decay);
  VMThread::execute(&op);
}

// ==========================================================================
//
//  PHASE 3: MDO IMPORTER
//
// ==========================================================================

// --------------------------------------------------------------------------
// PortableMDOReader — binary stream reader with bounds checking
// --------------------------------------------------------------------------

class PortableMDOReader : public StackObj {
  const uint8_t* _base;
  const uint8_t* _end;
  const uint8_t* _cursor;
  bool _error;

public:
  PortableMDOReader(const uint8_t* base, size_t length)
    : _base(base), _end(base + length), _cursor(base), _error(false) {}

  bool error() const { return _error; }
  size_t position() const { return (size_t)(_cursor - _base); }
  size_t remaining() const { return _error ? 0 : (size_t)(_end - _cursor); }

  void set_position(size_t offset) {
    if (offset > (size_t)(_end - _base)) {
      _error = true;
      return;
    }
    _cursor = _base + offset;
  }

  bool read_raw(void* dest, size_t len) {
    if (_error || _cursor + len > _end) {
      _error = true;
      return false;
    }
    memcpy(dest, _cursor, len);
    _cursor += len;
    return true;
  }

  uint8_t read_u1() {
    uint8_t v = 0;
    read_raw(&v, sizeof(v));
    return v;
  }

  uint16_t read_u2() {
    uint16_t v = 0;
    read_raw(&v, sizeof(v));
    return v;
  }

  uint32_t read_u4() {
    uint32_t v = 0;
    read_raw(&v, sizeof(v));
    return v;
  }

  int16_t read_i2() {
    int16_t v = 0;
    read_raw(&v, sizeof(v));
    return v;
  }

  int32_t read_i4() {
    int32_t v = 0;
    read_raw(&v, sizeof(v));
    return v;
  }

  bool read_struct(void* dest, size_t len) {
    return read_raw(dest, len);
  }

  // Skip len bytes
  void skip(size_t len) {
    if (_error || _cursor + len > _end) {
      _error = true;
      return;
    }
    _cursor += len;
  }

  // Read raw bytes without copying (returns pointer into the buffer)
  const uint8_t* read_bytes(size_t len) {
    if (_error || _cursor + len > _end) {
      _error = true;
      return nullptr;
    }
    const uint8_t* p = _cursor;
    _cursor += len;
    return p;
  }

  // Advance to 4-byte alignment
  void align4() {
    size_t pos = position();
    size_t rem = pos % 4;
    if (rem != 0) {
      skip(4 - rem);
    }
  }
};

// --------------------------------------------------------------------------
// Imported reference table entries (in-memory parsed form)
// --------------------------------------------------------------------------

struct ImportedKlassRef : public CHeapObj<mtInternal> {
  Symbol* name;            // Interned symbol — has refcount
  PortableClassLoaderTag loader_tag;
  Klass* resolved;         // Cached resolution result, or nullptr
  bool resolution_attempted;

  ImportedKlassRef() : name(nullptr), loader_tag(PortableClassLoaderTag::BOOT),
                       resolved(nullptr), resolution_attempted(false) {}
};

struct ImportedMethodRef : public CHeapObj<mtInternal> {
  int16_t klass_ref_index;
  Symbol* name;
  Symbol* sig;

  ImportedMethodRef() : klass_ref_index(-1), name(nullptr), sig(nullptr) {}
};

// --------------------------------------------------------------------------
// Lookup key for the MDO entry map: (klass_name, method_name, method_sig)
// --------------------------------------------------------------------------

struct MDOLookupKey {
  const Symbol* klass_name;
  const Symbol* method_name;
  const Symbol* method_sig;

  bool operator==(const MDOLookupKey& other) const {
    return klass_name == other.klass_name &&
           method_name == other.method_name &&
           method_sig == other.method_sig;
  }
};

static unsigned mdo_lookup_key_hash(const MDOLookupKey& key) {
  // Combine the Symbol* addresses — symbols are interned so pointer
  // identity == value identity.
  uintptr_t h = (uintptr_t)key.klass_name;
  h = h * 31 + (uintptr_t)key.method_name;
  h = h * 31 + (uintptr_t)key.method_sig;
  return (unsigned)(h ^ (h >> 16));
}

static bool mdo_lookup_key_equals(const MDOLookupKey& a, const MDOLookupKey& b) {
  return a == b;
}

// --------------------------------------------------------------------------
// Parsed MDO entry — offset into the import buffer for fast reconstruction
// --------------------------------------------------------------------------

struct ImportedMDOEntry : public CHeapObj<mtInternal> {
  // Identity
  int16_t klass_ref_index;
  Symbol* method_name;     // Interned
  Symbol* method_sig;      // Interned
  uint32_t bytecode_fingerprint;

  // File offset where the header fields begin (after the identity block)
  size_t header_fields_offset;

  // File offset where the entry counts struct begins
  size_t counts_offset;

  // Counts (parsed eagerly for validation)
  uint16_t record_count;
  uint16_t extra_record_count;
  uint16_t param_type_count;
  uint16_t exception_handler_count;

  // File offset where the records section begins
  size_t records_offset;

  // Total byte length of this entry (for skipping)
  size_t total_length;

  ImportedMDOEntry() : klass_ref_index(-1), method_name(nullptr), method_sig(nullptr),
                       bytecode_fingerprint(0), header_fields_offset(0),
                       counts_offset(0), record_count(0), extra_record_count(0),
                       param_type_count(0), exception_handler_count(0),
                       records_offset(0), total_length(0) {}
};

// --------------------------------------------------------------------------
// Import state — file-scope globals (only one import file at a time)
// --------------------------------------------------------------------------

static uint8_t*                _import_buffer = nullptr;
static size_t                  _import_buffer_size = 0;
static PortableMDOFileHeader   _import_header;
static ImportedKlassRef*       _import_klass_refs = nullptr;
static int                     _import_klass_ref_count = 0;
static ImportedMethodRef*      _import_method_refs = nullptr;
static int                     _import_method_ref_count = 0;
static ImportedMDOEntry*       _import_entries = nullptr;
static int                     _import_entry_count = 0;

// Lookup map: (klass_name, method_name, sig) -> entry index
using MDOLookupMap = ResourceHashtable<MDOLookupKey, int,
                                        1024,
                                        AnyObj::C_HEAP, mtInternal,
                                        mdo_lookup_key_hash,
                                        mdo_lookup_key_equals>;
static MDOLookupMap*           _import_lookup_map = nullptr;
static bool                    _import_initialized = false;

// --------------------------------------------------------------------------
// Header validation
// --------------------------------------------------------------------------

static bool validate_import_header(const PortableMDOFileHeader* hdr) {
  if (hdr->magic != PortableMDO::MAGIC) {
    log_error(aot, training)("PortableMDO import: bad magic 0x%08x (expected 0x%08x)",
             hdr->magic, PortableMDO::MAGIC);
    return false;
  }
  if (hdr->format_version != PortableMDO::FORMAT_VERSION) {
    log_error(aot, training)("PortableMDO import: unsupported format version %u (expected %u)",
             hdr->format_version, PortableMDO::FORMAT_VERSION);
    return false;
  }
  // JDK version hash check
  uint32_t current_hash = compute_jdk_version_hash();
  if (hdr->jdk_version_hash != current_hash) {
    log_error(aot, training)("PortableMDO import: JDK version mismatch "
             "(file=0x%08x, current=0x%08x)", hdr->jdk_version_hash, current_hash);
    return false;
  }
  // Pointer size check
  if (hdr->pointer_size != (uint8_t)sizeof(void*)) {
    log_error(aot, training)("PortableMDO import: pointer size mismatch "
             "(file=%u, current=%u)", hdr->pointer_size, (uint8_t)sizeof(void*));
    return false;
  }
  // Endianness check
  uint8_t current_endianness;
#ifdef VM_LITTLE_ENDIAN
  current_endianness = 0;
#else
  current_endianness = 1;
#endif
  if (hdr->endianness != current_endianness) {
    log_error(aot, training)("PortableMDO import: endianness mismatch "
             "(file=%u, current=%u)", hdr->endianness, current_endianness);
    return false;
  }
  // Sanity check counts
  if (hdr->mdo_entry_count > PortableMDO::MAX_MDO_ENTRY_COUNT) {
    log_error(aot, training)("PortableMDO import: too many MDO entries (%u)",
             hdr->mdo_entry_count);
    return false;
  }
  return true;
}

// --------------------------------------------------------------------------
// Parse klass reference table
// --------------------------------------------------------------------------

static bool parse_klass_ref_table(PortableMDOReader* reader,
                                   const PortableMDOFileHeader* hdr) {
  reader->set_position(hdr->klass_ref_table_offset);
  if (reader->error()) return false;

  int count = (int)hdr->klass_ref_count;
  if (count > PortableMDO::MAX_KLASS_REF_COUNT) return false;

  _import_klass_refs = NEW_C_HEAP_ARRAY(ImportedKlassRef, count, mtInternal);
  _import_klass_ref_count = count;

  for (int i = 0; i < count; i++) {
    PortableKlassRefEntry entry;
    if (!reader->read_struct(&entry, sizeof(entry))) return false;

    if (entry.name_length > PortableMDO::MAX_UTF8_LENGTH) return false;
    const uint8_t* name_bytes = reader->read_bytes(entry.name_length);
    if (name_bytes == nullptr) return false;
    reader->align4();

    ImportedKlassRef* ref = &_import_klass_refs[i];
    ref->name = SymbolTable::new_symbol((const char*)name_bytes, entry.name_length);
    ref->loader_tag = entry.loader_tag;
    ref->resolved = nullptr;
    ref->resolution_attempted = false;
  }
  return !reader->error();
}

// --------------------------------------------------------------------------
// Parse method reference table
// --------------------------------------------------------------------------

static bool parse_method_ref_table(PortableMDOReader* reader,
                                    const PortableMDOFileHeader* hdr) {
  reader->set_position(hdr->method_ref_table_offset);
  if (reader->error()) return false;

  int count = (int)hdr->method_ref_count;
  if (count > PortableMDO::MAX_METHOD_REF_COUNT) return false;

  _import_method_refs = NEW_C_HEAP_ARRAY(ImportedMethodRef, count, mtInternal);
  _import_method_ref_count = count;

  for (int i = 0; i < count; i++) {
    PortableMethodRefEntry entry;
    if (!reader->read_struct(&entry, sizeof(entry))) return false;

    if (entry.name_length > PortableMDO::MAX_UTF8_LENGTH) return false;
    if (entry.sig_length > PortableMDO::MAX_UTF8_LENGTH) return false;

    const uint8_t* name_bytes = reader->read_bytes(entry.name_length);
    if (name_bytes == nullptr) return false;
    const uint8_t* sig_bytes = reader->read_bytes(entry.sig_length);
    if (sig_bytes == nullptr) return false;
    reader->align4();

    ImportedMethodRef* ref = &_import_method_refs[i];
    ref->klass_ref_index = (int16_t)entry.klass_ref_index;
    ref->name = SymbolTable::new_symbol((const char*)name_bytes, entry.name_length);
    ref->sig = SymbolTable::new_symbol((const char*)sig_bytes, entry.sig_length);
  }
  return !reader->error();
}

// --------------------------------------------------------------------------
// Skip a single profile record (used during entry parsing to compute offsets)
// --------------------------------------------------------------------------

static bool skip_profile_record(PortableMDOReader* reader) {
  PortableProfileRecordHeader hdr;
  if (!reader->read_struct(&hdr, sizeof(hdr))) return false;

  switch (hdr.tag) {
    case DataLayout::bit_data_tag:
      // No payload
      break;

    case DataLayout::counter_data_tag:
      reader->skip(sizeof(PortableCounterDataPayload));
      break;

    case DataLayout::jump_data_tag:
      reader->skip(sizeof(PortableJumpDataPayload));
      break;

    case DataLayout::receiver_type_data_tag:
    case DataLayout::virtual_call_data_tag: {
      PortableReceiverTypeDataPayload payload;
      if (!reader->read_struct(&payload, sizeof(payload))) return false;
      reader->skip(payload.num_rows * sizeof(PortableReceiverRow));
      break;
    }

    case DataLayout::ret_data_tag: {
      PortableRetDataPayload payload;
      if (!reader->read_struct(&payload, sizeof(payload))) return false;
      reader->skip(payload.num_rows * sizeof(PortableRetRow));
      break;
    }

    case DataLayout::branch_data_tag:
      reader->skip(sizeof(PortableBranchDataPayload));
      break;

    case DataLayout::multi_branch_data_tag: {
      PortableMultiBranchDataPayload payload;
      if (!reader->read_struct(&payload, sizeof(payload))) return false;
      reader->skip(payload.num_cases * sizeof(uint32_t));
      break;
    }

    case DataLayout::call_type_data_tag: {
      PortableCallTypeDataPayload payload;
      if (!reader->read_struct(&payload, sizeof(payload))) return false;
      reader->skip(payload.num_args * sizeof(PortableSymbolicTypeEntry));
      if (payload.has_return) {
        reader->skip(sizeof(PortableSymbolicTypeEntry));
      }
      break;
    }

    case DataLayout::virtual_call_type_data_tag: {
      PortableVirtualCallTypeDataPayload payload;
      if (!reader->read_struct(&payload, sizeof(payload))) return false;
      reader->skip(payload.num_receiver_rows * sizeof(PortableReceiverRow));
      reader->skip(payload.num_args * sizeof(PortableSymbolicTypeEntry));
      if (payload.has_return) {
        reader->skip(sizeof(PortableSymbolicTypeEntry));
      }
      break;
    }

    default:
      // Unknown tag — no payload assumed
      break;
  }
  return !reader->error();
}

// --------------------------------------------------------------------------
// Skip extra records
// --------------------------------------------------------------------------

static bool skip_extra_record(PortableMDOReader* reader) {
  PortableExtraRecordHeader hdr;
  if (!reader->read_struct(&hdr, sizeof(hdr))) return false;

  if (hdr.tag == PortableExtraTag::SPECULATIVE_TRAP) {
    reader->skip(sizeof(PortableSpeculativeTrapPayload));
  }
  // BIT_DATA has no payload beyond the header
  return !reader->error();
}

// --------------------------------------------------------------------------
// Parse MDO entries and build lookup map
// --------------------------------------------------------------------------

static bool parse_mdo_entries(PortableMDOReader* reader,
                               const PortableMDOFileHeader* hdr) {
  reader->set_position(hdr->mdo_entries_offset);
  if (reader->error()) return false;

  int count = (int)hdr->mdo_entry_count;
  _import_entries = NEW_C_HEAP_ARRAY(ImportedMDOEntry, count, mtInternal);
  _import_entry_count = count;

  _import_lookup_map = new (mtInternal) MDOLookupMap();

  for (int i = 0; i < count; i++) {
    size_t entry_start = reader->position();
    ImportedMDOEntry* entry = &_import_entries[i];

    // Read method identity
    PortableMDOMethodIdentity identity;
    if (!reader->read_struct(&identity, sizeof(identity))) return false;

    if (identity.name_length > PortableMDO::MAX_UTF8_LENGTH) return false;
    if (identity.sig_length > PortableMDO::MAX_UTF8_LENGTH) return false;

    const uint8_t* name_bytes = reader->read_bytes(identity.name_length);
    if (name_bytes == nullptr) return false;
    const uint8_t* sig_bytes = reader->read_bytes(identity.sig_length);
    if (sig_bytes == nullptr) return false;
    reader->align4();

    entry->klass_ref_index = (int16_t)identity.klass_ref_index;
    entry->method_name = SymbolTable::new_symbol((const char*)name_bytes, identity.name_length);
    entry->method_sig = SymbolTable::new_symbol((const char*)sig_bytes, identity.sig_length);
    entry->bytecode_fingerprint = identity.bytecode_fingerprint;

    // Record offset of header fields
    entry->header_fields_offset = reader->position();

    // Skip header fields struct
    reader->skip(sizeof(PortableMDOHeaderFields));
    if (reader->error()) return false;

    // Skip arg_modified bytes (count is in the header fields we just skipped)
    // Re-read arg_modified_count from the header fields
    PortableMDOReader hdr_reader(_import_buffer, _import_buffer_size);
    hdr_reader.set_position(entry->header_fields_offset);
    PortableMDOHeaderFields header_fields;
    hdr_reader.read_struct(&header_fields, sizeof(header_fields));
    uint16_t arg_modified_count = header_fields.arg_modified_count;
    reader->skip(arg_modified_count);
    reader->align4();

    // Read counts
    entry->counts_offset = reader->position();
    PortableMDOEntryCounts counts;
    if (!reader->read_struct(&counts, sizeof(counts))) return false;
    entry->record_count = counts.record_count;
    entry->extra_record_count = counts.extra_record_count;
    entry->param_type_count = counts.param_type_count;
    entry->exception_handler_count = counts.exception_handler_count;

    // Record where records section begins
    entry->records_offset = reader->position();

    // Skip all profile records
    for (uint16_t r = 0; r < counts.record_count; r++) {
      if (!skip_profile_record(reader)) return false;
    }

    // Skip extra data records
    for (uint16_t e = 0; e < counts.extra_record_count; e++) {
      if (!skip_extra_record(reader)) return false;
    }

    // Skip parameter type entries
    reader->skip(counts.param_type_count * sizeof(PortableSymbolicTypeEntry));

    // Skip exception handler entries
    reader->skip(counts.exception_handler_count * sizeof(PortableExceptionHandlerEntry));

    if (reader->error()) return false;

    entry->total_length = reader->position() - entry_start;

    // Add to lookup map
    if (entry->klass_ref_index >= 0 &&
        entry->klass_ref_index < _import_klass_ref_count) {
      MDOLookupKey key;
      key.klass_name = _import_klass_refs[entry->klass_ref_index].name;
      key.method_name = entry->method_name;
      key.method_sig = entry->method_sig;
      bool created;
      _import_lookup_map->put_if_absent(key, i, &created);
    }
  }
  return !reader->error();
}

// --------------------------------------------------------------------------
// Klass resolution (lookup-only, no class loading triggered)
// --------------------------------------------------------------------------

static Handle classloader_handle_from_tag(PortableClassLoaderTag tag, Thread* current) {
  switch (tag) {
    case PortableClassLoaderTag::BOOT:
      return Handle(); // null handle = bootstrap
    case PortableClassLoaderTag::PLATFORM:
      return Handle(current, SystemDictionary::java_platform_loader());
    case PortableClassLoaderTag::APP:
      return Handle(current, SystemDictionary::java_system_loader());
    case PortableClassLoaderTag::CUSTOM:
      // Custom classloaders can't be resolved by tag alone —
      // would need additional name-based lookup.
      return Handle();
    default:
      return Handle();
  }
}

static Klass* resolve_imported_klass_ref(int16_t index, Thread* current) {
  if (index < 0 || index >= _import_klass_ref_count) return nullptr;

  ImportedKlassRef* ref = &_import_klass_refs[index];

  // Return cached result if we already attempted resolution
  if (ref->resolution_attempted) return ref->resolved;
  ref->resolution_attempted = true;

  // Custom loaders are not resolved
  if (ref->loader_tag == PortableClassLoaderTag::CUSTOM) {
    log_debug(aot, training)("PortableMDO import: skipping custom-loader klass %s",
             ref->name->as_C_string());
    return nullptr;
  }

  Handle loader = classloader_handle_from_tag(ref->loader_tag, current);
  InstanceKlass* k = SystemDictionary::find_instance_klass(current, ref->name, loader);

  if (k != nullptr) {
    ref->resolved = k;
  } else {
    log_debug(aot, training)("PortableMDO import: unresolved klass %s",
             ref->name->as_C_string());
  }
  return ref->resolved;
}

// --------------------------------------------------------------------------
// Type entry reconstruction helper
//
// Reconstructs the intptr_t encoding for TypeStackSlotEntries and
// ReturnTypeEntry from a PortableSymbolicTypeEntry.
// --------------------------------------------------------------------------

static intptr_t reconstruct_type_entry(const PortableSymbolicTypeEntry* entry,
                                        Thread* current) {
  intptr_t result = TypeEntries::type_none();

  if (entry->null_seen) {
    result |= TypeEntries::null_seen;
  }

  if (entry->type_unknown) {
    result |= TypeEntries::type_unknown;
  } else if (entry->klass_ref_index != PortableMDO::NULL_KLASS_REF) {
    Klass* k = resolve_imported_klass_ref(entry->klass_ref_index, current);
    if (k != nullptr) {
      result = TypeEntries::with_status((intptr_t)k, result);
    }
    // If klass not resolved, leave as type_none with null_seen bit preserved
  }

  return result;
}

// --------------------------------------------------------------------------
// Patch header fields into a freshly-allocated MethodData
// --------------------------------------------------------------------------

static void patch_import_header(MethodData* mdo,
                                 PortableMDOReader* reader,
                                 size_t header_offset,
                                 size_t arg_modified_start) {
  reader->set_position(header_offset);
  PortableMDOHeaderFields hdr;
  reader->read_struct(&hdr, sizeof(hdr));
  if (reader->error()) return;

  // Invocation / backedge counters
  mdo->invocation_counter()->set(hdr.invocation_count);
  mdo->backedge_counter()->set(hdr.backedge_count);

  // Reset baseline counters so invocation_count() / backedge_count()
  // return the imported values directly.
  mdo->reset_start_counters();

  // Trap histogram — write directly into CompilerCounters trap history.
  // The trap_hist encoding: stored value = count + 1. Value 0 in the
  // array means count of (uint)-1 = saturated. Value N means count N-1.
  // We just set the raw byte: _trap_hist._array[i] = stored_value.
  // The portable format stores decoded counts, so re-encode here.
  uint trap_limit = MIN2((uint)_import_header.trap_hist_length,
                          (uint)MethodData::trap_reason_limit());
  for (uint i = 0; i < trap_limit; i++) {
    uint count = hdr.trap_hist[i];
    // Re-encode: the raw array stores count+1 (0 means saturated).
    // We clamp to 254 since 255+1=0 would mean saturated.
    if (count >= 255) {
      // Set as saturated — but this shouldn't happen since export capped at 255
      // To set saturated: call inc_trap_count until it saturates.
      // Simpler: just set the raw byte. We need access to the underlying array.
      // For now, use inc_trap_count in a loop.
      for (uint j = 0; j < 256; j++) {
        mdo->inc_trap_count(i);
      }
    } else {
      for (uint j = 0; j < count; j++) {
        mdo->inc_trap_count(i);
      }
    }
  }

  // Decompile count — increment one by one since there's only inc_decompile_count().
  // IMPORTANT: MethodData::inc_decompile_count() has a side effect: if the
  // count exceeds PerMethodRecompilationCutoff, it marks the method as
  // not-compilable. We must cap the imported count to prevent this, since
  // the purpose of importing profiles is to *enable* compilation, not block it.
  uint decompile_cap = (PerMethodRecompilationCutoff > 0)
      ? (uint)PerMethodRecompilationCutoff : (uint)hdr.nof_decompiles;
  uint decompiles_to_import = MIN2((uint)hdr.nof_decompiles, decompile_cap);
  for (uint d = 0; d < decompiles_to_import; d++) {
    mdo->inc_decompile_count();
  }

  // Tenure traps
  for (uint t = 0; t < hdr.tenure_traps; t++) {
    mdo->inc_tenure_traps();
  }

  // Structural info
  mdo->set_num_loops(hdr.num_loops);
  mdo->set_num_blocks(hdr.num_blocks);

  // Would profile
  if (hdr.would_profile == 2) {
    mdo->set_would_profile(true);
  } else if (hdr.would_profile == 1) {
    mdo->set_would_profile(false);
  }

  // Arg modified
  reader->set_position(arg_modified_start);
  DataLayout* arg_dl = find_arg_info_layout(mdo);
  for (int a = 0; a < hdr.arg_modified_count; a++) {
    uint8_t modified = reader->read_u1();
    if (!reader->error() && arg_dl != nullptr) {
      ArgInfoData args(arg_dl);
      args.set_arg_modified(a, modified);
    }
  }
}

// --------------------------------------------------------------------------
// Patch per-BCI profile records
// --------------------------------------------------------------------------

static void patch_profile_records(MethodData* mdo,
                                   PortableMDOReader* reader,
                                   size_t records_offset,
                                   uint16_t record_count,
                                   Thread* current) {
  reader->set_position(records_offset);
  if (reader->error()) return;

  // Walk the live MDO data and the imported records in parallel,
  // matching by bci + tag.
  ProfileData* live_data = mdo->first_data();
  int import_idx = 0;

  while (mdo->is_valid(live_data) && import_idx < record_count) {
    PortableProfileRecordHeader rec_hdr;
    size_t rec_start = reader->position();
    if (!reader->read_struct(&rec_hdr, sizeof(rec_hdr))) return;

    // Try to match by bci and tag
    if (live_data->bci() == rec_hdr.bci &&
        ((DataLayout*)live_data->dp())->tag() == rec_hdr.tag) {
      // Match — patch this live record

      // Set trap state
      ((DataLayout*)live_data->dp())->set_trap_state(rec_hdr.trap_state);

      switch (rec_hdr.tag) {
        case DataLayout::bit_data_tag: {
          // No payload — flags are already in the DataLayout header.
          // The flags byte has been exported; OR it onto the existing flags.
          // DataLayout::set_flag_at is per-bit; just set the raw flags byte
          // by OR'ing each bit that is set in the imported flags.
          for (int bit = 0; bit < 8; bit++) {
            if (rec_hdr.flags & (1 << bit)) {
              ((DataLayout*)live_data->dp())->set_flag_at(bit);
            }
          }
          break;
        }

        case DataLayout::counter_data_tag: {
          PortableCounterDataPayload payload;
          reader->read_struct(&payload, sizeof(payload));
          CounterData* cd = live_data->as_CounterData();
          cd->set_count(payload.count);
          break;
        }

        case DataLayout::jump_data_tag: {
          PortableJumpDataPayload payload;
          reader->read_struct(&payload, sizeof(payload));
          JumpData* jd = live_data->as_JumpData();
          jd->set_taken(payload.taken);
          // displacement already correct from allocate/initialize
          break;
        }

        case DataLayout::receiver_type_data_tag:
        case DataLayout::virtual_call_data_tag: {
          PortableReceiverTypeDataPayload payload;
          reader->read_struct(&payload, sizeof(payload));
          ReceiverTypeData* rtd = live_data->as_ReceiverTypeData();
          rtd->set_count(payload.count);

          uint rows_to_read = payload.num_rows;
          uint rows_to_patch = MIN2((uint)payload.num_rows, rtd->row_limit());

          for (uint r = 0; r < rows_to_read; r++) {
            PortableReceiverRow row;
            reader->read_struct(&row, sizeof(row));
            if (r < rows_to_patch) {
              Klass* k = resolve_imported_klass_ref(row.klass_ref_index, current);
              rtd->set_receiver(r, k);
              rtd->set_receiver_count(r, k != nullptr ? row.count : 0);
            }
          }
          break;
        }

        case DataLayout::ret_data_tag: {
          PortableRetDataPayload payload;
          reader->read_struct(&payload, sizeof(payload));
          RetData* rd = live_data->as_RetData();
          rd->set_count(payload.count);

          uint rows_to_read = payload.num_rows;
          uint rows_to_patch = MIN2((uint)payload.num_rows, RetData::row_limit());

          // RetData layout per row (3 cells each):
          //   cell[counter_cell_count + row*3 + 0] = bci
          //   cell[counter_cell_count + row*3 + 1] = count
          //   cell[counter_cell_count + row*3 + 2] = displacement (don't touch)
          // counter_cell_count = 1 (from CounterData: count_off=0, counter_cell_count=1)
          DataLayout* ret_dl = (DataLayout*)rd->dp();
          for (uint r = 0; r < rows_to_read; r++) {
            PortableRetRow row;
            reader->read_struct(&row, sizeof(row));
            if (r < rows_to_patch) {
              // bci cell index: header_cells + counter_cell_count + r*3 + 0
              // DataLayout cells start after the header, and ProfileData cell
              // indices are relative to cell[0].
              // CounterData: counter_cell_count = 1
              // RetData: bci0_offset = 1, count0_offset = 2, displacement0_offset = 3
              // ret_row_cell_count = 3
              int bci_cell = 1 + r * 3 + 0;   // bci0_offset + r * ret_row_cell_count
              int cnt_cell = 1 + r * 3 + 1;   // count0_offset + r * ret_row_cell_count
              ret_dl->set_cell_at(bci_cell, (intptr_t)row.target_bci);
              ret_dl->set_cell_at(cnt_cell, (intptr_t)row.count);
              // displacement stays as initialized
            }
          }
          break;
        }

        case DataLayout::branch_data_tag: {
          PortableBranchDataPayload payload;
          reader->read_struct(&payload, sizeof(payload));
          BranchData* bd = live_data->as_BranchData();
          bd->set_taken(payload.taken);
          bd->set_not_taken(payload.not_taken);
          // displacement already correct
          break;
        }

        case DataLayout::multi_branch_data_tag: {
          PortableMultiBranchDataPayload payload;
          reader->read_struct(&payload, sizeof(payload));
          MultiBranchData* mbd = live_data->as_MultiBranchData();

          // MultiBranchData layout in cells (after array_len):
          //   cell[array_start + 0] = default_count
          //   cell[array_start + 1] = default_displacement
          //   cell[array_start + 2 + i*2 + 0] = case_count[i]
          //   cell[array_start + 2 + i*2 + 1] = case_displacement[i]
          // array_start_off_set = 1 (from ArrayData), so cell indices are offset by 1
          // from the DataLayout cell array.
          // Use DataLayout::set_cell_at() which is public.
          DataLayout* dl = (DataLayout*)live_data->dp();
          // default_count is at cell index: array_start(1) + 0
          dl->set_cell_at(1 + 0, (intptr_t)payload.default_count);

          int ncases = MIN2((int)payload.num_cases, mbd->number_of_cases());
          for (int c = 0; c < (int)payload.num_cases; c++) {
            uint32_t cnt = reader->read_u4();
            if (c < ncases) {
              // case_count[c] is at cell index: array_start(1) + 2 + c*2 + 0
              dl->set_cell_at(1 + 2 + c * 2, (intptr_t)cnt);
            }
          }
          // displacements already correct from initialize()
          break;
        }

        case DataLayout::call_type_data_tag: {
          PortableCallTypeDataPayload payload;
          reader->read_struct(&payload, sizeof(payload));
          CallTypeData* ctd = live_data->as_CallTypeData();
          ctd->set_count(payload.count);

          // Argument types
          for (uint8_t a = 0; a < payload.num_args; a++) {
            PortableSymbolicTypeEntry ste;
            reader->read_struct(&ste, sizeof(ste));
            if (ctd->has_arguments() && a < (uint8_t)ctd->number_of_arguments()) {
              intptr_t type_val = reconstruct_type_entry(&ste, current);
              const_cast<TypeStackSlotEntries*>(ctd->args())->set_type(a, type_val);
            }
          }

          // Return type
          if (payload.has_return) {
            PortableSymbolicTypeEntry ste;
            reader->read_struct(&ste, sizeof(ste));
            if (ctd->has_return()) {
              intptr_t type_val = reconstruct_type_entry(&ste, current);
              const_cast<ReturnTypeEntry*>(ctd->ret())->set_type(type_val);
            }
          }
          break;
        }

        case DataLayout::virtual_call_type_data_tag: {
          PortableVirtualCallTypeDataPayload payload;
          reader->read_struct(&payload, sizeof(payload));
          VirtualCallTypeData* vctd = live_data->as_VirtualCallTypeData();
          vctd->set_count(payload.count);

          // Receiver rows
          uint rows_to_patch = MIN2((uint)payload.num_receiver_rows, vctd->row_limit());
          for (uint r = 0; r < payload.num_receiver_rows; r++) {
            PortableReceiverRow row;
            reader->read_struct(&row, sizeof(row));
            if (r < rows_to_patch) {
              Klass* k = resolve_imported_klass_ref(row.klass_ref_index, current);
              vctd->set_receiver(r, k);
              vctd->set_receiver_count(r, k != nullptr ? row.count : 0);
            }
          }

          // Argument types
          for (uint8_t a = 0; a < payload.num_args; a++) {
            PortableSymbolicTypeEntry ste;
            reader->read_struct(&ste, sizeof(ste));
            if (vctd->has_arguments() && a < (uint8_t)vctd->number_of_arguments()) {
              intptr_t type_val = reconstruct_type_entry(&ste, current);
              const_cast<TypeStackSlotEntries*>(vctd->args())->set_type(a, type_val);
            }
          }

          // Return type
          if (payload.has_return) {
            PortableSymbolicTypeEntry ste;
            reader->read_struct(&ste, sizeof(ste));
            if (vctd->has_return()) {
              intptr_t type_val = reconstruct_type_entry(&ste, current);
              const_cast<ReturnTypeEntry*>(vctd->ret())->set_type(type_val);
            }
          }
          break;
        }

        default:
          // Unknown tag at this position — skip its data
          // Re-seek past the record by re-skipping from rec_start
          reader->set_position(rec_start);
          skip_profile_record(reader);
          break;
      }

      import_idx++;
      live_data = mdo->next_data(live_data);

    } else if (live_data->bci() < rec_hdr.bci) {
      // Live data is behind the imported record — advance live_data,
      // but re-read this imported record on the next iteration.
      reader->set_position(rec_start);
      live_data = mdo->next_data(live_data);

    } else {
      // Imported record BCI is behind live data — this record was for
      // a bytecode that no longer has profile data. Skip it.
      reader->set_position(rec_start);
      skip_profile_record(reader);
      import_idx++;
    }
  }

  // Skip any remaining unmatched imported records
  while (import_idx < record_count) {
    skip_profile_record(reader);
    import_idx++;
  }
}

// --------------------------------------------------------------------------
// Patch extra data records
// --------------------------------------------------------------------------

static void patch_extra_data(MethodData* mdo,
                              PortableMDOReader* reader,
                              uint16_t extra_count,
                              Thread* current) {
  if (extra_count == 0) return;

  Mutex* lock = mdo->extra_data_lock();
  MutexLocker ml(lock, Mutex::_no_safepoint_check_flag);

  DataLayout* dp = mdo->extra_data_base();
  DataLayout* end = mdo->args_data_limit();

  for (uint16_t i = 0; i < extra_count; i++) {
    PortableExtraRecordHeader ehdr;
    if (!reader->read_struct(&ehdr, sizeof(ehdr))) return;

    if (dp >= end) {
      // No more room in extra data — skip remaining
      if (ehdr.tag == PortableExtraTag::SPECULATIVE_TRAP) {
        reader->skip(sizeof(PortableSpeculativeTrapPayload));
      }
      continue;
    }

    if (ehdr.tag == PortableExtraTag::SPECULATIVE_TRAP) {
      PortableSpeculativeTrapPayload payload;
      reader->read_struct(&payload, sizeof(payload));

      // Resolve the method reference
      Method* m = nullptr;
      if (payload.method_ref_index >= 0 &&
          payload.method_ref_index < _import_method_ref_count) {
        ImportedMethodRef* mref = &_import_method_refs[payload.method_ref_index];
        Klass* holder = resolve_imported_klass_ref(mref->klass_ref_index, current);
        if (holder != nullptr && holder->is_instance_klass()) {
          InstanceKlass* ik = InstanceKlass::cast(holder);
          m = ik->find_method(mref->name, mref->sig);
          // Don't install old methods
          if (m != nullptr && m->is_old()) m = nullptr;
        }
      }

      if (m != nullptr) {
        // Initialize as a SpeculativeTrapData entry
        int cell_count = SpeculativeTrapData::static_cell_count();
        dp->initialize(DataLayout::speculative_trap_data_tag,
                       ehdr.bci, cell_count);
        dp->set_trap_state(ehdr.trap_state);
        SpeculativeTrapData data(dp);
        data.set_method(m);
        dp = MethodData::next_extra(dp);
      }
      // If method not resolved, just skip this entry

    } else if (ehdr.tag == PortableExtraTag::BIT_DATA) {
      // Stray trap — install as a BitData entry
      int cell_count = BitData::static_cell_count();
      dp->initialize(DataLayout::bit_data_tag, ehdr.bci, cell_count);
      dp->set_trap_state(ehdr.trap_state);
      dp = MethodData::next_extra(dp);
    }
  }
}

// --------------------------------------------------------------------------
// Patch parameter type data
// --------------------------------------------------------------------------

static void patch_parameter_types(MethodData* mdo,
                                   PortableMDOReader* reader,
                                   uint16_t param_count,
                                   Thread* current) {
  ParametersTypeData* params = mdo->parameters_type_data();
  if (params == nullptr || param_count == 0) {
    // Skip the data in the reader
    reader->skip(param_count * sizeof(PortableSymbolicTypeEntry));
    return;
  }

  int live_count = params->number_of_parameters();
  const TypeStackSlotEntries* entries = params->parameters();

  for (uint16_t i = 0; i < param_count; i++) {
    PortableSymbolicTypeEntry ste;
    reader->read_struct(&ste, sizeof(ste));
    if (reader->error()) return;

    if ((int)i < live_count) {
      intptr_t type_val = reconstruct_type_entry(&ste, current);
      // Set the raw intptr_t which includes Klass* | status_bits encoding
      const_cast<TypeStackSlotEntries*>(entries)->set_type(i, type_val);
    }
  }
}

// --------------------------------------------------------------------------
// Patch exception handler data
// --------------------------------------------------------------------------

static void patch_exception_handlers(MethodData* mdo,
                                      PortableMDOReader* reader,
                                      uint16_t handler_count) {
  if (handler_count == 0) return;

  DataLayout* dp = mdo->exception_handler_data_base();
  DataLayout* end = mdo->exception_handler_data_limit();
  int entry_size = DataLayout::compute_size_in_bytes(BitData::static_cell_count());

  for (uint16_t i = 0; i < handler_count; i++) {
    PortableExceptionHandlerEntry entry;
    reader->read_struct(&entry, sizeof(entry));
    if (reader->error()) return;

    if (dp < end) {
      // Match by BCI — the live MDO has exception handler entries
      // in the same order as the bytecodes
      BitData data(dp);
      if (dp->bci() == entry.handler_bci && entry.entered) {
        data.set_exception_handler_entered();
      }
      dp = (DataLayout*)((address)dp + entry_size);
    }
  }
}

// --------------------------------------------------------------------------
// MDO reconstruction — the core import routine
// --------------------------------------------------------------------------

static MethodData* reconstruct_mdo(const methodHandle& method,
                                    ImportedMDOEntry* entry,
                                    TRAPS) {
  // Step 1: Allocate MDO normally — this computes the correct layout,
  // displacements, and initializes all cells to zero.
  ClassLoaderData* loader = method->method_holder()->class_loader_data();
  MethodData* mdo = MethodData::allocate(loader, method, CHECK_NULL);

  PortableMDOReader reader(_import_buffer, _import_buffer_size);

  // Step 2: Patch header fields
  size_t arg_modified_start = entry->header_fields_offset + sizeof(PortableMDOHeaderFields);
  patch_import_header(mdo, &reader, entry->header_fields_offset, arg_modified_start);

  // Step 3: Patch per-BCI profile records
  patch_profile_records(mdo, &reader, entry->records_offset,
                        entry->record_count, THREAD);

  // Step 4: Compute offset of extra records (after all profile records)
  // The reader left off after the profile records, so current position
  // is the start of extra data.
  size_t extra_offset = reader.position();

  // Step 5: Patch extra data
  reader.set_position(extra_offset);
  patch_extra_data(mdo, &reader, entry->extra_record_count, THREAD);

  // Step 6: Patch parameter type data
  patch_parameter_types(mdo, &reader, entry->param_type_count, THREAD);

  // Step 7: Patch exception handler data
  patch_exception_handlers(mdo, &reader, entry->exception_handler_count);

  if (reader.error()) {
    log_warning(aot, training)("PortableMDO import: read error during reconstruction of %s.%s%s",
               method->method_holder()->external_name(),
               method->name()->as_C_string(),
               method->signature()->as_C_string());
    MetadataFactory::free_metadata(loader, mdo);
    return nullptr;
  }

  // Step 8: Fix up runtime state
  // _hint_di is initialized to 0 by MethodData::allocate/initialize,
  // which is correct (first_di() == 0). No fixup needed.

  log_info(aot, training)("PortableMDO import: reconstructed MDO for %s.%s%s "
           "(%d records, %d extra, %d params, %d handlers)",
           method->method_holder()->external_name(),
           method->name()->as_C_string(),
           method->signature()->as_C_string(),
           entry->record_count, entry->extra_record_count,
           entry->param_type_count, entry->exception_handler_count);

  return mdo;
}

// --------------------------------------------------------------------------
// Public API: initialize_import
// --------------------------------------------------------------------------

void PortableMDO::initialize_import(const char* filepath) {
  if (filepath == nullptr) return;
  if (_import_initialized) return;

  // Read the entire file into a C-heap buffer
  FILE* file = os::fopen(filepath, "rb");
  if (file == nullptr) {
    log_error(aot, training)("PortableMDO import: failed to open %s", filepath);
    return;
  }

  // Get file size
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    log_error(aot, training)("PortableMDO import: failed to seek in %s", filepath);
    return;
  }
  long file_size = ftell(file);
  if (file_size <= 0 || (size_t)file_size < sizeof(PortableMDOFileHeader)) {
    fclose(file);
    log_error(aot, training)("PortableMDO import: file too small or empty: %s", filepath);
    return;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return;
  }

  _import_buffer_size = (size_t)file_size;
  _import_buffer = NEW_C_HEAP_ARRAY(uint8_t, _import_buffer_size, mtInternal);
  size_t read = fread(_import_buffer, 1, _import_buffer_size, file);
  fclose(file);

  if (read != _import_buffer_size) {
    log_error(aot, training)("PortableMDO import: short read from %s", filepath);
    FREE_C_HEAP_ARRAY(uint8_t, _import_buffer);
    _import_buffer = nullptr;
    return;
  }

  // Validate header
  memcpy(&_import_header, _import_buffer, sizeof(_import_header));
  if (!validate_import_header(&_import_header)) {
    FREE_C_HEAP_ARRAY(uint8_t, _import_buffer);
    _import_buffer = nullptr;
    return;
  }

  PortableMDOReader reader(_import_buffer, _import_buffer_size);

  // Parse reference tables
  if (!parse_klass_ref_table(&reader, &_import_header)) {
    log_error(aot, training)("PortableMDO import: failed to parse klass ref table");
    shutdown_import();
    return;
  }

  if (!parse_method_ref_table(&reader, &_import_header)) {
    log_error(aot, training)("PortableMDO import: failed to parse method ref table");
    shutdown_import();
    return;
  }

  // Parse MDO entries and build lookup map
  if (!parse_mdo_entries(&reader, &_import_header)) {
    log_error(aot, training)("PortableMDO import: failed to parse MDO entries");
    shutdown_import();
    return;
  }

  _import_initialized = true;

  log_info(aot, training)("PortableMDO import: loaded %d MDO profiles from %s "
           "(%d klass refs, %d method refs)",
           _import_entry_count, filepath,
           _import_klass_ref_count, _import_method_ref_count);
}

// --------------------------------------------------------------------------
// Public API: has_import_data
// --------------------------------------------------------------------------

bool PortableMDO::has_import_data() {
  return _import_initialized;
}

// --------------------------------------------------------------------------
// Public API: try_import
// --------------------------------------------------------------------------

MethodData* PortableMDO::try_import(const methodHandle& method, TRAPS) {
  if (!_import_initialized) return nullptr;

  // Build lookup key from the method
  InstanceKlass* holder = method->method_holder();
  Symbol* klass_name = holder->name();
  Symbol* method_name = method->name();
  Symbol* method_sig = method->signature();

  MDOLookupKey key;
  key.klass_name = klass_name;
  key.method_name = method_name;
  key.method_sig = method_sig;

  int* entry_idx = _import_lookup_map->get(key);
  if (entry_idx == nullptr) {
    log_trace(aot, training)("PortableMDO import: no entry for %s.%s%s",
             klass_name->as_C_string(),
             method_name->as_C_string(),
             method_sig->as_C_string());
    return nullptr;
  }

  ImportedMDOEntry* entry = &_import_entries[*entry_idx];

  // Validate bytecode fingerprint
  uint32_t current_fp = PortableMDO::compute_bytecode_fingerprint(method());
  if (current_fp != entry->bytecode_fingerprint) {
    log_info(aot, training)("PortableMDO import: fingerprint mismatch for %s.%s%s "
             "(expected 0x%08x, got 0x%08x)",
             holder->external_name(),
             method_name->as_C_string(),
             method_sig->as_C_string(),
             entry->bytecode_fingerprint, current_fp);
    return nullptr;
  }

  // Reconstruct the MDO. Compilation is NOT triggered here — that is the
  // job of the explicit Java-side drain (see eager_compile_imported_methods,
  // exposed via jdk.internal.misc.VM.waitForEagerCompilation()).
  return reconstruct_mdo(method, entry, THREAD);
}

// --------------------------------------------------------------------------
// Public API: on_class_linked
// --------------------------------------------------------------------------
//
// Called from InstanceKlass::link_class_impl as soon as a class finishes
// linking. This hook only INSTALLS imported MDOs — it never triggers
// compilation. That keeps the path fully reentrant-safe: even if a future
// drain compiles a method whose inlining causes more class loading, the
// recursive on_class_linked just installs more MDOs (cheap, no locks
// beyond the normal MDO allocation path).
//
// Compilation is performed exclusively by eager_compile_imported_methods,
// which is invoked from Java via jdk.internal.misc.VM.waitForEagerCompilation()
// at a controlled point after the application has finished its initial
// class-loading burst.

void PortableMDO::on_class_linked(InstanceKlass* klass, TRAPS) {
  if (!_import_initialized) return;
  if (!EagerCompilePortableMDO) return;

  Symbol* klass_name = klass->name();

  // Walk all methods in this class and check if we have imported profiles.
  for (int i = 0; i < klass->methods()->length(); i++) {
    Method* m = klass->methods()->at(i);
    if (m->is_abstract() || m->is_native()) continue;

    MDOLookupKey key;
    key.klass_name = klass_name;
    key.method_name = m->name();
    key.method_sig = m->signature();

    int* entry_idx = _import_lookup_map->get(key);
    if (entry_idx == nullptr) continue;

    // Force MDO creation which routes through try_import — pure
    // reconstruction, no compilation.
    if (m->method_data() == nullptr) {
      methodHandle mh(THREAD, m);
      m->build_profiling_method_data(mh, THREAD);
      if (HAS_PENDING_EXCEPTION) { CLEAR_PENDING_EXCEPTION; }
    }
  }
}

// --------------------------------------------------------------------------
// Public API: eager_compile_imported_methods
// --------------------------------------------------------------------------

void PortableMDO::eager_compile_imported_methods(TRAPS) {
  if (!_import_initialized) return;
  if (!UseCompiler || !CompilationPolicy::is_compilation_enabled()) return;

  ResourceMark rm(THREAD);
  int compiled = 0;
  int skipped = 0;

  log_info(aot, training)("PortableMDO: starting eager compilation of %d imported methods",
           _import_entry_count);

  for (int i = 0; i < _import_entry_count; i++) {
    ImportedMDOEntry* entry = &_import_entries[i];

    // Resolve the holder class
    if (entry->klass_ref_index < 0 || entry->klass_ref_index >= _import_klass_ref_count) {
      skipped++;
      continue;
    }
    ImportedKlassRef* kref = &_import_klass_refs[entry->klass_ref_index];

    // Skip custom classloader classes — we can't resolve them
    if (kref->loader_tag == PortableClassLoaderTag::CUSTOM) {
      skipped++;
      continue;
    }

    // Use resolve_or_null to actually load the class if needed
    Handle loader = classloader_handle_from_tag(kref->loader_tag, THREAD);
    TempNewSymbol klass_sym = SymbolTable::new_symbol(kref->name->as_C_string(),
                                                       (int)kref->name->utf8_length());
    Klass* k = SystemDictionary::resolve_or_null(klass_sym, loader, THREAD);
    if (HAS_PENDING_EXCEPTION) { CLEAR_PENDING_EXCEPTION; }
    if (k == nullptr || !k->is_instance_klass()) {
      skipped++;
      continue;
    }

    InstanceKlass* holder = InstanceKlass::cast(k);

    // Link the class if not already linked
    holder->link_class(THREAD);
    if (HAS_PENDING_EXCEPTION) { CLEAR_PENDING_EXCEPTION; }

    // Find the method
    Method* target = holder->find_method(entry->method_name, entry->method_sig);
    if (target == nullptr || target->is_abstract() || target->is_native()) {
      skipped++;
      continue;
    }

    // Validate bytecode fingerprint
    uint32_t current_fp = PortableMDO::compute_bytecode_fingerprint(target);
    if (current_fp != entry->bytecode_fingerprint) {
      skipped++;
      continue;
    }

    // Ensure the MDO is installed (via try_import path)
    methodHandle mh(THREAD, target);
    if (target->method_data() == nullptr) {
      target->build_profiling_method_data(mh, THREAD);
      if (HAS_PENDING_EXCEPTION) { CLEAR_PENDING_EXCEPTION; }
    }

    if (target->method_data() == nullptr) {
      skipped++;
      continue;
    }

    // Read the stored compilation level from the header fields
    PortableMDOReader hdr_reader(_import_buffer, _import_buffer_size);
    hdr_reader.set_position(entry->header_fields_offset);
    PortableMDOHeaderFields header_fields;
    if (!hdr_reader.read_struct(&header_fields, sizeof(header_fields))) {
      skipped++;
      continue;
    }

    CompLevel level = (CompLevel)header_fields.highest_comp_level;
    if (level < CompLevel_none) {
      level = CompLevel_none;
    } else if (level > CompLevel_full_optimization) {
      level = CompLevel_full_optimization;
    }
    // Don't eagerly compile at level 0 (interpreted) — no benefit
    if (level <= CompLevel_none) {
      skipped++;
      continue;
    }

    // Queue the method for compilation
    CompileBroker::compile_method(mh, InvocationEntryBci, level,
                                  0, CompileTask::Reason_MustBeCompiled, THREAD);
    if (HAS_PENDING_EXCEPTION) { CLEAR_PENDING_EXCEPTION; }
    compiled++;
  }

  // Wait for all compilations to complete
  for (;;) {
    CompileBroker::wait_for_no_active_tasks();
    CompileQueue* q1 = CompileBroker::c1_compile_queue();
    CompileQueue* q2 = CompileBroker::c2_compile_queue();
    bool empty1 = (q1 == nullptr) || q1->is_empty();
    bool empty2 = (q2 == nullptr) || q2->is_empty();
    if (empty1 && empty2) break;
    os::naked_short_sleep(1);
  }

  log_info(aot, training)("PortableMDO: eager compilation done: %d compiled, %d skipped",
           compiled, skipped);
}

// --------------------------------------------------------------------------
// Public API: shutdown_import
// --------------------------------------------------------------------------

void PortableMDO::shutdown_import() {
  if (_import_buffer != nullptr) {
    FREE_C_HEAP_ARRAY(uint8_t, _import_buffer);
    _import_buffer = nullptr;
  }
  if (_import_klass_refs != nullptr) {
    // Release symbol refcounts
    for (int i = 0; i < _import_klass_ref_count; i++) {
      if (_import_klass_refs[i].name != nullptr) {
        _import_klass_refs[i].name->decrement_refcount();
      }
    }
    FREE_C_HEAP_ARRAY(ImportedKlassRef, _import_klass_refs);
    _import_klass_refs = nullptr;
  }
  if (_import_method_refs != nullptr) {
    for (int i = 0; i < _import_method_ref_count; i++) {
      if (_import_method_refs[i].name != nullptr) {
        _import_method_refs[i].name->decrement_refcount();
      }
      if (_import_method_refs[i].sig != nullptr) {
        _import_method_refs[i].sig->decrement_refcount();
      }
    }
    FREE_C_HEAP_ARRAY(ImportedMethodRef, _import_method_refs);
    _import_method_refs = nullptr;
  }
  if (_import_entries != nullptr) {
    for (int i = 0; i < _import_entry_count; i++) {
      if (_import_entries[i].method_name != nullptr) {
        _import_entries[i].method_name->decrement_refcount();
      }
      if (_import_entries[i].method_sig != nullptr) {
        _import_entries[i].method_sig->decrement_refcount();
      }
    }
    FREE_C_HEAP_ARRAY(ImportedMDOEntry, _import_entries);
    _import_entries = nullptr;
  }
  if (_import_lookup_map != nullptr) {
    delete _import_lookup_map;
    _import_lookup_map = nullptr;
  }
  _import_klass_ref_count = 0;
  _import_method_ref_count = 0;
  _import_entry_count = 0;
  _import_initialized = false;
  _import_buffer_size = 0;
}
