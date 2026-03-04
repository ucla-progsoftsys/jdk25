#include "services/profileCheckpoint.hpp"
#include "utilities/ostream.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/classLoaderData.hpp"
#include "services/profileCheckpoint_globals.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/methodData.hpp"
#include "oops/method.hpp"
#include "oops/methodCounters.hpp"
#include "oops/klass.inline.hpp"
#include "oops/objArrayOop.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "logging/log.hpp"
#include "utilities/copy.hpp"
#include "runtime/os.hpp"
#include "runtime/globals.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/mutexLocker.hpp"
#include "compiler/compilationPolicy.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/compilerDefinitions.hpp"
#include "memory/resourceArea.hpp"
#include "utilities/stringUtils.hpp"
#include "interpreter/linkResolver.hpp"
#include "oops/constantPool.inline.hpp"
#include "oops/cpCache.inline.hpp"
#include "oops/resolvedIndyEntry.hpp"
#include "prims/methodHandles.hpp"
#include "ci/ciReplay.hpp"
#include "classfile/classLoader.hpp"
#include "services/dynoLocatorScan.hpp"
#include <cstdio>
#include <cstring>


// ============================================================================
// Binary format + I/O helpers (MDOX) + debug printing
// ============================================================================
static u2 detect_endianness() {
  union { u4 v; u1 b[4]; } u; u.v = 1;
  return (u.b[0] == 1) ? (u2)0 : (u2)1;
}

static bool write_exact(fileStream* out, const void* p, size_t n) {
  if (n == 0) return true;
  out->write((const char*)p, n);
  return true;
}

static bool read_exact(FILE* in, void* p, size_t n) {
  return ::fread(p, 1, n, in) == n;
}

static bool write_u4(fileStream* out, u4 v) {
  return write_exact(out, &v, sizeof(v));
}

static bool write_u1(fileStream* out, u1 v) {
  return write_exact(out, &v, sizeof(v));
}

static bool read_u4(FILE* in, u4& v) {
  return read_exact(in, &v, sizeof(v));
}

static bool read_u1(FILE* in, u1& v) {
  return read_exact(in, &v, sizeof(v));
}

static char* read_str(FILE* in) {
  u4 len = 0;
  if (!read_u4(in, len)) return nullptr;
  char* buf = (char*)os::malloc(len + 1, mtInternal);
  if (buf == nullptr) return nullptr;
  if (len > 0 && !read_exact(in, buf, len)) { os::free(buf); return nullptr; }
  buf[len] = '\0';
  return buf;
}

static bool write_u2(fileStream* out, u2 v) { return write_exact(out, &v, sizeof(v)); }
static bool read_u2(FILE* in, u2& v) { return read_exact(in, &v, sizeof(v)); }

static bool write_header(fileStream* out, const ProfileCheckpoint::Header& h) {
  if (!write_exact(out, h.magic, sizeof(h.magic))) return false;
  if (!write_u2(out, h.pointer_size)) return false;
  if (!write_u2(out, h.endianness)) return false;
  if (!write_exact(out, &h.layout, sizeof(h.layout))) return false;
  if (!write_u4(out, h.sym_count)) return false;
  if (!write_u4(out, h.rec_count)) return false;
  if (!write_u4(out, h.class_count)) return false;
  return true;
}

static bool read_header(FILE* in, ProfileCheckpoint::Header& h) {
  if (!read_exact(in, h.magic, sizeof(h.magic))) return false;
  if (h.magic[0] != 'M' || h.magic[1] != 'D' || h.magic[2] != 'O' || h.magic[3] != 'X') return false;
  if (!read_u2(in, h.pointer_size)) return false;
  if (!read_u2(in, h.endianness)) return false;
  if (!read_exact(in, &h.layout, sizeof(h.layout))) return false;
  if (!read_u4(in, h.sym_count)) return false;
  if (!read_u4(in, h.rec_count)) return false;
  if (!read_u4(in, h.class_count)) return false;
  return true;
}

static void init_header(ProfileCheckpoint::Header& h, u4 sym_count, u4 rec_count, u4 class_count) {
  h.magic[0] = 'M'; h.magic[1] = 'D'; h.magic[2] = 'O'; h.magic[3] = 'X';
  h.pointer_size = (u2)sizeof(void*);
  h.endianness = detect_endianness();
  h.layout.type_profile_level = (uint32_t)TypeProfileLevel;
  h.layout.type_profile_args_limit = (int32_t)TypeProfileArgsLimit;
  h.layout.type_profile_parms_limit = (int32_t)TypeProfileParmsLimit;
  h.layout.type_profile_width = (int64_t)TypeProfileWidth;
  h.layout.profile_traps = (uint8_t)(ProfileTraps ? 1 : 0);
  h.layout.type_profile_casts = (uint8_t)(TypeProfileCasts ? 1 : 0);
  h.layout.spec_trap_limit_extra_entries = (int32_t)SpecTrapLimitExtraEntries;
  h.sym_count = sym_count;
  h.rec_count = rec_count;
  h.class_count = class_count;
}

static bool write_symtab(fileStream* out, const GrowableArray<const char*>& symbols) {
  for (int i = 0; i < symbols.length(); i++) {
    const char* s = symbols.at(i);
    u4 len = (u4)strlen(s);
    if (!write_u4(out, len)) return false;
    if (!write_exact(out, s, len)) return false;
  }
  return true;
}

static bool read_symtab(FILE* in, GrowableArray<char*>& symbols, u4 expected) {
  for (u4 si = 0; si < expected; si++) {
    char* s = read_str(in);
    if (s == nullptr) {
      return false;
    }
    symbols.append(s);
  }
  return true;
}

static bool write_classes(fileStream* out, const GrowableArray<ProfileCheckpoint::Class>& classes) {
  // MDOX format: [u1 loader_id][u4 loader_name_sym_id][u4 klass_sym_id]
  const u4 NO_LOADER_NAME = 0xFFFFFFFF;
  for (int i = 0; i < classes.length(); i++) {
    const ProfileCheckpoint::Class& c = classes.at(i);
    if (!write_u1(out, (u1)c.loader)) return false;
    u4 loader_name_id = (c.loader == ProfileCheckpoint::LoaderId::NAMED) ? c.loader_name.id : NO_LOADER_NAME;
    if (!write_u4(out, loader_name_id)) return false;
    if (!write_u4(out, c.klass.id)) return false;
  }
  return true;
}

static bool read_classes(FILE* in, GrowableArray<ProfileCheckpoint::Class>& classes, u4 expected) {
  const u4 NO_LOADER_NAME = 0xFFFFFFFF;
  for (u4 ci = 0; ci < expected; ci++) {
    u1 loader_raw = 0;
    if (!read_u1(in, loader_raw)) {
      return false;
    }
    u4 loader_name_id = NO_LOADER_NAME;
    if (!read_u4(in, loader_name_id)) {
      return false;
    }
    u4 klass_id = 0;
    if (!read_u4(in, klass_id)) {
      return false;
    }
    ProfileCheckpoint::Class c;
    c.loader = (ProfileCheckpoint::LoaderId)loader_raw;
    c.loader_name.id = loader_name_id;
    c.klass.id = klass_id;
    classes.append(c);
  }
  return true;
}

static bool write_record(fileStream* out, const ProfileCheckpoint::Record& r, const void* mdo_bytes, const ProfileCheckpoint::Fixup* fixups, const void* mc_bytes, const void* header_bytes) {
  const u4 NO_LOADER_NAME = 0xFFFFFFFF;
  if (!write_u4(out, r.key.klass.id)) return false;
  if (!write_u4(out, r.key.name.id))  return false;
  if (!write_u4(out, r.key.sig.id))   return false;
  u1 loader = (u1)r.key.loader;
  if (!write_exact(out, &loader, sizeof(loader))) return false;
  // MDOX: write loader_name_id after loader
  u4 loader_name_id = (r.key.loader == ProfileCheckpoint::LoaderId::NAMED) ? r.key.loader_name.id : NO_LOADER_NAME;
  if (!write_u4(out, loader_name_id)) return false;
  if (!write_exact(out, &r.comp_level, sizeof(r.comp_level))) return false;
  if (!write_u4(out, r.mdo_size)) return false;
  if (!write_u4(out, r.fixup_count)) return false;
  for (u4 i = 0; i < r.fixup_count; i++) {
    if (!write_u4(out, fixups[i].offset_in_mdo)) return false;
    if (!write_u4(out, fixups[i].target.id)) return false;
    u1 fx_loader = (u1)fixups[i].loader;
    if (!write_u1(out, fx_loader)) return false;
    // MDOX: write fixup loader_name_id
    u4 fx_loader_name_id = (fixups[i].loader == ProfileCheckpoint::LoaderId::NAMED) ? fixups[i].loader_name.id : NO_LOADER_NAME;
    if (!write_u4(out, fx_loader_name_id)) return false;
  }
  if (!write_exact(out, mdo_bytes, r.mdo_size)) return false;
  if (!write_u4(out, r.header_size)) return false;
  if (r.header_size > 0) {
    if (!write_exact(out, header_bytes, r.header_size)) return false;
  }
  if (!write_u4(out, r.mc_size)) return false;
  if (r.mc_size > 0) {
    if (!write_exact(out, mc_bytes, r.mc_size)) return false;
  }
  return true;
}

