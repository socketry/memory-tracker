// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "capture.h"

void Init_Memory_Tracker(void)
{
#ifdef HAVE_RB_EXT_RACTOR_SAFE
    rb_ext_ractor_safe(true);
#endif
    
    VALUE Memory = rb_const_get(rb_cObject, rb_intern("Memory"));
    VALUE Memory_Tracker = rb_define_module_under(Memory, "Tracker");
    
    Init_Memory_Tracker_Capture(Memory_Tracker);
}

