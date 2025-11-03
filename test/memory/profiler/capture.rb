# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "memory/profiler/capture"

describe Memory::Profiler::Capture do
	let(:capture) {subject.new}
	
	with "#start" do
		it "can start capturing" do
			result = capture.start
			expect(result).to be == true
		end
		
		it "returns false if already started" do
			capture.start
			result = capture.start
			expect(result).to be == false
		end
	end
	
	with "#stop" do
		it "can stop capturing" do
			capture.start
			result = capture.stop
			expect(result).to be == true
		end
		
		it "returns false if not started" do
			result = capture.stop
			expect(result).to be == false
		end
	end
	
	with "#track" do
		it "can track a class" do
			capture.track(Hash)
			expect(capture.tracking?(Hash)).to be == true
		end
		
		it "returns false for untracked classes" do
			expect(capture.tracking?(Array)).to be == false
		end
	end
	
	with "#untrack" do
		it "can stop tracking a class" do
			capture.track(Hash)
			capture.untrack(Hash)
			expect(capture.tracking?(Hash)).to be == false
		end
	end
	
	with "#count_for" do
		it "tracks allocations for a class" do
			capture.start
			
			initial_count = capture.count_for(Hash)
			
			objects = 10.times.map do
				Hash.new
			end
			
			capture.stop
			
			new_count = capture.count_for(Hash)
			expect(new_count).to be >= initial_count + objects.size
		end
		
		it "decrements count when objects are freed" do
			GC.start
			
			capture.start
			
			initial_count = capture.count_for(Hash)
			
			# Allocate and retain
			retained = []
			5.times {retained << {}}
			
			# Allocate and don't retain
			10.times {{}}
			
			# Force GC
			GC.start
			
			# Check count after GC (should only have retained + initial)
			final_count = capture.count_for(Hash)
			
			# Should be close to initial + 5 (some variation due to GC internals)
			expect(final_count).to be >= initial_count
			expect(final_count).to be <= initial_count + 10
			
			capture.stop
		end
		
		it "returns 0 for untracked classes" do
			expect(capture.count_for(String)).to be == 0
		end
	end
	
	with "callback" do
		it "calls callback on allocation" do
			captured_classes = []
			
			capture.track(Hash) do |klass, event, state|
				captured_classes << klass if event == :newobj
			end
			
			capture.start
			
			hash = {}
			
			capture.stop
			
			# Should have captured the hash
			expect(captured_classes.length).to be >= 1
			
			# Find our hash in the captured classes
			found = captured_classes.any? {|klass| klass == Hash}
			expect(found).to be == true
		end
		
		it "does not call callback when not started" do
			call_count = 0
			
			capture.track(Hash) do |klass, event, state|
				call_count += 1
			end
			
			# Don't start!
			hash = {}
			
			expect(call_count).to be == 0
		end
		
		it "callback can capture caller_locations" do
			captured_locations = []
			
			capture.track(Hash) do |klass, event, state|
				if event == :newobj
					locations = caller_locations(1, 3)
					captured_locations << locations
				end
			end
			
			capture.start
			
			hash = {}
			
			capture.stop
			
			# Should have captured locations
			expect(captured_locations.length).to be >= 1
			expect(captured_locations.first).to be_a(Array)
		end
	end
	
	with "#clear" do
		it "resets all counts to zero" do
			capture.track(Hash)
			capture.start
			
			10.times {{}}
			
			expect(capture.count_for(Hash)).to be > 0
			
			capture.clear
			
			expect(capture.count_for(Hash)).to be == 0
			
			capture.stop
		end
	end
	
	with "multiple classes" do
		it "can track multiple classes independently" do
			capture.track(Hash)
			capture.track(Array)
			capture.start
			
			5.times {{}}
			3.times {[]}
			
			hash_count = capture.count_for(Hash)
			array_count = capture.count_for(Array)
			
			expect(hash_count).to be >= 5
			expect(array_count).to be >= 3
			
			capture.stop
		end
	end
	
	with "GC stress test" do
		it "handles GC during tracking with callbacks that store state" do
			# This test attempts to recreate the T_NONE marking bug
			# by storing state in callbacks and forcing GC
			
			# Pre-allocate state objects to avoid allocation during callback
			state_objects = 200.times.map {{index: rand}}
			state_index = 0
			
			capture.track(Hash) do |klass, event, state|
				if event == :newobj
					# Return pre-allocated state
					result = state_objects[state_index]
					state_index = (state_index + 1) % state_objects.size
					result
				elsif event == :freeobj
					# Just return state, don't allocate
					state
				end
			end
			
			capture.start
			
			# Allocate many objects without retaining them
			100.times do
				{}
			end
			
			# Mix old generation objects by running GC multiple times
			3.times {GC.start}
			
			# Should not crash with "try to mark T_NONE object"
			expect(capture.count_for(Hash)).to be >= 0
			
			capture.stop
		end
		
		it "handles tracking anonymous classes that get collected" do
			# Another way to trigger T_NONE: track a class that gets GC'd
			anonymous_class = Class.new
			
			capture.track(anonymous_class)
			capture.start
			
			# Allocate some instances
			3.times {anonymous_class.new}
			
			# Remove reference to the class
			anonymous_class = nil
			
			# Force GC multiple times to mix old generation objects
			# and increase chance the class gets collected
			3.times {GC.start}
			
			# Allocate some other objects to trigger more marking
			1000.times {[]}
			
			# Force more GC to trigger marking of tracked_classes
			3.times {GC.start}
			
			# Should not crash during GC marking
			capture.stop
		end
		
		it "recreates T_NONE marking failure during array join" do
			# Recreate the specific failure from the stack trace:
			# The bug occurs during rb_ary_join which allocates strings,
			# triggering GC, which tries to mark classes that became T_NONE
			
			capture.track(String)
			capture.start
			
			# Allocate many strings
			1000.times {"test"}
			
			# Mix old generation objects
			3.times {GC.start}
			
			# Now do array join which allocates strings and can trigger GC
			# This matches the stack trace: /array.c:2915 rb_ary_join
			large_array = 1000.times.map {"item_#{rand}"}
			result = large_array.join(",")
			
			# Mix old generation again
			3.times {GC.start}
			
			# Should not crash with "try to mark T_NONE object"
			expect(result).not.to be == nil
			
			capture.stop
		end
		
		it "handles GC compaction during tracking" do
			capture.track(String) do |klass, event, state|
				if event == :newobj
					# Store a string as state
					"allocated"
				end
			end
			
			capture.start
			
			# Allocate many strings
			strings = 1000.times.map {"test#{rand}"}
			
			# Should not crash
			expect(capture.count_for(String)).to be >= 1000
			
			capture.stop
			
			# Trigger GC compaction if available
			begin
				GC.verify_compaction_references(expand_heap: true, toward: :empty)
			rescue NotImplementedError
				# Compaction not available on this Ruby version
			end
		end
	end
end