static bool read_record(FILE* in, ProfileCheckpoint::Record& r, ProfileCheckpoint::Fixup*& fixups, char*& mdo_bytes, char*& mc_bytes, char*& header_bytes) {
  const u4 NO_LOADER_NAME = 0xFFFFFFFF;
  if (!read_u4(in, r.key.klass.id)) return false;
  if (!read_u4(in, r.key.name.id))  return false;
  if (!read_u4(in, r.key.sig.id))   return false;
  u1 loader = 0;
  if (!read_exact(in, &loader, sizeof(loader))) return false;
  r.key.loader = (ProfileCheckpoint::LoaderId)loader;
  r.key.loader_name.id = NO_LOADER_NAME;
  if (!read_u4(in, r.key.loader_name.id)) return false;
  if (!read_exact(in, &r.comp_level, sizeof(r.comp_level))) return false;
  if (!read_u4(in, r.mdo_size)) return false;
  if (!read_u4(in, r.fixup_count)) return false;
  fixups = nullptr;
  if (r.fixup_count > 0) {
    fixups = (ProfileCheckpoint::Fixup*)os::malloc(sizeof(ProfileCheckpoint::Fixup) * r.fixup_count, mtInternal);
    if (fixups == nullptr) return false;
    for (u4 i = 0; i < r.fixup_count; i++) {
      if (!read_u4(in, fixups[i].offset_in_mdo)) { os::free(fixups); return false; }
      if (!read_u4(in, fixups[i].target.id)) { os::free(fixups); return false; }
      u1 fx_loader = 0;
      if (!read_u1(in, fx_loader)) { os::free(fixups); return false; }
      fixups[i].loader = (ProfileCheckpoint::LoaderId)fx_loader;
      fixups[i].loader_name.id = NO_LOADER_NAME;
      if (!read_u4(in, fixups[i].loader_name.id)) { os::free(fixups); return false; }
    }
  }
  mdo_bytes = nullptr;
  if (r.mdo_size > 0) {
    mdo_bytes = (char*)os::malloc(r.mdo_size, mtInternal);
    if (mdo_bytes == nullptr) { if (fixups) os::free(fixups); return false; }
    if (!read_exact(in, mdo_bytes, r.mdo_size)) { os::free(mdo_bytes); if (fixups) os::free(fixups); return false; }
  }
  header_bytes = nullptr;
  if (!read_u4(in, r.header_size)) {
    if (fixups) os::free(fixups);
    if (mdo_bytes) os::free(mdo_bytes);
    return false;
  }
  if (r.header_size > 0) {
    header_bytes = (char*)os::malloc(r.header_size, mtInternal);
    if (header_bytes == nullptr) {
      if (fixups) os::free(fixups);
      if (mdo_bytes) os::free(mdo_bytes);
      return false;
    }
    if (!read_exact(in, header_bytes, r.header_size)) {
      os::free(header_bytes);
      if (fixups) os::free(fixups);
      if (mdo_bytes) os::free(mdo_bytes);
      return false;
    }
  }
  if (!read_u4(in, r.mc_size)) return false;
  mc_bytes = nullptr;
  if (r.mc_size > 0) {
    mc_bytes = (char*)os::malloc(r.mc_size, mtInternal);
    if (mc_bytes == nullptr) {
      if (fixups) os::free(fixups);
      if (mdo_bytes) os::free(mdo_bytes);
      if (header_bytes) os::free(header_bytes);
      return false;
    }
    if (!read_exact(in, mc_bytes, r.mc_size)) {
      os::free(mc_bytes);
      if (fixups) os::free(fixups);
      if (mdo_bytes) os::free(mdo_bytes);
      if (header_bytes) os::free(header_bytes);
      return false;
    }
  }
  return true;
}

static void print_mdo_header(MethodData* mdo, outputStream* st = tty) {
  if (mdo == nullptr) {
    st->print_cr("  (MethodData is null)");
    return;
  }
  ResourceMark rm;
  Method* m = mdo->method();
  const char* method_name = (m != nullptr) ? m->name_and_sig_as_C_string() : "<null>";
  const char* holder_name = (m != nullptr && m->method_holder() != nullptr) ? m->method_holder()->external_name() : "<null>";
  st->print_cr("  method=%s holder=%s", method_name, holder_name);
  st->print_cr("  size_in_bytes=%d data_size=%d extra_data_size=%d",
               mdo->size_in_bytes(),
               mdo->data_size(),
               mdo->extra_data_size());
  st->print_cr("  parameters_type_data_di=%d exception_handler_data_di=%d",
               mdo->parameters_type_data_di(),
               mdo->exception_handlers_data_di());
  st->print_cr("  invocation_counter=%d start=%d delta=%d",
               mdo->invocation_count(),
               mdo->invocation_count_start(),
               mdo->invocation_count_delta());
  st->print_cr("  backedge_counter=%d start=%d delta=%d",
               mdo->backedge_count(),
               mdo->backedge_count_start(),
               mdo->backedge_count_delta());
  st->print_cr("  tenure_traps=%u decompile_count=%u overflow_recompiles=%u overflow_traps=%u",
               mdo->tenure_traps(),
               mdo->decompile_count(),
               mdo->overflow_recompile_count(),
               mdo->overflow_trap_count());
  st->print_cr("  loops=%d blocks=%d mature=%s would_profile=%s",
               mdo->num_loops(),
               mdo->num_blocks(),
               mdo->is_mature() ? "true" : "false",
               mdo->would_profile() ? "true" : "false");
  st->print_cr("  escape_flags=0x%lx arg_local=0x%lx arg_stack=0x%lx arg_returned=0x%lx",
               (long)mdo->eflags(),
               (long)mdo->arg_local(),
               (long)mdo->arg_stack(),
               (long)mdo->arg_returned());

  st->print("  trap_counts:");
  bool printed = false;
  const uint reason_limit = MethodData::trap_reason_limit();
  for (uint reason = 0; reason < reason_limit; reason++) {
    uint count = mdo->trap_count((int)reason);
    if (count == 0) {
      continue;
    }
    st->print(" %s=%u", Deoptimization::trap_reason_name(reason), count);
    printed = true;
  }
  if (!printed) {
    st->print(" (none)");
  }
  st->cr();
}

// ============================================================================
// Symbolic resolution helpers (LoaderId / klass / method)
// ============================================================================

// Get the stable loader name for a non-built-in, non-hidden class loader.
// Returns nullptr if the loader doesn't have an explicit name set.
// Caller needs ResourceMark.
static const char* get_loader_name_for_klass(InstanceKlass* ik) {
  if (ik == nullptr || ik->is_hidden()) return nullptr;
  ClassLoaderData* cld = ik->class_loader_data();
  if (cld == nullptr || cld->is_builtin_class_loader_data()) return nullptr;
  // Use loader_name() which returns the explicit name or class name
  return cld->loader_name();
}

static ProfileCheckpoint::LoaderId loader_id_from_klass(InstanceKlass* ik) {
  if (ik != nullptr && ik->is_hidden()) {
    return ProfileCheckpoint::LoaderId::HIDDEN;
  }
  ClassLoaderData* cld = (ik != nullptr) ? ik->class_loader_data() : nullptr;
  if (cld == nullptr || cld->is_boot_class_loader_data()) return ProfileCheckpoint::LoaderId::BOOT;
  if (cld->is_platform_class_loader_data()) return ProfileCheckpoint::LoaderId::PLATFORM;
  if (cld->is_system_class_loader_data()) return ProfileCheckpoint::LoaderId::SYSTEM;
  // Non-built-in loader: use NAMED if we can get a name
  log_debug(compilation)("Non-builtin loader encountered: %s", cld->loader_name_and_id());
  return ProfileCheckpoint::LoaderId::NAMED;
}

