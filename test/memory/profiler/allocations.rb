# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "memory/profiler"

describe Memory::Profiler::Allocations do
	let(:allocations) {Memory::Profiler::Allocations.new}
	
	with "#as_json" do
		it "returns hash with allocation statistics" do
			json = allocations.as_json
			
			expect(json).to be_a(Hash)
			expect(json).to have_keys(
				new_count: be == 0,
				free_count: be == 0,
				retained_count: be == 0,
			)
		end
		
		it "reflects current counts" do
			# We can't easily manipulate the C struct from Ruby,
			# but we can verify the method exists and returns proper structure
			json = allocations.as_json
			
			expect(json.keys).to be == [:new_count, :free_count, :retained_count]
		end
	end
	
	with "#to_json" do
		it "converts to JSON string" do
			require "json"
			
			json_string = allocations.to_json
			
			expect(json_string).to be_a(String)
			
			# Parse back to verify it's valid JSON
			parsed = JSON.parse(json_string)
			expect(parsed).to have_keys(
				"new_count" => be == 0,
				"free_count" => be == 0,
				"retained_count" => be == 0,
			)
		end
	end
	
	with "integration with Capture" do
		it "returns correct JSON from captured allocations" do
			capture = Memory::Profiler::Capture.new
			capture.start
			
			# Create some objects
			10.times{Hash.new}
			
			# Get allocations object
			allocations = capture[Hash]
			
			expect(allocations).to be_a(Memory::Profiler::Allocations)
			
			json = allocations.as_json
			
			expect(json).to have_keys(
				new_count: be >= 10,
				retained_count: be >= 10,
				free_count: be >= 0,
			)
			
			capture.stop
		end
	end
end

