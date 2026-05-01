#include "services/dynoLocatorScan.hpp"

#include "classfile/javaClasses.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/symbolTable.hpp"
#include "ci/ciReplay.hpp"
#include "interpreter/bytecodeStream.hpp"
#include "memory/resourceArea.hpp"
#include "oops/constantPool.inline.hpp"
#include "oops/cpCache.inline.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/method.hpp"
#include "oops/oop.inline.hpp"
#include "oops/resolvedIndyEntry.hpp"
#include "oops/symbol.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/thread.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/threads.hpp"
#include "runtime/mutex.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "runtime/threadSMR.hpp"
#include "interpreter/linkResolver.hpp"
#include "ci/ciReplay.hpp"
#include "oops/objArrayOop.inline.hpp"
#include "utilities/debug.hpp"
#include "utilities/utf8.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/ostream.hpp"
#include <cstdarg>
#include <cctype>
#include <cstring>

namespace {

// Limit borrowed from ciEnv::_dyno_name
static const int LOC_BUF_LEN = 1024;

class DynoLocatorTable {
  class Entry {
  public:
    InstanceKlass* _ik;
    const char*    _loc;
    Entry() : _ik(nullptr), _loc(nullptr) {}
    Entry(InstanceKlass* ik, const char* loc) : _ik(ik), _loc(loc) {}
  };
  static GrowableArray<Entry>* _entries;
  static Mutex* _lock;
  static void ensure_init() {
    if (_entries == nullptr) {
      _entries = new (mtInternal) GrowableArray<Entry>(8, mtInternal);
    }
    if (_lock == nullptr) {
      _lock = new Mutex(Mutex::nosafepoint, "DynoLocatorTable");
    }
  }
public:
  static void record(InstanceKlass* ik, const char* loc) {
    log_debug(compilation)("recording ik: %s with name %s", ik->name()->as_utf8(), loc);
    if (ik == nullptr || loc == nullptr) return;
    ensure_init();
    MutexLocker ml(_lock, Mutex::_no_safepoint_check_flag);
    for (int i = 0; i < _entries->length(); i++) {
      if (_entries->at(i)._ik == ik) return;
    }
    const char* copy = os::strdup(loc, mtInternal);
    _entries->append(Entry(ik, copy));
  }
  static const char* lookup(InstanceKlass* ik) {
    if (_entries == nullptr || ik == nullptr) return nullptr;
    MutexLocker ml(_lock, Mutex::_no_safepoint_check_flag);
    for (int i = 0; i < _entries->length(); i++) {
      if (_entries->at(i)._ik == ik) return _entries->at(i)._loc;
    }
    return nullptr;
  }
};

GrowableArray<DynoLocatorTable::Entry>* DynoLocatorTable::_entries = nullptr;
Mutex* DynoLocatorTable::_lock = nullptr;

// --- Hidden locator parser (based on ciReplay) ---
class HiddenLocatorParser {
  JavaThread* _jt;
  char* _buf;
  char* _p;

  static void unescape_string(char* value) {
    char* from = value;
    char* to = value;
    while (*from != '\0') {
      if (*from != '\\') {
        *to++ = *from++;
      } else {
        switch (from[1]) {
          case 'u': {
            from += 2;
            jchar v = 0;
            for (int i = 0; i < 4; i++) {
              char c = *from++;
              switch (c) {
                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9':
                  v = (v << 4) + c - '0';
                  break;
                case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
                  v = (v << 4) + 10 + c - 'a';
                  break;
                case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
                  v = (v << 4) + 10 + c - 'A';
                  break;
                default:
                  ShouldNotReachHere();
              }
            }
            UNICODE::convert_to_utf8(&v, 1, to);
            to++;
            break;
          }
          case 't': *to++ = '\t'; from += 2; break;
          case 'n': *to++ = '\n'; from += 2; break;
          case 'r': *to++ = '\r'; from += 2; break;
          case 'f': *to++ = '\f'; from += 2; break;
          default:
            ShouldNotReachHere();
        }
      }
    }
    *from = *to;
  }

  void skip_ws() {
    while (*_p == ' ' || *_p == '\t') _p++;
  }