// Look up a user-defined class loader by its name.
// Returns a Handle to the loader oop if found, or null Handle if not found or multiple matches.
static Handle loader_handle_by_name(const char* loader_name, TRAPS) {
  if (loader_name == nullptr) return Handle();

  oop candidate = nullptr;
  int candidate_count = 0;

  {
    MutexLocker ml(ClassLoaderDataGraph_lock, Mutex::_no_safepoint_check_flag);

    class FindLoaderByName : public CLDClosure {
      const char* _name;
      oop* _candidate;
      int* _count;
     public:
      FindLoaderByName(const char* name, oop* candidate, int* count)
        : _name(name), _candidate(candidate), _count(count) {}
      void do_cld(ClassLoaderData* cld) override {
        if (cld == nullptr) return;
        if (cld->is_builtin_class_loader_data()) return;
        // Skip CLDs dedicated to non-strong hidden classes.
        if (cld->has_class_mirror_holder()) return;
        oop loader = cld->class_loader_no_keepalive();
        if (loader == nullptr) return;
        // Compare loader name
        const char* cld_name = cld->loader_name();
        if (cld_name != nullptr && strcmp(cld_name, _name) == 0) {
          (*_count)++;
          if (*_candidate == nullptr) {
            *_candidate = loader;
          }
        }
      }
    } finder(loader_name, &candidate, &candidate_count);

    ClassLoaderDataGraph::loaded_cld_do(&finder);
  }

  log_debug(compilation)("loader_handle_by_name('%s'): found %d matches", loader_name, candidate_count);
  if (candidate_count == 1 && candidate != nullptr) {
    return Handle(THREAD, candidate);
  }
  if (candidate_count > 1) {
    log_debug(compilation)("loader_handle_by_name('%s'): multiple loaders with same name, failing closed", loader_name);
  }
  return Handle();
}

// Get a Handle to the class loader for resolution.
// For NAMED loaders, uses the loader_name to look up the loader.
static Handle loader_handle_from_loader(ProfileCheckpoint::LoaderId loader_id, const char* loader_name, TRAPS) {
  switch (loader_id) {
    case ProfileCheckpoint::LoaderId::BOOT: return Handle();
    case ProfileCheckpoint::LoaderId::PLATFORM: return Handle(THREAD, SystemDictionary::java_platform_loader());
    case ProfileCheckpoint::LoaderId::SYSTEM: return Handle(THREAD, SystemDictionary::java_system_loader());
    case ProfileCheckpoint::LoaderId::NAMED: return loader_handle_by_name(loader_name, THREAD);
    default: return Handle();
  }
}

static bool is_builtin_loader(ProfileCheckpoint::LoaderId loader_id) {
  return loader_id == ProfileCheckpoint::LoaderId::BOOT ||
         loader_id == ProfileCheckpoint::LoaderId::PLATFORM ||
         loader_id == ProfileCheckpoint::LoaderId::SYSTEM;
}

static void normalize_class_pattern(const char* raw, char* normalized) {
  int i = 0;
  for (; raw[i] != '\0'; i++) {
    // Support both java/lang/Foo and java.lang.Foo style patterns.
    normalized[i] = (raw[i] == '.') ? '/' : raw[i];
  }
  normalized[i] = '\0';
}

static void build_eager_init_allowlist(GrowableArray<const char*>& patterns) {
  if (EagerInitAfterLoadAllowlist == nullptr || EagerInitAfterLoadAllowlist[0] == '\0') {
    return;
  }

  StringUtils::CommaSeparatedStringIterator iter(EagerInitAfterLoadAllowlist);
  for (; *iter != nullptr; ++iter) {
    const char* token = *iter;
    if (token[0] == '\0') {
      continue;
    }
    const size_t len = strlen(token);
    char* normalized = NEW_RESOURCE_ARRAY(char, len + 1);
    normalize_class_pattern(token, normalized);
    patterns.append(normalized);
  }
}

static bool class_matches_eager_init_allowlist(const char* klass_name,
                                               const GrowableArray<const char*>& patterns) {
  if (klass_name == nullptr || klass_name[0] == '\0' || klass_name[0] == '@') {
    return false;
  }
  for (int i = 0; i < patterns.length(); i++) {
    if (StringUtils::is_star_match(patterns.at(i), klass_name)) {
      return true;
    }
  }
  return false;
}

static void log_and_clear_pending_exception(const char* action,
                                            const char* klass_name,
                                            ProfileCheckpoint::LoaderId loader_id,
                                            TRAPS) {
  if (!HAS_PENDING_EXCEPTION) {
    return;
  }

  ResourceMark rm(THREAD);
  const char* kname = (klass_name != nullptr) ? klass_name : "<null>";
  oop exception = PENDING_EXCEPTION;
  stringStream exception_text;
  java_lang_Throwable::print(exception, &exception_text);
  log_info(compilation)("MDO checkpoint: %s FAILED for %s (loader=%d): %s",
                        action, kname, (int)loader_id, exception_text.as_string());
  CLEAR_PENDING_EXCEPTION;
}

static bool try_link_class(InstanceKlass* klass,
                           const char* klass_name,
                           ProfileCheckpoint::LoaderId loader_id,
                           const char* action,
                           TRAPS) {
  if (klass == nullptr) {
    return false;
  }
  klass->link_class(THREAD);
  if (HAS_PENDING_EXCEPTION) {
    log_and_clear_pending_exception(action, klass_name, loader_id, THREAD);
    return false;
  }
  return true;
}

static bool try_initialize_class(InstanceKlass* klass,
                                 const char* klass_name,
                                 ProfileCheckpoint::LoaderId loader_id,
                                 const char* action,
                                 TRAPS) {
  if (klass == nullptr) {
    return false;
  }
  klass->initialize(THREAD);
  if (HAS_PENDING_EXCEPTION) {
    log_and_clear_pending_exception(action, klass_name, loader_id, THREAD);
    return false;
  }
  return true;
}

static const char* get_klassname_utf8(InstanceKlass* ik) {
  if (ik == nullptr) {
    return "<null>";
  }
  if (ik->is_hidden()) {
    if (const char* loc = DynoLocatorScan::lookup(ik)) {
      log_debug(compilation)("found locator for hidden ik %s: %s", ik->name()->as_utf8(), loc);
      return loc;
    }
    log_debug(compilation)("hidden ik encountered without locator: %s", ik->name()->as_utf8());
  }
  return ik->name()->as_utf8();
}

static InstanceKlass* resolve_klass_utf8(const char* name, ProfileCheckpoint::LoaderId loader_id, const char* loader_name, TRAPS) {
  if (name != nullptr && name[0] == '@') {
    log_debug(compilation)("resolving %s", name);
    InstanceKlass* hk = DynoLocatorScan::resolve_locator(name, THREAD);
    if (hk != nullptr) return hk;
  }
  Symbol* sym = SymbolTable::new_symbol(name);
  Handle loader = loader_handle_from_loader(loader_id, loader_name, THREAD);
  Klass* k = SystemDictionary::resolve_or_fail(sym, loader, true, THREAD);
  if (HAS_PENDING_EXCEPTION) { CLEAR_PENDING_EXCEPTION; return nullptr; }
  return (k != nullptr && k->is_instance_klass()) ? InstanceKlass::cast(k) : nullptr;
}

static Method* resolve_method_utf8(InstanceKlass* ik, const char* mname, const char* msig) {
  if (ik == nullptr) return nullptr;
  Symbol* mn = SymbolTable::new_symbol(mname);
  Symbol* sg = SymbolTable::new_symbol(msig);
  return ik->find_method(mn, sg);
}

