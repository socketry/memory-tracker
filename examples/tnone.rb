require_relative "../lib/memory/profiler"

# ruby -I ext ./test.rb
# Disable the check for T_NONE in capture.c to see the bug.

GC.stress = true

classes = 2.times.map{Class.new}
classes.each(&:object_id) # Ensure the object_id exists, so freeobj will see it.
objects = 4.times.map{|i| classes[i % classes.size].new}

capture = Memory::Profiler::Capture.new
GC.start
capture.start

objects.clear
classes.clear
GC.start

capture.stop

pp capture.statistics
