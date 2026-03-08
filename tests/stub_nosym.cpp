/* Stub .so — valid shared library but no gm_* symbols.
 * Used to test loadPlugins "missing ABI symbols" error path. */
extern "C" int stub_dummy(void) { return 0; }