// ============================================================================
// Type-profile fixups (collect/sanitize/apply)
// ============================================================================
static void sanitize_type_entries(MethodData* mdo) {
  for (ProfileData* pd = mdo->first_data(); mdo->is_valid(pd); pd = mdo->next_data(pd)) {
    if (pd->is_VirtualCallData() || pd->is_ReceiverTypeData()) {
      ReceiverTypeData* rtd = pd->as_ReceiverTypeData();
      for (uint row = 0; row < rtd->row_limit(); row++) {
        int off_b = in_bytes(ReceiverTypeData::receiver_offset(row));
        intptr_t* cell = (intptr_t*)(pd->dp() + off_b);
        *cell = TypeEntries::with_status((Klass*)nullptr, *cell);
      }
    }
    if (pd->is_CallTypeData()) {
      CallTypeData* ctd = (CallTypeData*)pd;
      if (ctd->has_arguments()) {
        for (int ai = 0; ai < ctd->number_of_arguments(); ai++) {
          int off_b = in_bytes(ctd->argument_type_offset(ai));
          intptr_t* cell = (intptr_t*)(pd->dp() + off_b);
          *cell = TypeEntries::with_status((Klass*)nullptr, *cell);
        }
      }
      if (ctd->has_return()) {
        int off_b = in_bytes(ctd->return_type_offset());
        intptr_t* cell = (intptr_t*)(pd->dp() + off_b);
        *cell = TypeEntries::with_status((Klass*)nullptr, *cell);
      }
    }
    if (pd->is_VirtualCallTypeData()) {
      VirtualCallTypeData* vctd = pd->as_VirtualCallTypeData();
      if (vctd->has_arguments()) {
        for (int ai = 0; ai < vctd->number_of_arguments(); ai++) {
          int off_b = in_bytes(vctd->argument_type_offset(ai));
          intptr_t* cell = (intptr_t*)(pd->dp() + off_b);
          *cell = TypeEntries::with_status((Klass*)nullptr, *cell);
        }
      }
      if (vctd->has_return()) {
        int off_b = in_bytes(vctd->return_type_offset());
        intptr_t* cell = (intptr_t*)(pd->dp() + off_b);
        *cell = TypeEntries::with_status((Klass*)nullptr, *cell);
      }
    }
  }
  if (ParametersTypeData* p = mdo->parameters_type_data()) {
    address p_dp = p->dp();
    for (int pi = 0; pi < p->number_of_parameters(); pi++) {
      int off_b = in_bytes(ParametersTypeData::type_offset(pi));
      intptr_t* cell = (intptr_t*)(p_dp + off_b);
      *cell = TypeEntries::with_status((Klass*)nullptr, *cell);
    }
  }
}

static void collect_type_fixups(MethodData* mdo,
                                ProfileCheckpoint::SymtabBuilder& stb,
                                GrowableArray<ProfileCheckpoint::Fixup>& out_fixups) {
  // Helper lambda to create a fixup with loader_name properly set
  auto make_fixup = [&stb](InstanceKlass* ik, u4 offset) -> ProfileCheckpoint::Fixup {
    ProfileCheckpoint::Fixup fx;
    fx.offset_in_mdo = offset;
    fx.target.id = stb.intern(get_klassname_utf8(ik));
    fx.loader = loader_id_from_klass(ik);
    if (fx.loader == ProfileCheckpoint::LoaderId::NAMED) {
      const char* lname = get_loader_name_for_klass(ik);
      fx.loader_name.id = (lname != nullptr) ? stb.intern(lname) : 0xFFFFFFFF;
    } else {
      fx.loader_name.id = 0xFFFFFFFF;
    }
    return fx;
  };

  for (ProfileData* pd = mdo->first_data(); mdo->is_valid(pd); pd = mdo->next_data(pd)) {
    if (pd->is_VirtualCallData() || pd->is_ReceiverTypeData()) {
      ReceiverTypeData* rtd = pd->as_ReceiverTypeData();
      for (uint row = 0; row < rtd->row_limit(); row++) {
        int off_b = in_bytes(ReceiverTypeData::receiver_offset(row));
        intptr_t* cell = (intptr_t*)(pd->dp() + off_b);
        Klass* k = TypeEntries::valid_klass(*cell);
        if (k != nullptr && k->is_instance_klass()) {
          InstanceKlass* ik = InstanceKlass::cast(k);
          out_fixups.append(make_fixup(ik, (u4)((pd->dp() + off_b) - (address)mdo)));
        }
      }
    }
    if (pd->is_CallTypeData()) {
      CallTypeData* ctd = (CallTypeData*)pd;
      if (ctd->has_arguments()) {
        for (int ai = 0; ai < ctd->number_of_arguments(); ai++) {
          int off_b = in_bytes(ctd->argument_type_offset(ai));
          intptr_t* cell = (intptr_t*)(pd->dp() + off_b);
          Klass* k = TypeEntries::valid_klass(*cell);
          if (k != nullptr && k->is_instance_klass()) {
            InstanceKlass* ik = InstanceKlass::cast(k);
            out_fixups.append(make_fixup(ik, (u4)((pd->dp() + off_b) - (address)mdo)));
          }
        }
      }
      if (ctd->has_return()) {
        int off_b = in_bytes(ctd->return_type_offset());
        intptr_t* cell = (intptr_t*)(pd->dp() + off_b);
        Klass* k = TypeEntries::valid_klass(*cell);
        if (k != nullptr && k->is_instance_klass()) {
          InstanceKlass* ik = InstanceKlass::cast(k);
          out_fixups.append(make_fixup(ik, (u4)((pd->dp() + off_b) - (address)mdo)));
        }
      }
    }
    if (pd->is_VirtualCallTypeData()) {
      VirtualCallTypeData* vctd = pd->as_VirtualCallTypeData();
      if (vctd->has_arguments()) {
        for (int ai = 0; ai < vctd->number_of_arguments(); ai++) {
          int off_b = in_bytes(vctd->argument_type_offset(ai));
          intptr_t* cell = (intptr_t*)(pd->dp() + off_b);
          Klass* k = TypeEntries::valid_klass(*cell);
          if (k != nullptr && k->is_instance_klass()) {
            InstanceKlass* ik = InstanceKlass::cast(k);
            out_fixups.append(make_fixup(ik, (u4)((pd->dp() + off_b) - (address)mdo)));
          }
        }
      }
      if (vctd->has_return()) {
        int off_b = in_bytes(vctd->return_type_offset());
        intptr_t* cell = (intptr_t*)(pd->dp() + off_b);
        Klass* k = TypeEntries::valid_klass(*cell);
        if (k != nullptr && k->is_instance_klass()) {
          InstanceKlass* ik = InstanceKlass::cast(k);
          out_fixups.append(make_fixup(ik, (u4)((pd->dp() + off_b) - (address)mdo)));
        }
      }
    }
  }
  if (ParametersTypeData* p = mdo->parameters_type_data()) {
    address p_dp = p->dp();
    for (int pi = 0; pi < p->number_of_parameters(); pi++) {
      int off_b = in_bytes(ParametersTypeData::type_offset(pi));
      intptr_t* cell = (intptr_t*)(p_dp + off_b);
      Klass* k = TypeEntries::valid_klass(*cell);
      if (k != nullptr && k->is_instance_klass()) {
        InstanceKlass* ik = InstanceKlass::cast(k);
        out_fixups.append(make_fixup(ik, (u4)((p_dp + off_b) - (address)mdo)));
      }
    }
  }
}

static void apply_fixups(MethodData* mdo,
                         const ProfileCheckpoint::Fixup* fixups,
                         u4 fixup_count,
                         GrowableArray<char*>& symtab,
                         TRAPS) {
  for (u4 fi = 0; fi < fixup_count; fi++) {
    const ProfileCheckpoint::Fixup& fx = fixups[fi];
    if ((int)fx.target.id < 0 || (int)fx.target.id >= symtab.length()) {
      log_debug(compilation)("MDO checkpoint: invalid sym_id=%u (symtab_len=%d) at off=%u",
                             fx.target.id, symtab.length(), fx.offset_in_mdo);
      continue;
    }
    const char* cname = symtab.at((int)fx.target.id);
    // Get loader name from symtab if it's a NAMED loader
    const char* loader_name = nullptr;
    if (fx.loader == ProfileCheckpoint::LoaderId::NAMED && fx.loader_name.id != 0xFFFFFFFF) {
      if ((int)fx.loader_name.id >= 0 && (int)fx.loader_name.id < symtab.length()) {
        loader_name = symtab.at((int)fx.loader_name.id);
      }
    }
    InstanceKlass* k = resolve_klass_utf8(cname, fx.loader, loader_name, THREAD);
    if (k != nullptr) {
      if (!try_link_class(k, cname, fx.loader, "fixup link", THREAD)) {
        continue;
      }
      address cell_addr = (address)mdo + fx.offset_in_mdo;
      intptr_t* cell = (intptr_t*)cell_addr;
      *cell = TypeEntries::with_status(InstanceKlass::cast(k), *cell);
    } else {
      log_debug(compilation)("MDO checkpoint: fixup unresolved %s (loader=%d) at off=%u",
                             cname, (int)fx.loader, fx.offset_in_mdo);
    }
  }
}