  char* parse_token() {
    skip_ws();
    if (*_p == '\0') return nullptr;
    if (*_p == ';') {
      _p++;
      return (char*)";";
    }
    char* tok = nullptr;
    if (*_p == '"') {
      _p++;
      tok = _p;
      while (*_p != '\0' && *_p != '"') {
        if (*_p == '\\' && _p[1] != '\0') {
          _p += 2; // skip escaped char
        } else {
          _p++;
        }
      }
      if (*_p == '"') {
        *_p = '\0';
        _p++;
      }
    } else {
      tok = _p;
      while (*_p != ' ' && *_p != '\t' && *_p != '\0' && *_p != ';') _p++;
      if (*_p != '\0') { *_p = '\0'; _p++; }
    }
    if (tok != nullptr && !(tok[0] == ';' && tok[1] == '\0')) {
      unescape_string(tok);
    }
    return tok;
  }

  bool parse_terminator() {
    skip_ws();
    if (*_p == ';') {
      _p++;
      return true;
    }
    return false;
  }

  int parse_int(bool* ok) {
    skip_ws();
    char* endp = nullptr;
    long v = strtol(_p, &endp, 10);
    if (endp == _p) { if (ok) *ok = false; return 0; }
    _p = endp;
    if (ok) *ok = true;
    return (int)v;
  }

  InstanceKlass* parse_bci() {
    char* klass = parse_token();
    char* mname = parse_token();
    char* msig  = parse_token();
    bool ok = true;
    int bci = parse_int(&ok);
    if (!ok || klass == nullptr || mname == nullptr || msig == nullptr) return nullptr;
    Symbol* ksym = SymbolTable::new_symbol(klass);
    Symbol* mnsym = SymbolTable::new_symbol(mname);
    Symbol* mssym = SymbolTable::new_symbol(msig);
    Handle loader(_jt, SystemDictionary::java_system_loader());
    InstanceKlass* ik = InstanceKlass::cast(SystemDictionary::resolve_or_fail(ksym, loader, true, _jt));
    if (_jt->has_pending_exception()) { _jt->clear_pending_exception(); return nullptr; }
    ik->link_class(_jt);
    if (_jt->has_pending_exception()) { _jt->clear_pending_exception(); return nullptr; }
    Method* m = ik->find_method(mnsym, mssym);
    if (m == nullptr) return nullptr;
    methodHandle caller(_jt, m);
    if (m->validate_bci(bci) != bci) {
      return nullptr;
    }
    Bytecode_invoke bytecode = Bytecode_invoke_check(caller, bci);
    if (!Bytecodes::is_defined(bytecode.code()) || !bytecode.is_valid()) return nullptr;
    bytecode.verify();
    int index = bytecode.index();
    const constantPoolHandle cp(_jt, ik->constants());
    CallInfo callInfo;
    Bytecodes::Code bc = bytecode.invoke_code();
    LinkResolver::resolve_invoke(callInfo, Handle(), cp, index, bc, _jt);
    if (_jt->has_pending_exception()) { _jt->clear_pending_exception(); return nullptr; }
    oop appendix = nullptr;
    Method* adapter_method = nullptr;
    int pool_index = 0;
    if (bytecode.is_invokedynamic()) {
      cp->cache()->set_dynamic_call(callInfo, index);
      appendix = cp->resolved_reference_from_indy(index);
      adapter_method = cp->resolved_indy_entry_at(index)->method();
      pool_index = cp->resolved_indy_entry_at(index)->constant_pool_index();
    } else if (bytecode.is_invokehandle()) {
      ResolvedMethodEntry* method_entry = cp->cache()->set_method_handle(index, callInfo);
      appendix = cp->cache()->appendix_if_resolved(method_entry);
      adapter_method = method_entry->method();
      pool_index = method_entry->constant_pool_index();
    } else {
      return nullptr;
    }
    char* dyno_ref = parse_token();
    if (dyno_ref == nullptr) return nullptr;
    oop obj = nullptr;
    if (strcmp(dyno_ref, "<appendix>") == 0) {
      obj = appendix;
    } else if (strcmp(dyno_ref, "<adapter>") == 0) {
      if (!parse_terminator()) return nullptr;
      return (adapter_method != nullptr) ? adapter_method->method_holder() : nullptr;
    } else if (strcmp(dyno_ref, "<bsm>") == 0) {
      BootstrapInfo bs(cp, pool_index, index);
      obj = cp->resolve_possibly_cached_constant_at(bs.bsm_index(), _jt);
    } else {
      return nullptr;
    }
    if (obj == nullptr) {
      return nullptr;
    }
    char* field = parse_token();
    while (field != nullptr && strcmp(field, ";") != 0) {
      if (strcmp(field, "<vmtarget>") == 0) {
        Method* vmtarget = java_lang_invoke_MemberName::vmtarget(obj);
        InstanceKlass* res = (vmtarget != nullptr) ? vmtarget->method_holder() : nullptr;
        if (!parse_terminator()) return nullptr;
        return res;
      }
      obj = ciReplay::obj_field(obj, field);
      if (obj == nullptr) {
        return nullptr;
      }
      if (obj->is_objArray()) {
        bool idx_ok = true;
        int index = parse_int(&idx_ok);
        if (!idx_ok || index >= ((objArrayOop)obj)->length()) return nullptr;
        obj = ((objArrayOop)obj)->obj_at(index);
      }
      field = parse_token();
    }
    if (field == nullptr) {
      if (!parse_terminator()) return nullptr;
    }
    if (obj == nullptr) {
      return nullptr;
    }
    return obj->klass()->is_instance_klass() ? InstanceKlass::cast(obj->klass()) : nullptr;
  }

