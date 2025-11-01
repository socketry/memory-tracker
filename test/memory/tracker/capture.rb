# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "memory/tracker/capture"

describe Memory::Tracker::Capture do
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
			captured_objects = []
			
			capture.track(Hash) do |klass|
				captured_objects << klass
			end
			
			capture.start
			
			hash = {}
			
			capture.stop
			
			# Should have captured the hash
			expect(captured_objects.length).to be >= 1
			
			# Find our hash in the captured objects
			found = captured_objects.any? {|klass| klass == Hash}
			expect(found).to be == true
		end
		
		it "does not call callback when not started" do
			call_count = 0
			
			capture.track(Hash) do |obj, klass|
				call_count += 1
			end
			
			# Don't start!
			hash = {}
			
			expect(call_count).to be == 0
		end
		
		it "callback can capture caller_locations" do
			captured_locations = []
			
			capture.track(Hash) do |obj, klass|
				locations = caller_locations(1, 3)
				captured_locations << locations
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
end