// ============================================================================
// Load/install path (validate header, install records into live MDOs)
// ============================================================================
static bool validate_mdo_bcis(MethodData* mdo, Method* method) {
  int code_size = method->code_size();
  for (ProfileData* data = mdo->first_data();
       mdo->is_valid(data);
       data = mdo->next_data(data)) {
    int bci = data->bci();
    if (bci < 0 || bci >= code_size) {
      log_debug(compilation)("MDO checkpoint: invalid BCI %d (code_size=%d)", bci, code_size);
      return false;
    }
  }
  return true;
}

static bool copy_mdo_payload(MethodData* dst_mdo, const char* src_bytes, u4 src_size) {
  if (dst_mdo == nullptr) return false;
  if ((u4)dst_mdo->size_in_bytes() != src_size) return false;
  address dst_base = dst_mdo->data_base();
  const size_t data_sz  = (size_t)dst_mdo->data_size();
  const size_t data_off = (size_t)(dst_base - (address)dst_mdo);
  const char* src = src_bytes + data_off;
  Copy::conjoint_jbytes(src, (char*)dst_base, (jlong)data_sz);
  return true;
}

static void trigger_eager_compile(Method* target, u1 stored_level, JavaThread* thread) {
  if (!EagerCompileAfterLoad) return;
  if (target == nullptr || thread == nullptr) return;
  if (!UseCompiler || !CompilationPolicy::is_compilation_enabled()) return;
  if (target->is_abstract() || target->is_native()) {
    log_debug(compilation)("MDO checkpoint: eager compile SKIPPED (abstract/native) %s::%s%s",
                           target->method_holder()->name()->as_utf8(),
                           target->name()->as_utf8(), target->signature()->as_utf8());
    return;
  }
  InstanceKlass* holder = target->method_holder();
  if (!holder->is_initialized()) {
    log_info(compilation)("MDO checkpoint: eager compile SKIPPED (holder not initialized) %s::%s%s",
                          holder->name()->as_utf8(),
                          target->name()->as_utf8(), target->signature()->as_utf8());
    return;
  }
  CompLevel level = (CompLevel)stored_level;
  if (level < CompLevel_none) {
    level = CompLevel_none;
  } else if (level > CompLevel_full_optimization) {
    level = CompLevel_full_optimization;
  }
  JavaThread* THREAD = thread; // For exception macros.
  methodHandle mh(THREAD, target);
  log_info(compilation)("Eager compiling %s %s %s at level %u", target->name()->as_utf8(), target->signature()->as_utf8(), target->method_holder()->name()->as_utf8(), level);
  CompileBroker::compile_method(mh, InvocationEntryBci, level, 0, CompileTask::Reason_MustBeCompiled, THREAD);
  if (HAS_PENDING_EXCEPTION) { CLEAR_PENDING_EXCEPTION; }
}

static bool header_matches_current_vm(const ProfileCheckpoint::Header& hdr) {
  bool ok = true;

  const u2 expected_ptr_size = (u2)sizeof(void*);
  const u2 expected_endian = detect_endianness();
  if (hdr.pointer_size != expected_ptr_size) {
    log_warning(compilation)("MDO checkpoint: header mismatch: pointer_size file=%u vm=%u",
                             (unsigned)hdr.pointer_size, (unsigned)expected_ptr_size);
    ok = false;
  }
  if (hdr.endianness != expected_endian) {
    log_warning(compilation)("MDO checkpoint: header mismatch: endianness file=%u vm=%u",
                             (unsigned)hdr.endianness, (unsigned)expected_endian);
    ok = false;
  }

  const auto& lf = hdr.layout;
  const uint32_t exp_tpl = (uint32_t)TypeProfileLevel;
  const int32_t  exp_args = (int32_t)TypeProfileArgsLimit;
  const int32_t  exp_parms = (int32_t)TypeProfileParmsLimit;
  const int64_t  exp_width = (int64_t)TypeProfileWidth;
  const uint8_t  exp_traps = (uint8_t)(ProfileTraps ? 1 : 0);
  const uint8_t  exp_casts = (uint8_t)(TypeProfileCasts ? 1 : 0);
  const int32_t  exp_spec_extra = (int32_t)SpecTrapLimitExtraEntries;

  if (lf.type_profile_level != exp_tpl) {
    log_warning(compilation)("MDO checkpoint: header mismatch: TypeProfileLevel file=%u vm=%u",
                             (unsigned)lf.type_profile_level, (unsigned)exp_tpl);
    ok = false;
  }
  if (lf.type_profile_args_limit != exp_args) {
    log_warning(compilation)("MDO checkpoint: header mismatch: TypeProfileArgsLimit file=%d vm=%d",
                             (int)lf.type_profile_args_limit, (int)exp_args);
    ok = false;
  }
  if (lf.type_profile_parms_limit != exp_parms) {
    log_warning(compilation)("MDO checkpoint: header mismatch: TypeProfileParmsLimit file=%d vm=%d",
                             (int)lf.type_profile_parms_limit, (int)exp_parms);
    ok = false;
  }
  if (lf.type_profile_width != exp_width) {
    log_warning(compilation)("MDO checkpoint: header mismatch: TypeProfileWidth file=" INT64_FORMAT " vm=" INT64_FORMAT,
                             (int64_t)lf.type_profile_width, (int64_t)exp_width);
    ok = false;
  }
  if (lf.profile_traps != exp_traps) {
    log_warning(compilation)("MDO checkpoint: header mismatch: ProfileTraps file=%u vm=%u",
                             (unsigned)lf.profile_traps, (unsigned)exp_traps);
    ok = false;
  }
  if (lf.type_profile_casts != exp_casts) {
    log_warning(compilation)("MDO checkpoint: header mismatch: TypeProfileCasts file=%u vm=%u",
                             (unsigned)lf.type_profile_casts, (unsigned)exp_casts);
    ok = false;
  }
  if (lf.spec_trap_limit_extra_entries != exp_spec_extra) {
    log_warning(compilation)("MDO checkpoint: header mismatch: SpecTrapLimitExtraEntries file=%d vm=%d",
                             (int)lf.spec_trap_limit_extra_entries, (int)exp_spec_extra);
    ok = false;
  }

  return ok;
}

void ProfileCheckpoint::wait_for_compile_completion(JavaThread* THREAD) {
  if (!UseCompiler || !CompilationPolicy::is_compilation_enabled()) {
    return;
  }

  // Block until compile queues are drained and no active tasks remain.
  for (;;) {
    CompileBroker::wait_for_no_active_tasks();
    CompileQueue* q1 = CompileBroker::c1_compile_queue();
    CompileQueue* q2 = CompileBroker::c2_compile_queue();
    bool empty1 = (q1 == nullptr) || q1->is_empty();
    bool empty2 = (q2 == nullptr) || q2->is_empty();
    if (empty1 && empty2) break;
    os::naked_short_sleep(1);
  }
  log_info(compilation)("Eager compilation completed");
}

ProfileCheckpoint::Loader::Loader(JavaThread* thread)
  : _thread(thread),
    _records_read(0),
    _records_installed(0),
    _size_mismatch(0) {}

const char* ProfileCheckpoint::Loader::load_status_name(LoadStatus status) {
  switch (status) {
    case LoadStatus::Success:          return "success";
    case LoadStatus::MissingPath:      return "missing_path";
    case LoadStatus::FileOpenFailed:   return "file_open_failed";
    case LoadStatus::HeaderInvalid:    return "header_invalid";
    case LoadStatus::HeaderMismatch:   return "header_mismatch";
    case LoadStatus::SymtabReadFailed: return "symtab_read_failed";
    case LoadStatus::RecordReadFailed: return "record_read_failed";
    default:                           return "unknown";
  }
}