  InstanceKlass* parse_cpi() {
    char* klass = parse_token();
    bool ok = true;
    int cpi = parse_int(&ok);
    if (!ok || klass == nullptr) return nullptr;
    Symbol* ksym = SymbolTable::new_symbol(klass);
    Handle loader(_jt, SystemDictionary::java_system_loader());
    InstanceKlass* ik = InstanceKlass::cast(SystemDictionary::resolve_or_fail(ksym, loader, true, _jt));
    if (_jt->has_pending_exception()) { _jt->clear_pending_exception(); return nullptr; }
    ik->link_class(_jt);
    if (_jt->has_pending_exception()) { _jt->clear_pending_exception(); return nullptr; }
    const constantPoolHandle cp(_jt, ik->constants());
    if (cpi >= cp->length() || !cp->tag_at(cpi).is_method_handle()) return nullptr;
    oop obj = cp->resolve_possibly_cached_constant_at(cpi, _jt);
    if (obj == nullptr) return nullptr;
    skip_ws();
    char* field = parse_token();
    while (field != nullptr && strcmp(field, ";") != 0) {
      if (strcmp(field, "<vmtarget>") == 0) {
        Method* vmtarget = java_lang_invoke_MemberName::vmtarget(obj);
        InstanceKlass* res = (vmtarget != nullptr) ? vmtarget->method_holder() : nullptr;
        if (!parse_terminator()) return nullptr;
        return res;
      }
      obj = ciReplay::obj_field(obj, field);
      if (obj == nullptr) {
        return nullptr;
      }
      if (obj->is_objArray()) {
        bool idx_ok = true;
        int index = parse_int(&idx_ok);
        if (!idx_ok || index >= ((objArrayOop)obj)->length()) return nullptr;
        obj = ((objArrayOop)obj)->obj_at(index);
      }
      field = parse_token();
    }
    if (field == nullptr) {
      if (!parse_terminator()) return nullptr;
    }
    if (obj == nullptr) {
      return nullptr;
    }
    return obj->klass()->is_instance_klass() ? InstanceKlass::cast(obj->klass()) : nullptr;
  }

public:
  HiddenLocatorParser(const char* loc, JavaThread* jt) : _jt(jt) {
    size_t len = strlen(loc);
    _buf = NEW_RESOURCE_ARRAY(char, len + 1);
    strncpy(_buf, loc, len + 1);
    _p = _buf;
  }

