// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "capture.h"

// Return the memory address of an object as a hex string
// This matches the format used by ObjectSpace.dump_all
static VALUE Memory_Profiler_address_of(VALUE module, VALUE object) {
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "0x%lx", (unsigned long)object);
	return rb_str_new_cstr(buffer);
}

void Init_Memory_Profiler(void)
{
#ifdef HAVE_RB_EXT_RACTOR_SAFE
	rb_ext_ractor_safe(true);
#endif
	
	VALUE Memory = rb_const_get(rb_cObject, rb_intern("Memory"));
	VALUE Memory_Profiler = rb_define_module_under(Memory, "Profiler");
	
	// Add Memory::Profiler.address_of(object) module function:
	rb_define_module_function(Memory_Profiler, "address_of", Memory_Profiler_address_of, 1);
	
	Init_Memory_Profiler_Capture(Memory_Profiler);
}