bool ProfileCheckpoint::Loader::install_record(const Record& rec,
                                               Fixup* fixups,
                                               char* mdo_bytes,
                                               char* mc_bytes,
                                               char* header_bytes,
                                               GrowableArray<char*>& symtab) {
  JavaThread* THREAD = _thread; // For exception macros.
  const char* kname = symtab.at((int)rec.key.klass.id);
  const char* mname = symtab.at((int)rec.key.name.id);
  const char* msig  = symtab.at((int)rec.key.sig.id);
  // Get loader name from symtab if it's a NAMED loader
  const char* loader_name = nullptr;
  if (rec.key.loader == LoaderId::NAMED && rec.key.loader_name.id != 0xFFFFFFFF) {
    if ((int)rec.key.loader_name.id >= 0 && (int)rec.key.loader_name.id < symtab.length()) {
      loader_name = symtab.at((int)rec.key.loader_name.id);
    }
  }

  InstanceKlass* holder = resolve_klass_utf8(kname, rec.key.loader, loader_name, THREAD);
  if (holder == nullptr) {
    log_debug(compilation)("MDO checkpoint: resolve class failed for %s", kname);
    return false;
  }
  Method* target = resolve_method_utf8(holder, mname, msig);
  if (target == nullptr) {
    log_debug(compilation)("MDO checkpoint: resolve method failed for %s %s %s", kname, mname, msig);
    return false;
  }

  if (!try_link_class(holder, kname, rec.key.loader, "record holder link", THREAD)) {
    return false;
  }

  if (target->method_data() == nullptr) {
    methodHandle mh(THREAD, target);
    target->build_profiling_method_data(mh, THREAD);
    if (HAS_PENDING_EXCEPTION) { CLEAR_PENDING_EXCEPTION; }
  }

  MethodData* mdo = target->method_data();
  if (mdo == nullptr) {
    log_debug(compilation)("MDO checkpoint: MethodData allocation/build failed for %s %s %s", kname, mname, msig);
    return false;
  }

  if (!copy_mdo_payload(mdo, mdo_bytes, rec.mdo_size)) {
    log_debug(compilation)("MDO checkpoint: copy payload failed for %s %s %s (size=%u)", kname, mname, msig, rec.mdo_size);
    _size_mismatch++;
    return false;
  }

  bool mdo_valid = true;

  if (rec.key.bytecode_crc32 != 0) {
    uint32_t current_crc = (uint32_t)ClassLoader::crc32(0, (const char*)target->code_base(), target->code_size());
    if (current_crc != rec.key.bytecode_crc32) {
      log_debug(compilation)("MDO checkpoint: bytecode CRC32 mismatch for %s %s %s (stored=0x%08x current=0x%08x)",
                             kname, mname, msig, rec.key.bytecode_crc32, current_crc);
      mdo_valid = false;
    }
  }

  if (header_bytes != nullptr) {
    if (rec.header_size == sizeof(MethodData::HeaderSnapshot)) {
      MethodData::HeaderSnapshot snapshot;
      Copy::conjoint_jbytes(header_bytes, (char*)&snapshot, rec.header_size);
      if (!mdo->restore_header(snapshot)) {
        log_debug(compilation)("MDO checkpoint: header restore mismatch for %s %s %s", kname, mname, msig);
        mdo_valid = false;
      }
    } else {
      log_debug(compilation)("MDO checkpoint: header size mismatch for %s %s %s (rec=%u expected=%zu)",
                             kname, mname, msig, rec.header_size, sizeof(MethodData::HeaderSnapshot));
      mdo_valid = false;
    }
  }

  if (mdo_valid) {
    mdo_valid = validate_mdo_bcis(mdo, target);
  }

  sanitize_type_entries(mdo);
  if (fixups != nullptr && rec.fixup_count > 0) {
    apply_fixups(mdo, fixups, rec.fixup_count, symtab, THREAD);
  }

  _records_installed++;

  if (rec.mc_size > 0) {
    MethodCounters* mc = target->method_counters();
    if (mc == nullptr) {
      methodHandle mh(THREAD, target);
      MethodCounters* ensured = Method::build_method_counters(THREAD, target);
      if (HAS_PENDING_EXCEPTION) { CLEAR_PENDING_EXCEPTION; }
      mc = ensured;
    }
    if (mc != nullptr) {
      const size_t ic_sz = sizeof(InvocationCounter);
      if (rec.mc_size >= ic_sz * 2 + sizeof(jlong) + sizeof(float) + sizeof(jint)) {
        char* p = mc_bytes;
        Copy::conjoint_jbytes(p, (char*)mc->invocation_counter(), (jlong)ic_sz); p += ic_sz;
        Copy::conjoint_jbytes(p, (char*)mc->backedge_counter(), (jlong)ic_sz); p += ic_sz;
        jlong prev_time = *(jlong*)p; p += sizeof(jlong);
        mc->set_prev_time(prev_time);
        float rate = *(float*)p; p += sizeof(float);
        mc->set_rate(rate);
        jint pec = *(jint*)p; p += sizeof(jint);
        mc->set_prev_event_count(pec);
      }
    }
  }

  if (mdo_valid) {
    trigger_eager_compile(target, rec.comp_level, THREAD);
  } else {
    log_debug(compilation)("MDO checkpoint: skipping eager compile for %s %s %s (invalid MDO)", kname, mname, msig);
  }

  if (PrintMDOAfterLoad) {
    tty->print_cr("[AfterLoad] %s %s %s", kname, mname, msig);
    tty->print_cr("[MethodDataHeader]");
    print_mdo_header(mdo);
    tty->print_cr("[MethodData]");
    mdo->print_data_on(tty);
    tty->print_cr("[MethodCounters]");
    MethodCounters* mc = target->method_counters();
    if (mc != nullptr) {
      mc->print_data_on(tty);
    } else {
      tty->print_cr("  (none)");
    }
  }

  return true;
}