  InstanceKlass* parse() {
    skip_ws();
    if (*_p != '@') return nullptr;
    _p++;
    char* kind = parse_token();
    if (kind == nullptr) return nullptr;
    InstanceKlass* ik = nullptr;
    if (strcmp(kind, "bci") == 0) {
      ik = parse_bci();
    } else if (strcmp(kind, "cpi") == 0) {
      ik = parse_cpi();
    }
    if (ik != nullptr && !ik->is_hidden()) {
      return nullptr;
    }
    return ik;
  }
};

class RecordLocation {
  char* _start;
  char* _end;
  char* _buf;
public:
  ATTRIBUTE_PRINTF(3, 4)
  RecordLocation(char* buf, const char* fmt, ...) {
    _buf = buf;
    _start = _buf + (int)strlen(_buf);
    va_list args;
    va_start(args, fmt);
    _end = _start + os::vsnprintf(_start, LOC_BUF_LEN - (_start - _buf), fmt, args);
    va_end(args);
    if (_end >= _buf + LOC_BUF_LEN) {
      _end = _buf + LOC_BUF_LEN - 1;
      *_end = '\0';
    }
  }
  ~RecordLocation() {
    *_start = '\0';
  }
};

static void record_hidden(InstanceKlass* ik, const char* loc) {
  if (ik == nullptr || !ik->is_hidden() || loc == nullptr) {
    return;
  }
  // Mirror ciEnv::dyno_name: recorded locators must end with a terminator.
  bool has_term = false;
  size_t len = strlen(loc);
  for (size_t i = len; i > 0; i--) {
    char ch = loc[i - 1];
    if (!isspace(ch)) {
      has_term = (ch == ';');
      break;
    }
  }
  if (has_term) {
    DynoLocatorTable::record(ik, loc);
    return;
  }
  const size_t needed = len + 3; // " ;" + '\0'
  if (needed > LOC_BUF_LEN) {
    return; // avoid overflow; nothing recorded.
  }
  char term_buf[LOC_BUF_LEN];
  jio_snprintf(term_buf, sizeof(term_buf), "%s ;", loc);
  DynoLocatorTable::record(ik, term_buf);
}

static void record_call_site_obj(JavaThread* jt, oop obj, char* loc_buf);
static void record_mh(JavaThread* jt, oop mh, char* loc_buf);

// Read an object field by name.
static inline oop obj_field(oop obj, const char* name) {
  return ciReplay::obj_field(obj, name);
}

static void record_member(JavaThread* jt, oop member, char* loc_buf) {
  assert(java_lang_invoke_MemberName::is_instance(member), "!");
  oop clazz = java_lang_invoke_MemberName::clazz(member);
  if (clazz != nullptr && clazz->klass()->is_instance_klass()) {
    RecordLocation rl(loc_buf, " clazz");
    InstanceKlass* ik = InstanceKlass::cast(clazz->klass());
    record_hidden(ik, loc_buf);
  }
  Method* vmtarget = java_lang_invoke_MemberName::vmtarget(member);
  if (vmtarget != nullptr) {
    RecordLocation rl(loc_buf, " <vmtarget>");
    InstanceKlass* ik = vmtarget->method_holder();
    record_hidden(ik, loc_buf);
  }
}

static void record_lambdaform(JavaThread* jt, oop form, char* loc_buf) {
  assert(java_lang_invoke_LambdaForm::is_instance(form), "!");

  {
    oop member = java_lang_invoke_LambdaForm::vmentry(form);
    RecordLocation rl(loc_buf, " vmentry");
    record_member(jt, member, loc_buf);
  }

  objArrayOop names = (objArrayOop)obj_field(form, "names");
  if (names != nullptr) {
    RecordLocation lp0(loc_buf, " names");
    int len = names->length();
    for (int i = 0; i < len; ++i) {
      oop name = names->obj_at(i);
      RecordLocation lp1(loc_buf, " %d", i);
      RecordLocation lp2(loc_buf, " function");
      oop function = obj_field(name, "function");
      if (function != nullptr) {
        oop member = obj_field(function, "member");
        if (member != nullptr) {
          RecordLocation lp3(loc_buf, " member");
          record_member(jt, member, loc_buf);
        }
        oop mh = obj_field(function, "resolvedHandle");
        if (mh != nullptr) {
          RecordLocation lp3(loc_buf, " resolvedHandle");
          record_mh(jt, mh, loc_buf); // will recurse
        }
        oop invoker = obj_field(function, "invoker");
        if (invoker != nullptr) {
          RecordLocation lp3(loc_buf, " invoker");
          record_mh(jt, invoker, loc_buf);
        }
      }
    }
  }
}

static void record_mh(JavaThread* jt, oop mh, char* loc_buf) {
  assert(java_lang_invoke_MethodHandle::is_instance(mh), "!");
  // MethodHandle.form
  {
    oop form = java_lang_invoke_MethodHandle::form(mh);
    RecordLocation rl(loc_buf, " form");
    record_lambdaform(jt, form, loc_buf);
  }

  if (java_lang_invoke_DirectMethodHandle::is_instance(mh)) {
    oop member = java_lang_invoke_DirectMethodHandle::member(mh);
    RecordLocation rl(loc_buf, " member");
    record_member(jt, member, loc_buf);
    return;
  }

  // For BoundMethodHandle and friends, scan argL* fields.
  char arg_name[] = " argLXX";
  const int max_arg = 99;
  for (int index = 0; index <= max_arg; ++index) {
    jio_snprintf(arg_name, sizeof(arg_name), " argL%d", index);
    oop arg = obj_field(mh, arg_name + 1);
    if (arg != nullptr) {
      RecordLocation rl(loc_buf, "%s", arg_name);
      if (arg->klass()->is_instance_klass()) {
        InstanceKlass* ik = InstanceKlass::cast(arg->klass());
        record_hidden(ik, loc_buf);
        record_call_site_obj(jt, arg, loc_buf);
      }
    } else {
      break;
    }
  }
}

static void record_call_site_obj(JavaThread* jt, oop obj, char* loc_buf) {
  if (obj == nullptr) {
    return;
  }
  if (java_lang_invoke_MethodHandle::is_instance(obj)) {
    record_mh(jt, obj, loc_buf);
  } else if (java_lang_invoke_ConstantCallSite::is_instance(obj)) {
    oop target = java_lang_invoke_CallSite::target(obj);
    if (target != nullptr && target->klass()->is_instance_klass()) {
      RecordLocation rl(loc_buf, " target");
      InstanceKlass* ik = InstanceKlass::cast(target->klass());
      record_hidden(ik, loc_buf);
    }
  }
}

static void record_call_site_method(JavaThread* jt, Method* adapter, char* loc_buf) {
  if (adapter == nullptr) return;
  InstanceKlass* holder = adapter->method_holder();
  if (!holder->is_hidden()) return;
  RecordLocation rl(loc_buf, " <adapter>");
  record_hidden(holder, loc_buf);
}

static void process_invokedynamic(const constantPoolHandle& cp, int indy_index, JavaThread* jt, char* loc_buf) {
  ResolvedIndyEntry* indy_info = cp->resolved_indy_entry_at(indy_index);
  if (indy_info == nullptr || indy_info->method() == nullptr) {
    return;
  }
  // adapter
  Method* adapter = indy_info->method();
  record_call_site_method(jt, adapter, loc_buf);
  // appendix
  oop appendix = cp->resolved_reference_from_indy(indy_index);
  {
    RecordLocation rl(loc_buf, " <appendix>");
    record_call_site_obj(jt, appendix, loc_buf);
  }
  // BSM
  int pool_index = indy_info->constant_pool_index();
  BootstrapInfo bootstrap_specifier(cp, pool_index, indy_index);
  oop bsm = cp->resolve_possibly_cached_constant_at(bootstrap_specifier.bsm_index(), jt);
  {
    RecordLocation rl(loc_buf, " <bsm>");
    record_call_site_obj(jt, bsm, loc_buf);
  }
}

static void process_invokehandle(const constantPoolHandle& cp, int index, JavaThread* jt, char* loc_buf) {
  const int holder_index = cp->klass_ref_index_at(index, Bytecodes::_invokehandle);
  if (!cp->tag_at(holder_index).is_klass()) {
    return;  // not resolved
  }
  Klass* holder = ConstantPool::klass_at_if_loaded(cp, holder_index);
  Symbol* name = cp->name_ref_at(index, Bytecodes::_invokehandle);
  if (MethodHandles::is_signature_polymorphic_name(holder, name)) {
    ResolvedMethodEntry* method_entry = cp->resolved_method_entry_at(index);
    if (method_entry->is_resolved(Bytecodes::_invokehandle)) {
      Method* adapter = method_entry->method();
      oop appendix = cp->cache()->appendix_if_resolved(method_entry);
      record_call_site_method(jt, adapter, loc_buf);
      {
        RecordLocation rl(loc_buf, " <appendix>");
        record_call_site_obj(jt, appendix, loc_buf);
      }
    }
  }
}

} // anonymous namespace

