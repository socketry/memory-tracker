# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "memory/profiler/sampler"

describe Memory::Profiler::Sampler do
	let(:sampler) {subject.new(depth: 4, increases_threshold: 2)}
	
	after do
		@sampler&.stop
	end
	
	with "#initialize" do
		it "creates a sampler with default options" do
			sampler = subject.new
			expect(sampler.depth).to be == 4
			expect(sampler.increases_threshold).to be == 10
		end
		
		it "accepts custom options" do
			sampler = subject.new(depth: 6, increases_threshold: 5)
			expect(sampler.depth).to be == 6
			expect(sampler.increases_threshold).to be == 5
		end
	end
	
	with "#start and #stop" do
		it "starts and stops capture" do
			sampler.start
			expect(sampler.capture).not.to be == nil
			
			sampler.stop
		end
	end
	
	with "#sample!" do
		it "samples allocation counts" do
			sampler.start
			
			# Allocate some hashes
			hashes = 10.times.map{{}}
			
			sampled_classes = []
			sampler.sample! do |sample, increased|
				sampled_classes << sample.target
			end
			
			# Should have sampled various classes
			expect(sampled_classes.size).to be > 0
			
			sampler.stop
		end
		
		it "detects increases beyond threshold" do
			sampler.start
			
			# Allocate a small baseline of hashes
			baseline_hashes = 100.times.map{Hash.new}
			
			# First sample establishes baseline (~100 hashes)
			baseline_hash_count = nil
			sampler.sample! do |sample, increased|
				if sample.target == Hash
					baseline_hash_count = sample.current_size
				end
			end
			
			# Allocate many more hashes to trigger increase (threshold is 1000 by default)
			# Delta should be ~1500 which exceeds threshold
			retained_hashes = 1500.times.map{Hash.new}
			
			increased_classes = []
			sampler.sample! do |sample, increased|
				increased_classes << sample.target if increased
			end
			
			# Hash should show as increased
			expect(increased_classes).to be(:include?, Hash)
			
			sampler.stop
			
			# Keep reference so hashes don't get GC'd during test
			retained_hashes.clear
		end
	end
	
	with "#analyze" do
		it "returns nil for untracked class" do
			sampler.start
			result = sampler.analyze(String)
			expect(result).to be == nil
			sampler.stop
		end
		
		it "returns basic allocation statistics" do
			sampler.start
			
			# Allocate some hashes
			hashes = 100.times.map{{}}
			
			# Take sample to record counts
			sampler.sample!
			
			# Force tracking
			sampler.track(Hash)
			
			result = sampler.analyze(Hash, allocation_roots: false, retained_addresses: false)
			
			expect(result).to have_keys(
				allocations: be_a(Hash)
			)
			
			expect(result[:allocations]).to have_keys(
				new_count: be > 0,
				free_count: be_a(Integer),
				retained_count: be > 0
			)
			
			sampler.stop
		end
		
		it "includes allocation_roots when requested" do
			sampler = subject.new(depth: 4, increases_threshold: 0)
			sampler.start
			
			# Force tracking with call tree
			sampler.track(Hash)
			
			# Allocate from known location
			def allocate_hashes
				10.times.map{{}}
			end
			
			hashes = allocate_hashes
			
			result = sampler.analyze(Hash, allocation_roots: true, retained_addresses: false, retained_minimum: 0)
			
			expect(result).to have_keys(
				allocation_roots: be_a(Hash)
			)
			
			# Call tree should show allocate_hashes method
			call_tree = result[:allocation_roots]
			expect(call_tree).to have_keys(
				total_allocations: be > 0
			)
			
			sampler.stop
		end
		
		# it "includes retained_addresses when requested" do
		# 	sampler.start
		# 	sampler.track(String)
		
		# 	# Allocate and retain some strings
		# 	strings = 5.times.map { |i| "test #{i}" }
		
		# 	result = sampler.analyze(String, allocation_roots: false, retained_addresses: true, retained_minimum: 0)
		
		# 	expect(result).to have_keys(
		# 		retained_addresses: be_a(Array)
		# 	)
		
		# 	addresses = result[:retained_addresses]
		# 	expect(addresses.size).to be == 5
		
		# 	# Addresses should be hex strings
		# 	addresses.each do |addr|
		# 		expect(addr).to be =~ /^0x[0-9a-f]+$/
		# 	end
		
		# 	sampler.stop
		# end
		
		it "limits retained_addresses when integer provided" do
			sampler.start
			sampler.track(Hash)
			
			# Allocate many hashes
			hashes = 100.times.map{Hash.new}
			
			result = sampler.analyze(Hash, allocation_roots: false, retained_addresses: 10)
			
			addresses = result[:retained_addresses]
			expect(addresses.size).to be == 10
			
			sampler.stop
		end
		
		it "respects retained_minimum threshold" do
			sampler.start
			sampler.track(String)
			
			# Allocate only a few strings (below default minimum of 100)
			strings = 5.times.map{"test"}
			
			result = sampler.analyze(String, retained_minimum: 100)
			
			# Should return nil because retained count < minimum
			expect(result).to be == nil
			
			sampler.stop
		end
	end
	
	with "#track and #untrack" do
		it "manually tracks a class" do
			sampler.start
			
			expect(sampler.tracking?(String)).to be == false
			
			sampler.track(String)
			
			expect(sampler.tracking?(String)).to be == true
			
			sampler.untrack(String)
			
			expect(sampler.tracking?(String)).to be == false
			
			sampler.stop
		end
	end
	
	with "#clear and #clear_all!" do
		it "clears tracking for a specific class" do
			sampler.start
			sampler.track(Hash)
			
			100.times{Hash.new}
			
			sampler.stop
			sampler.clear(Hash)
			
			# Call tree should be cleared
			tree = sampler.call_tree(Hash)
			expect(tree).to have_attributes(total_allocations: be == 0)
			
			sampler.stop
		end
		
		it "clears all tracking data" do
			sampler.start
			sampler.track(Hash)
			sampler.track(String)
			
			100.times{Hash.new}
			10.times{String.new}
			
			sampler.stop
			sampler.clear_all!
			
			# All call trees should be cleared
			expect(sampler.call_tree(Hash)).to have_attributes(total_allocations: be == 0)
			expect(sampler.call_tree(String)).to have_attributes(total_allocations: be == 0)
			
			sampler.stop
		end
	end
	
	with "#count" do
		it "returns live object count for a class" do
			sampler.start
			sampler.track(Hash)
			
			hashes = 50.times.map{{}}
			
			count = sampler.count(Hash)
			expect(count).to be >= 50
			
			sampler.stop
		end
	end
end

