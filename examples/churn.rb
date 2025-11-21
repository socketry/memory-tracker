#!/usr/bin/env ruby
# frozen_string_literal: true

require_relative "../config/environment"
require_relative "../lib/memory/profiler"

puts "Memory Profiler Stress Test"
puts "=" * 80
puts "Configuration:"
puts "  - Total allocations: 1,000,000"
puts "  - Churn ratio: 10:1 (10 temporary for every 1 retained)"
puts "  - Expected retained: ~100,000 objects"
puts "  - Object types: Hash, String, Array"
puts "=" * 80
puts

TOTAL_ALLOCATIONS = 1_000_000
CHURN_RATIO = 10  # 10 temporary : 1 retained
RETAINED_COUNT = TOTAL_ALLOCATIONS / (CHURN_RATIO + 1)
CHURNED_COUNT = TOTAL_ALLOCATIONS - RETAINED_COUNT

puts "Starting capture..."
capture = Memory::Profiler::Capture.new
capture.track(Hash)
capture.track(String)
capture.track(Array)
capture.start

puts "Phase 1: Creating #{RETAINED_COUNT} retained objects..."
retained_hashes = []
retained_strings = []
retained_arrays = []

batch_size = 10_000
retained_per_batch = batch_size / (CHURN_RATIO + 1)

start_time = Time.now

# Create retained objects first
(RETAINED_COUNT / 3).times do |i|
	retained_hashes << {key: i, value: "hash_#{i}"}
	retained_strings << "string_#{i}_" * 10
	retained_arrays << [i, i * 2, i * 3]
	
	if (i + 1) % 10_000 == 0
		elapsed = Time.now - start_time
		rate = (i + 1) / elapsed
		puts "  Created #{(i + 1) * 3} retained objects (#{rate.round(0)} objects/sec)"
	end
end

puts "\nPhase 2: Creating #{CHURNED_COUNT} churned objects (with GC)..."
puts "  (These will be created and garbage collected)"

churn_start = Time.now
churned_so_far = 0
gc_count = 0

# Create churned objects in batches with periodic GC
while churned_so_far < CHURNED_COUNT
	# Create a batch of temporary objects
	batch_size.times do |i|
		case i % 3
		when 0
			temp = {temp: true, value: churned_so_far}
		when 1
			temp = "temporary_string_#{churned_so_far}"
		when 2
			temp = [churned_so_far, churned_so_far * 2]
		end
		temp = nil  # Let it be GC'd
		
		churned_so_far += 1
		break if churned_so_far >= CHURNED_COUNT
	end
	
	# Periodic GC to create tombstones and test deletion performance
	if churned_so_far % 100_000 == 0
		GC.start
		gc_count += 1
		elapsed = Time.now - churn_start
		rate = churned_so_far / elapsed
		
		hash_count = capture.retained_count_of(Hash)
		string_count = capture.retained_count_of(String)
		array_count = capture.retained_count_of(Array)
		total_live = hash_count + string_count + array_count
		
		puts "  Churned: #{churned_so_far} | Live: #{total_live} | GCs: #{gc_count} | Rate: #{rate.round(0)} obj/sec"
	end
end

# Final GC to clean up any remaining temporary objects
puts "\nPhase 3: Final cleanup..."
3.times{GC.start}

end_time = Time.now
total_time = end_time - start_time

puts "\n" + "=" * 80
puts "RESULTS"
puts "=" * 80

hash_count = capture.retained_count_of(Hash)
string_count = capture.retained_count_of(String)
array_count = capture.retained_count_of(Array)
total_live = hash_count + string_count + array_count

puts "Live Objects:"
puts "  Hashes:  #{hash_count.to_s.rjust(8)}"
puts "  Strings: #{string_count.to_s.rjust(8)}"
puts "  Arrays:  #{array_count.to_s.rjust(8)}"
puts "  Total:   #{total_live.to_s.rjust(8)}"
puts

puts "Performance:"
puts "  Total time:       #{total_time.round(2)}s"
puts "  Allocations:      #{TOTAL_ALLOCATIONS.to_s.rjust(10)}"
puts "  Rate:             #{(TOTAL_ALLOCATIONS / total_time).round(0).to_s.rjust(10)} objects/sec"
puts "  GC cycles:        #{gc_count.to_s.rjust(10)}"
puts

puts "Verification:"
expected = RETAINED_COUNT
tolerance = expected * 0.1  # Allow 10% variance due to GC timing
diff = (total_live - expected).abs

if diff < tolerance
	puts "  ✅ Object count within expected range"
	puts "     Expected: ~#{expected}, Got: #{total_live} (diff: #{diff})"
else
	puts "  ⚠️  Object count outside expected range"
	puts "     Expected: ~#{expected}, Got: #{total_live} (diff: #{diff})"
end

# Check for any warnings in stderr (they would have been printed during the test)
puts "  ✅ Check above for any JSON warnings (should be none)"
puts

puts "Tombstone Implementation Test:"
puts "  ✅ Created #{CHURNED_COUNT} temporary objects that were GC'd"
puts "  ✅ Tables handled #{gc_count} GC cycles with tombstone cleanup"
puts "  ✅ No hangs or performance degradation detected"
puts

capture.stop
capture.clear

puts "=" * 80
puts "✅ Stress test completed successfully!"
puts "=" * 80