void DynoLocatorScan::scan_all_classes() {
  ResourceMark rm;
  char loc_buf[LOC_BUF_LEN];
  loc_buf[0] = '\0';

  Thread* thread = Thread::current();
  JavaThread* jt = thread->is_Java_thread() ? JavaThread::cast(thread) : nullptr;
  if (jt == nullptr) {
    // Fallback: use any alive JavaThread as context.
    JavaThreadIteratorWithHandle jtiwh;
    for (JavaThread* t = jtiwh.next(); t != nullptr; t = jtiwh.next()) {
      if (!t->is_terminated()) { jt = t; break; }
    }
  }
  if (jt == nullptr) {
    log_debug(compilation)("dls: no JavaThread available");
    return; // cannot walk without a JavaThread context
  }

  for (ClassHierarchyIterator iter(vmClasses::Object_klass()); !iter.done(); iter.next()) {
    Klass* k = iter.klass();
    if (!k->is_instance_klass()) continue;
    InstanceKlass* ik = InstanceKlass::cast(k);
    if (!ik->is_linked()) continue;
    if (ik->is_hidden()) continue; // only scan non-hidden sources


    const constantPoolHandle cp(jt, ik->constants());
    Array<Method*>* methods = ik->methods();
    for (int mi = 0; mi < methods->length(); mi++) {
      Method* m = methods->at(mi);
      BytecodeStream bcs(methodHandle(jt, m));
      while (!bcs.is_last_bytecode()) {
        Bytecodes::Code opcode = bcs.next();
        opcode = bcs.raw_code();
        if (opcode == Bytecodes::_invokedynamic || opcode == Bytecodes::_invokehandle) {
          RecordLocation rl(loc_buf, "@bci %s %s %s %d",
                           ik->name()->as_quoted_ascii(),
                           m->name()->as_quoted_ascii(),
                           m->signature()->as_quoted_ascii(),
                           bcs.bci());
          if (opcode == Bytecodes::_invokedynamic) {
            int index = bcs.get_index_u4();
            process_invokedynamic(cp, index, jt, loc_buf);
          } else {
            int cp_cache_index = bcs.get_index_u2();
            process_invokehandle(cp, cp_cache_index, jt, loc_buf);
          }
        }
      }
    }

    // Scan constant pool MethodHandle entries (@cpi)
    {
      RecordLocation rp(loc_buf, "@cpi %s", ik->name()->as_quoted_ascii());
      int len = cp->length();
      for (int i = 0; i < len; ++i) {
        if (cp->tag_at(i).is_method_handle()) {
          bool found_it;
          oop mh = cp->find_cached_constant_at(i, found_it, jt);
          if (mh != nullptr) {
            RecordLocation rl(loc_buf, " %d", i);
            record_mh(jt, mh, loc_buf);
          }
        }
      }
    }
  }
}

const char* DynoLocatorScan::lookup(InstanceKlass* ik) {
  return DynoLocatorTable::lookup(ik);
}

InstanceKlass* DynoLocatorScan::resolve_locator(const char* loc, JavaThread* jt) {
  if (loc == nullptr || jt == nullptr) return nullptr;
  ResourceMark rm(jt);
  HiddenLocatorParser p(loc, jt);
  return p.parse();
}
