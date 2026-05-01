#ifndef SHARE_SERVICES_DYNOLOCATORSCAN_HPP
#define SHARE_SERVICES_DYNOLOCATORSCAN_HPP

class InstanceKlass;

class DynoLocatorScan {
public:
  // Walk resolved invokedynamic/invokehandle sites in all linked, non-hidden
  // classes and record locator strings for any hidden classes they target.
  // Intended to be called at a safepoint on the VM thread.
  static void scan_all_classes();

  // Lookup a recorded locator for a hidden class scanned previously.
  static const char* lookup(InstanceKlass* ik);

  // Parse a dyno locator string (ciReplay format) and resolve the hidden class.
  // Returns nullptr if resolution fails.
  static InstanceKlass* resolve_locator(const char* loc, JavaThread* jt);
};

#endif // SHARE_SERVICES_DYNOLOCATORSCAN_HPP