ProfileCheckpoint::Loader::LoadResult ProfileCheckpoint::Loader::load_from_file(const char* path) {
  LoadResult result{LoadStatus::MissingPath, 0, 0, 0};
  if (path == nullptr) {
    return result;
  }

  FILE* f = os::fopen(path, "rb");
  if (f == nullptr) {
    result.status = LoadStatus::FileOpenFailed;
    return result;
  }

  ResourceMark rm;
  Header hdr;
  if (!read_header(f, hdr)) {
    fclose(f);
    result.status = LoadStatus::HeaderInvalid;
    return result;
  }
  if (!header_matches_current_vm(hdr)) {
    fclose(f);
    result.status = LoadStatus::HeaderMismatch;
    return result;
  }
  u4 sym_count = hdr.sym_count;
  u4 rec_total = hdr.rec_count;
  GrowableArray<char*> symtab((int)sym_count);
  if (!read_symtab(f, symtab, sym_count)) {
    fclose(f);
    result.status = LoadStatus::SymtabReadFailed;
    return result;
  }
  struct SymtabCleanup {
    GrowableArray<char*>& _syms;
    explicit SymtabCleanup(GrowableArray<char*>& syms) : _syms(syms) {}
    ~SymtabCleanup() {
      for (int i = 0; i < _syms.length(); i++) {
        if (_syms.at(i) != nullptr) {
          os::free(_syms.at(i));
        }
      }
    }
  } symtab_cleanup(symtab);
  GrowableArray<ProfileCheckpoint::Class> classes((int)hdr.class_count);
  if (!read_classes(f, classes, hdr.class_count)) {
    fclose(f);
    result.status = LoadStatus::RecordReadFailed;
    return result;
  }

  JavaThread* THREAD = _thread; // For exception macros.

  GrowableArray<const char*> eager_init_allowlist(8);
  build_eager_init_allowlist(eager_init_allowlist);
  const bool eager_init_enabled = EagerInitAfterLoad && eager_init_allowlist.length() > 0;
  if (EagerInitAfterLoad && !eager_init_enabled) {
    log_warning(compilation)("MDO checkpoint: EagerInitAfterLoad is enabled but "
                             "EagerInitAfterLoadAllowlist is empty; using link-only preload");
  } else if (eager_init_enabled) {
    log_info(compilation)("MDO checkpoint: EagerInitAfterLoad enabled with %d allowlist pattern(s)",
                          eager_init_allowlist.length());
  }

  // Two-pass class preload:
  // Pass 1: resolve all regular (non-hidden) classes first, so they're available
  //         in ClassLoaderDataGraph when DynoLocatorScan resolves hidden classes.
  // Pass 2: resolve hidden classes (locators like @bci/@cpi that reference
  //         enclosing classes loaded in pass 1).
  int classes_total = classes.length();
  int classes_resolved = 0;
  int classes_init_attempted = 0;
  int classes_initialized = 0;
  int classes_init_failed = 0;
  int classes_init_skipped = 0;
  int classes_resolve_failed = 0;
  int classes_hidden = 0;
  int classes_hidden_resolved = 0;
  int classes_linked_only = 0;
  int classes_link_failed = 0;

  // Helper lambda for resolving and initializing/linking a class entry
  auto resolve_class_entry = [&](int ci) {
    const ProfileCheckpoint::Class& cls = classes.at(ci);
    if ((int)cls.klass.id >= symtab.length()) return;
    const char* cname = symtab.at((int)cls.klass.id);
    bool is_hidden_locator = (cls.loader == LoaderId::HIDDEN) || (cname != nullptr && cname[0] == '@');
    if (is_hidden_locator) classes_hidden++;
    const char* loader_name = nullptr;
    if (cls.loader == LoaderId::NAMED && cls.loader_name.id != 0xFFFFFFFF) {
      if ((int)cls.loader_name.id >= 0 && (int)cls.loader_name.id < symtab.length()) {
        loader_name = symtab.at((int)cls.loader_name.id);
      }
    }
    InstanceKlass* holder = resolve_klass_utf8(cname, cls.loader, loader_name, THREAD);
    if (holder != nullptr) {
      classes_resolved++;
      if (is_hidden_locator) classes_hidden_resolved++;
      const bool can_eager_init = is_builtin_loader(cls.loader) &&
                                  class_matches_eager_init_allowlist(cname, eager_init_allowlist);
      if (eager_init_enabled && can_eager_init) {
        classes_init_attempted++;
        if (try_initialize_class(holder, cname, cls.loader, "preload initialize", THREAD)) {
          classes_initialized++;
        } else {
          classes_init_failed++;
          if (try_link_class(holder, cname, cls.loader, "preload fallback link", THREAD)) {
            classes_linked_only++;
          } else {
            classes_link_failed++;
          }
        }
      } else {
        if (is_builtin_loader(cls.loader)) {
          classes_init_skipped++;
        }
        if (try_link_class(holder, cname, cls.loader, "preload link", THREAD)) {
          classes_linked_only++;
        } else {
          classes_link_failed++;
        }
        if (!is_builtin_loader(cls.loader)) {
          log_debug(compilation)("MDO checkpoint: link-only for custom-loader class %s (loader=%d)",
                                 cname, (int)cls.loader);
        }
      }
    } else {
      classes_resolve_failed++;
      log_info(compilation)("MDO checkpoint: resolve FAILED for %s (loader=%d, hidden=%s)",
                            cname, (int)cls.loader, is_hidden_locator ? "yes" : "no");
    }
  };

  // Pass 1: regular classes (non-hidden)
  for (int ci = 0; ci < classes.length(); ci++) {
    if ((int)classes.at(ci).klass.id >= symtab.length()) continue;
    const char* cname = symtab.at((int)classes.at(ci).klass.id);
    bool is_hidden = (classes.at(ci).loader == LoaderId::HIDDEN) || (cname != nullptr && cname[0] == '@');
    if (!is_hidden) {
      resolve_class_entry(ci);
    }
  }
  log_info(compilation)("MDO checkpoint: pass 1 (regular classes) done: resolved=%d", classes_resolved);

  // Pass 2: hidden classes (now enclosing classes are loaded)
  for (int ci = 0; ci < classes.length(); ci++) {
    if ((int)classes.at(ci).klass.id >= symtab.length()) continue;
    const char* cname = symtab.at((int)classes.at(ci).klass.id);
    bool is_hidden = (classes.at(ci).loader == LoaderId::HIDDEN) || (cname != nullptr && cname[0] == '@');
    if (is_hidden) {
      resolve_class_entry(ci);
    }
  }

  log_info(compilation)("MDO checkpoint: class preload summary: total=%d resolved=%d "
                         "init_attempted=%d initialized=%d init_failed=%d init_skipped=%d "
                         "linked_only=%d link_failed=%d resolve_failed=%d hidden=%d hidden_resolved=%d",
                         classes_total, classes_resolved,
                         classes_init_attempted, classes_initialized,
                         classes_init_failed, classes_init_skipped,
                         classes_linked_only, classes_link_failed,
                         classes_resolve_failed, classes_hidden, classes_hidden_resolved);

  result.status = LoadStatus::Success;
  
  // process each record
  for (u4 i = 0; i < rec_total; i++) {
    Record rec;
    Fixup* fixups = nullptr;
    char* mdo_bytes = nullptr;
    char* mc_bytes = nullptr;
    char* header_bytes = nullptr;

    if (!read_record(f, rec, fixups, mdo_bytes, mc_bytes, header_bytes)) {
      log_debug(compilation)("MDO checkpoint: record read failed at %u", i);
      result.status = LoadStatus::RecordReadFailed;
      if (fixups) os::free(fixups);
      if (mdo_bytes) os::free(mdo_bytes);
      if (mc_bytes) os::free(mc_bytes);
      if (header_bytes) os::free(header_bytes);
      break;
    }

    _records_read++;

    if (rec.mdo_size == 0) {
      log_debug(compilation)("MDO checkpoint: empty MDO payload for sym ids %u %u %u",
                             rec.key.klass.id, rec.key.name.id, rec.key.sig.id);
      if (fixups) os::free(fixups);
      if (mdo_bytes) os::free(mdo_bytes);
      if (mc_bytes) os::free(mc_bytes);
      continue;
    }

    install_record(rec, fixups, mdo_bytes, mc_bytes, header_bytes, symtab);

    if (fixups != nullptr) os::free(fixups);
    if (mdo_bytes != nullptr) os::free(mdo_bytes);
    if (mc_bytes != nullptr) os::free(mc_bytes);
    if (header_bytes != nullptr) os::free(header_bytes);
  }

  fclose(f);

  result.records_read = _records_read;
  result.records_installed = _records_installed;
  result.size_mismatch = _size_mismatch;
  log_info(compilation)("MDO checkpoint: loaded %d records (%d installed, %d size mismatch)",
                         _records_read, _records_installed, _size_mismatch);
  return result;
}

// ============================================================================
// Dump enumeration helpers (collect classes/methods, build symtab)
// ============================================================================
// Build RecMeta (holder/name/sig/mdo size/pointer) for a method with an MDO
static ProfileCheckpoint::RecMeta make_rec_meta_for_method(Method* m) {
  ProfileCheckpoint::RecMeta r{};
  MethodData* mdo = m->method_data();
  // caller guarantees mdo != nullptr
  MutexLocker ml(mdo->extra_data_lock(), Mutex::_no_safepoint_check_flag);
  InstanceKlass* holder = m->method_holder();
  r.kname = get_klassname_utf8(holder);
  r.mname = m->name()->as_utf8();
  r.sig   = m->signature()->as_utf8();
  r.mdo_size = (u4)mdo->size_in_bytes();
  r.mdo_ptr  = (const void*)mdo;
  r.comp_level = (u1)CompLevel_none;
  if (UseCompiler) {
    CompLevel level = CompLevel_none;
    nmethod* code = m->code();
    if (code != nullptr && code->is_in_use()) {
      level = (CompLevel)code->comp_level();
    } else {
      level = (CompLevel)m->highest_comp_level();
    }
    if (level < CompLevel_none) {
      level = CompLevel_none;
    } else if (level > CompLevel_full_optimization) {
      level = CompLevel_full_optimization;
    }
    r.comp_level = (u1)level;
  }
  return r;
}

// Get all methods with MDO data
class CollectClassesClosure : public KlassClosure {
  ProfileCheckpoint::SymtabBuilder* _stb;
  GrowableArray<ProfileCheckpoint::Class>* _classes;
public:
  CollectClassesClosure(ProfileCheckpoint::SymtabBuilder* stb,
                        GrowableArray<ProfileCheckpoint::Class>* classes)
    : _stb(stb), _classes(classes) {}

  virtual void do_klass(Klass* k) {
    if (k == nullptr || !k->is_instance_klass()) return;
    InstanceKlass* ik = InstanceKlass::cast(k);
    // Skip hidden classes that have no dyno locator — they can never be
    // resolved on the load side and just waste space in the checkpoint.
    if (ik->is_hidden()) {
      const char* loc = DynoLocatorScan::lookup(ik);
      if (loc == nullptr) {
        log_debug(compilation)("MDO checkpoint dump: skipping hidden class without locator: %s",
                               ik->name()->as_utf8());
        return;
      }
    }
    ProfileCheckpoint::Class c;
    c.loader = loader_id_from_klass(ik);
    // For NAMED loaders, intern the loader name
    if (c.loader == ProfileCheckpoint::LoaderId::NAMED) {
      const char* lname = get_loader_name_for_klass(ik);
      c.loader_name.id = (lname != nullptr) ? _stb->intern(lname) : 0xFFFFFFFF;
    } else {
      c.loader_name.id = 0xFFFFFFFF;
    }
    c.klass.id = _stb->intern(get_klassname_utf8(ik));
    _classes->append(c);
  }
};

static void collect_classes(ProfileCheckpoint::SymtabBuilder& stb,
                            GrowableArray<ProfileCheckpoint::Class>& classes) {
  CollectClassesClosure closure(&stb, &classes);
  ClassLoaderDataGraph::classes_do(&closure);
}

