#ifndef SHARE_SERVICES_PROFILECHECKPOINT_HPP
#define SHARE_SERVICES_PROFILECHECKPOINT_HPP

#include "utilities/globalDefinitions.hpp"
#include <stdio.h>
#include "utilities/growableArray.hpp"

class fileStream;

class ProfileCheckpoint {
public:
    // Binary format (File = [HEADER][SYMTAB][CLASSES][RECORDS])
    //
    // HEADER:
    //   magic[4] = 'M','D','O','X'
    //   pointer_size: u16 (e.g., 8)
    //   endianness  : u16 (0=little, 1=big)
    //   layout flags: see LayoutFlags
    //   sym_count   : u32
    //   rec_count   : u32
    //   class_count : u32
    //
    // SYMTAB: repeated sym_count times
    //   [u32 len][len bytes utf8]
    //
    // CLASSES: repeated class_count times:
    //   [u1 loader_id][u4 loader_name_sym_id][u4 klass_sym_id]
    //   (loader_name_sym_id is 0xFFFFFFFF if loader != NAMED)
    // 
    // RECORD: repeated rec_count times
    //   klass_id:u32, name_id:u32, sig_id:u32, loader:u8, loader_name_id:u32, comp_level:u8,
    //   mdo_size:u32, fixup_count:u32, Fixup[fixup_count], [mdo_size bytes],
    //   header_size:u32, [header bytes], mc_size:u32, [mc bytes]
    //
    // Fixup: repeated fixup_count times
    //   [u4 offset_in_mdo][u4 target_sym_id][u1 loader_id][u4 loader_name_sym_id]

  // LoaderId values: BOOT=0, PLATFORM=1, SYSTEM=2, UNDEFINED=3, HIDDEN=4, NAMED=5
  // NAMED is used for user-defined loaders with a stable name (ClassLoader.getName())
  enum class LoaderId : u1 { BOOT, PLATFORM, SYSTEM, UNDEFINED, HIDDEN, NAMED };

  struct SymbolId { uint32_t id; };

  struct Fixup {
    uint32_t offset_in_mdo;
    SymbolId  target;
    LoaderId  loader;
    SymbolId  loader_name;  // valid only if loader == NAMED; 0xFFFFFFFF otherwise
  };

  struct ByteRange { uint64_t off; uint32_t size; };

  struct LayoutFlags {
    uint32_t type_profile_level;
    int32_t  type_profile_args_limit;
    int32_t  type_profile_parms_limit;
    int64_t  type_profile_width;
    uint8_t  profile_traps;
    uint8_t  type_profile_casts;
    int32_t  spec_trap_limit_extra_entries;
  };

  struct MethodKey {
    LoaderId  loader;
    SymbolId  loader_name;  // valid only if loader == NAMED; 0xFFFFFFFF otherwise
    SymbolId  klass; // "a/b/C"
    SymbolId  name;  // "foo"
    SymbolId  sig;   // "(I)Ljava/lang/String;"
    uint32_t  bytecode_crc32;
  };

  struct Descriptor {
    MethodKey key;
    ByteRange mdo_payload {0,0};
    ByteRange mdo_fixups  {0,0};
    ByteRange mc_payload  {0,0};
    ByteRange mc_fixups   {0,0};
  };

  // Minimal metadata per-record used during dump before IDs are frozen
  struct RecMeta {
    const char* kname;
    const char* mname;
    const char* sig;
    u4          mdo_size;
    const void* mdo_ptr;
    u1          comp_level;
  };

  struct Header {
    char magic[4];
    u2   pointer_size;
    u2   endianness;
    LayoutFlags layout;
    u4   sym_count;
    u4   rec_count;
    u4   class_count;
  };

  struct Class {
    LoaderId loader;
    SymbolId loader_name;  // valid only if loader == NAMED; 0xFFFFFFFF otherwise
    SymbolId klass;
  };

  struct Record {
    MethodKey key;
    u4        mdo_size;
    u4        fixup_count;
    u4        header_size;
    u4        mc_size; // bytes of MethodCounters snapshot (may be 0)
    u1        comp_level;
  };

  class SymtabBuilder {
  private:
    GrowableArray<const char*> _syms;
    bool                        _frozen;
  public:
    SymtabBuilder() : _syms(256), _frozen(false) {}
    u4 intern(const char* s);
    u4 id_of(const char* s) const;
    void freeze() { _frozen = true; }
    u4 length() const { return (u4)_syms.length(); }
    const GrowableArray<const char*>& symbols() const { return _syms; }
  };

  class Loader {
  public:
    enum class LoadStatus {
      Success,
      MissingPath,
      FileOpenFailed,
      HeaderInvalid,
      HeaderMismatch,
      SymtabReadFailed,
      RecordReadFailed
    };

    struct LoadResult {
      LoadStatus status;
      int records_read;
      int records_installed;
      int size_mismatch;
      bool ok() const { return status == LoadStatus::Success; }
    };

    explicit Loader(class JavaThread* thread);
    LoadResult load_from_file(const char* path);
    static const char* load_status_name(LoadStatus status);
  private:
    class JavaThread* _thread;
    int _records_read;
    int _records_installed;
    int _size_mismatch;

    bool install_record(const Record& rec,
                        Fixup* fixups,
                        char* mdo_bytes,
                        char* mc_bytes,
                        char* header_bytes,
                        GrowableArray<char*>& symtab);
  };

  // Deferred MDO install: records whose holder class couldn't be resolved
  // at load time (typically named-loader classes not yet loaded).
  struct PendingRecord {
    Record    rec;
    Fixup*    fixups;       // owned, may be nullptr
    char*     mdo_bytes;    // owned
    char*     mc_bytes;     // owned, may be nullptr
    char*     header_bytes; // owned, may be nullptr
    char*     kname;        // os::malloc'd copy of class name
    char*     mname;        // os::malloc'd copy of method name
    char*     msig;         // os::malloc'd copy of method signature
    char*     loader_name;  // os::malloc'd copy of loader name, may be nullptr
  };

  static void load(class JavaThread* THREAD);
  static void dump_to_stream(class fileStream* out);
  static void wait_for_compile_completion(class JavaThread* THREAD);
  static void scan_hidden_class_locators();

  // Called from InstanceKlass::initialize_impl() to install deferred MDO records
  // for classes whose classloader wasn't available at checkpoint load time.
  static void try_install_pending(class InstanceKlass* k, class JavaThread* thread);
  static bool has_pending_records();
};

#endif // SHARE_SERVICES_PROFILECHECKPOINT_HPP
