#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require_relative "../config/environment"
require_relative "../lib/memory/profiler"

puts "Memory::Profiler Example - Two Modes"
puts "=" * 60

# Mode 1: Count Only (Lightweight Monitoring)
puts "\n=== MODE 1: Count Only (Minimal Overhead) ==="
puts "-" * 60

capture = Memory::Profiler::Capture.new
capture.track(Hash)   # No callback = just counts!
capture.track(Array)
capture.start

puts "Tracking enabled for Hash and Array (count only)"

# Create some objects
leaked = []
100.times do
	leaked << {}  # These hashes are retained
	leaked << []  # These arrays are retained
end

200.times do
	hash = {}  # These will be GC'd
	array = []
end

# Force GC to demonstrate automatic cleanup
GC.start

puts "\nLive object counts (O(1) lookups):"
puts "  Hashes: #{capture.count_for(Hash)}"   # ~100
puts "  Arrays: #{capture.count_for(Array)}"  # ~100

capture.stop
capture.clear

# Mode 2: With Call Path Analysis
puts "\n\n=== MODE 2: With Call Path Analysis ==="
puts "-" * 60

sampler = Memory::Profiler::Sampler.new(depth: 10)
sampler.track(Hash)
sampler.start

puts "Tracking Hash with call path analysis enabled"

# Simulate allocations from different code paths
def allocate_in_method_a
	{ from: :method_a }
end

def allocate_in_method_b
	{ from: :method_b }
end

def deep_allocation
	allocate_in_method_a
	allocate_in_method_b
end

# Generate allocations
retained_hashes = []
50.times {retained_hashes << allocate_in_method_a}
30.times {retained_hashes << allocate_in_method_b}
20.times {retained_hashes << deep_allocation}

# Analyze
statistics = sampler.statistics(Hash)

if statistics
	puts "\nHash Allocation Analysis:"
	puts "  Live objects: #{statistics[:live_count]}"
	puts "  Total allocations: #{statistics[:total_allocations]}"
	puts "  Retained allocations: #{statistics[:retained_allocations]}"
	
	puts "\n  Top Call Paths (by retained count):"
	statistics[:top_paths].first(5).each_with_index do |path_data, i|
		total = path_data[:total_count]
		retained = path_data[:retained_count]
		puts "\n  #{i + 1}. Total: #{total}, Retained: #{retained} (#{(retained * 100.0 / total).round(1)}% kept)"
		path_data[:path].first(3).each {|frame| puts "       #{frame}"}
	end
	
	puts "\n  Hotspot Frames (by retained count):"
	statistics[:hotspots].first(5).each do |location, counts|
		total = counts[:total_count]
		retained = counts[:retained_count]
		puts "    #{location}"
		puts "      Total: #{total}, Retained: #{retained} (#{(retained * 100.0 / total).round(1)}% kept)"
	end
end

sampler.stop!

puts "\n" + "=" * 60
puts "Example complete!"
puts "\nKey Takeaways:"
puts "  - Mode 1 (count only): Minimal overhead, great for monitoring"
puts "  - Mode 2 (with analysis): Captures call paths, use when diagnosing"
puts "  - Both modes use NEWOBJ/FREEOBJ for automatic cleanup"
puts "  - No ObjectSpace.each_object needed!"