GrowableArray<Method*> get_methods_with_mdo() {
  GrowableArray<Method*> methods(1024);
  static GrowableArray<Method*>* g_methods;
  g_methods = &methods;
  auto collect_with_mdo = [](Method* m) {
    if (m != nullptr && m->method_data() != nullptr) {
      g_methods->push(m);
    }
  };
  ClassLoaderDataGraph::methods_do(collect_with_mdo);
  return methods;
}

u4 ProfileCheckpoint::SymtabBuilder::intern(const char* s) {
  assert(!_frozen, "frozen");
  for (int i = 0; i < _syms.length(); i++) {
    if (strcmp(_syms.at(i), s) == 0) return (u4)i;
  }
  _syms.append(s);
  return (u4)(_syms.length() - 1);
}

u4 ProfileCheckpoint::SymtabBuilder::id_of(const char* s) const {
  assert(_frozen, "must freeze before lookups");
  for (int i = 0; i < _syms.length(); i++) {
    if (strcmp(_syms.at(i), s) == 0) return (u4)i;
  }
  assert(false, "symbol not found in symtab");
  return (u4)UINT_MAX;
}

// ============================================================================
// Public entrypoints (ProfileCheckpoint::load / dump_to_stream)
// ============================================================================
void ProfileCheckpoint::load(JavaThread* THREAD) {
  if (!LoadMDOAtStartup || MDOReplayLoadFile == nullptr) return;
  Loader loader(THREAD);
  log_info(compilation)("MDO checkpoint: initiating load from %s", MDOReplayLoadFile);
  Loader::LoadResult res = loader.load_from_file(MDOReplayLoadFile);
  if (res.ok()) {
    log_info(compilation)("MDO checkpoint: loaded %d records (%d installed, %d size mismatch)",
                          res.records_read, res.records_installed, res.size_mismatch);
    wait_for_compile_completion(THREAD);
  } else {
    log_warning(compilation)("MDO checkpoint: load failed (status=%s, read=%d, installed=%d, mismatches=%d)",
                             Loader::load_status_name(res.status),
                             res.records_read,
                             res.records_installed,
                             res.size_mismatch);
  }
}

void ProfileCheckpoint::dump_to_stream(fileStream* out) {
  // Opportunistically scan resolved indy/invokehandle sites to harvest locators
  // for hidden classes before dumping.
  DynoLocatorScan::scan_all_classes();

  if (out == nullptr) {
    log_warning(compilation)("MDO checkpoint: dump failed (null output stream)");
    return;
  }

  bool ok = true;
  u4 emitted = 0;

  do {
    ResourceMark rm;
    SymtabBuilder stb;
    GrowableArray<ProfileCheckpoint::Class> classes(1024);
    collect_classes(stb, classes);
    GrowableArray<Method*> methods_with_mdo = get_methods_with_mdo();
    GrowableArray<RecMeta> rec_metas(1024);

    // create rec_metas for methods with MDO
    for (int i = 0; i < methods_with_mdo.length(); i++) {
      Method* m = methods_with_mdo.at(i);
      if (m->method_data() == nullptr) {
        log_warning(compilation)("MDO checkpoint: method %s %s %s has no MDO", m->name()->as_utf8(), m->signature()->as_utf8(), m->method_holder()->name()->as_utf8());
        ok = false; break;
      }
      rec_metas.append(make_rec_meta_for_method(m));
    }

    // create fixups for each rec_meta
    GrowableArray< GrowableArray<Fixup>* > fixups_per_rec(rec_metas.length());
    for (int ri = 0; ri < rec_metas.length(); ri++) {
      Method* m = methods_with_mdo.at(ri);
      MethodData* mdo = m->method_data();
      GrowableArray<Fixup>* fx = new GrowableArray<Fixup>(16);
      collect_type_fixups(mdo, stb, *fx);
      fixups_per_rec.append(fx);
    }
    
    // store all symbols in symtab
    for (int ri = 0; ri < rec_metas.length(); ri++) {
      stb.intern(rec_metas.at(ri).kname);
      stb.intern(rec_metas.at(ri).mname);
      stb.intern(rec_metas.at(ri).sig);
      // Also intern loader names for NAMED loaders
      Method* m = methods_with_mdo.at(ri);
      LoaderId lid = loader_id_from_klass(m->method_holder());
      if (lid == LoaderId::NAMED) {
        const char* lname = get_loader_name_for_klass(m->method_holder());
        if (lname != nullptr) stb.intern(lname);
      }
    }
    stb.freeze();

    // write header, symtab, classes
    Header h;
    init_header(h, stb.length(), (u4)rec_metas.length(), (u4)classes.length());
    if (!write_header(out, h)) { ok = false; break; }
    if (!write_symtab(out, stb.symbols())) { ok = false; break; }
    if (!write_classes(out, classes)) { ok = false; break; }

    // write each record
    for (int ri = 0; ri < rec_metas.length(); ri++) {
      const RecMeta& rec_meta = rec_metas.at(ri);
      Method* m = methods_with_mdo.at(ri);
      MethodData* mdo = m->method_data();
      Record rec;

      rec.key.loader = loader_id_from_klass(m->method_holder());
      // For NAMED loaders, set loader_name
      if (rec.key.loader == ProfileCheckpoint::LoaderId::NAMED) {
        const char* lname = get_loader_name_for_klass(m->method_holder());
        rec.key.loader_name.id = (lname != nullptr) ? stb.id_of(lname) : 0xFFFFFFFF;
      } else {
        rec.key.loader_name.id = 0xFFFFFFFF;
      }
      rec.key.klass.id = stb.id_of(rec_meta.kname);
      rec.key.name.id  = stb.id_of(rec_meta.mname);
      rec.key.sig.id   = stb.id_of(rec_meta.sig);
      rec.key.bytecode_crc32 = (uint32_t)ClassLoader::crc32(0, (const char*)m->code_base(), m->code_size());
      rec.mdo_size = rec_meta.mdo_size;
      rec.comp_level = rec_meta.comp_level;
      GrowableArray<Fixup>* fx_entries = fixups_per_rec.at(ri);
      rec.fixup_count = (u4)fx_entries->length();
      
      // create mc_bytes and header_bytes
      const void* mc_bytes = nullptr;
      MethodData::HeaderSnapshot header_snapshot;
      mdo->snapshot_header(&header_snapshot);
      rec.header_size = sizeof(header_snapshot);
      const void* header_bytes = &header_snapshot;
      GrowableArray<char> mc_buf(0);
      MethodCounters* mc = m->method_counters();
      if (mc != nullptr) {
        const size_t ic_sz = sizeof(InvocationCounter);
        const size_t mc_sz = ic_sz * 2 + sizeof(jlong) + sizeof(float) + sizeof(jint);
        for (size_t fill = 0; fill < mc_sz; fill++) mc_buf.append((char)0);
        char* p = mc_buf.adr_at(0);
        Copy::conjoint_jbytes((char*)mc->invocation_counter(), p, (jlong)ic_sz);
        p += ic_sz;
        Copy::conjoint_jbytes((char*)mc->backedge_counter(), p, (jlong)ic_sz);
        p += ic_sz;
        *(jlong*)p = mc->prev_time(); p += sizeof(jlong);
        *(float*)p = mc->rate(); p += sizeof(float);
        *(jint*)p = mc->prev_event_count(); p += sizeof(jint);
        rec.mc_size = (u4)mc_sz;
        mc_bytes = mc_buf.adr_at(0);
      } else {
        rec.mc_size = 0;
      }
      Fixup* fixup_buf = rec.fixup_count ? fx_entries->adr_at(0) : nullptr;
      if (!write_record(out, rec, rec_meta.mdo_ptr, fixup_buf, mc_bytes, header_bytes)) { ok = false; break; }

      if (PrintMDOAtDump) {
        const char* kname = rec_meta.kname;
        const char* mname = rec_meta.mname;
        const char* sig   = rec_meta.sig;
        u1 comp_level = rec_meta.comp_level;
        tty->print_cr("[Dump] %s %s %s %u", kname, mname, sig, comp_level);
        tty->print_cr("[MethodDataHeader]");
        print_mdo_header(mdo);
        tty->print_cr("[MethodData]");
        mdo->print_data_on(tty);
        tty->print_cr("[MethodCounters]");
        MethodCounters* mc_print = m->method_counters();
        if (mc_print != nullptr) {
          mc_print->print_data_on(tty);
        } else {
          tty->print_cr("  (none)");
        }
      }
      emitted++;
    }
  } while (false);

  if (!ok) {
    log_warning(compilation)("MDO checkpoint: dump failed");
    return;
  }

  log_info(compilation)("MDO checkpoint: dumped %u MDOs", emitted);
}
