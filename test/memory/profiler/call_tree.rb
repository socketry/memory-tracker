# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "memory/profiler/call_tree"

Location = Struct.new(:path, :lineno, :label) do
	def to_s
		"#{path}:#{lineno}#{label ? " in '#{label}'" : ""}"
	end
end

describe Memory::Profiler::CallTree do
	let(:tree) {subject.new}
	
	with "#record" do
		it "can record a single allocation" do
			locations = [
				Location.new("a.rb", 1, "foo"),
				Location.new("b.rb", 2, "bar")
			]
			
			tree.record(locations)
			
			expect(tree.total_allocations).to be == 1
		end
		
		it "builds tree structure for common paths" do
			locations1 = [
				Location.new("a.rb", 1, "foo"),
				Location.new("b.rb", 2, "bar")
			]
			
			locations2 = [
				Location.new("a.rb", 1, "foo"),
				Location.new("c.rb", 3, "baz")
			]
			
			tree.record(locations1)
			tree.record(locations2)
			
			expect(tree.total_allocations).to be == 2
			
			paths = tree.top_paths(10)
			expect(paths.length).to be == 2
		end
		
		it "deduplicates common call prefixes" do
			a_rb = Location.new("a.rb", 1, "foo")
			b_rb = Location.new("b.rb", 2, "bar")
			c_rb = Location.new("c.rb", 3, "baz")
			
			# Same root, different branches
			10.times do
				tree.record([
					a_rb, b_rb
				])
			end
			
			5.times do
				tree.record([
					a_rb, c_rb
				])
			end
			
			expect(tree.total_allocations).to be == 15
			
			# Should have deduped the common root "a.rb:1"
			hotspots = tree.hotspots(10)
			total, retained = hotspots[a_rb.to_s]
			expect(total).to be == 15  # Both paths share this
			expect(retained).to be == 15  # All retained (no frees)
		end
	end
	
	with "#top_paths" do
		it "returns paths sorted by count" do
			5.times do
				tree.record([Location.new("a.rb", 1, "foo")])
			end
			
			10.times do
				tree.record([Location.new("b.rb", 2, "bar")])
			end
			
			paths = tree.top_paths(10)
			
			# top_paths now returns [locations, total_count, retained_count]
			expect(paths.first[1]).to be == 10  # Top path has 10 total allocations
			expect(paths.first[2]).to be == 10  # Top path has 10 retained
			expect(paths.last[1]).to be == 5    # Second path has 5 total
			expect(paths.last[2]).to be == 5    # Second path has 5 retained
		end
	end
	
	with "#hotspots" do
		it "counts individual frames correctly" do
			a_rb = Location.new("a.rb", 1, "foo")
			b_rb = Location.new("b.rb", 2, "bar")
			
			3.times do
				tree.record([a_rb])
			end
			
			2.times do
				tree.record([b_rb])
			end
			
			hotspots = tree.hotspots(10)
			total_a, retained_a = hotspots[a_rb.to_s]
			total_b, retained_b = hotspots[b_rb.to_s]
			
			expect(total_a).to be == 3
			expect(retained_a).to be == 3
			expect(total_b).to be == 2
			expect(retained_b).to be == 2
		end
	end
	
	with "#clear!" do
		it "clears all data" do
			tree.record([Location.new("a.rb", 1, "foo")])
			expect(tree.total_allocations).to be > 0
			
			tree.clear!
			expect(tree.total_allocations).to be == 0
		end
	end
	
	with "dual counters" do
		it "tracks both total and retained allocations" do
			location = Location.new("a.rb", 1, "foo")
			
			# Record 5 allocations
			nodes = 5.times.map do
				tree.record([location])
			end
			
			expect(tree.total_allocations).to be == 5
			expect(tree.retained_allocations).to be == 5
			
			# Simulate 2 objects being freed
			2.times do |i|
				nodes[i].decrement_path!
			end
			
			# Total should stay the same, retained should decrease
			expect(tree.total_allocations).to be == 5
			expect(tree.retained_allocations).to be == 3
		end
		
		it "decrements through entire path" do
			location1 = Location.new("a.rb", 1, "foo")
			location2 = Location.new("b.rb", 2, "bar")
			
			# Create nested path
			node = tree.record([location1, location2])
			
			expect(tree.total_allocations).to be == 1
			expect(tree.retained_allocations).to be == 1
			
			# Decrement - should affect entire path
			node.decrement_path!
			
			expect(tree.total_allocations).to be == 1
			expect(tree.retained_allocations).to be == 0
			
			# Verify hotspots show the decrement
			hotspots = tree.hotspots(10)
			location1_total, location1_retained = hotspots[location1.to_s]
			location2_total, location2_retained = hotspots[location2.to_s]
			
			expect(location1_total).to be == 1
			expect(location1_retained).to be == 0
			expect(location2_total).to be == 1
			expect(location2_retained).to be == 0
		end
	end
	
	with "#prune!" do
		it "keeps top N children by retained count" do
			# Create paths with different retention
			10.times {tree.record([Location.new("high.rb", 1, "leak")])}
			5.times {tree.record([Location.new("medium.rb", 2, "moderate")])}
			2.times {tree.record([Location.new("low.rb", 3, "small")])}
			
			expect(tree.total_allocations).to be == 17
			expect(tree.retained_allocations).to be == 17
			
			# Prune to top 2 - should remove 1 node (low.rb)
			pruned = tree.prune!(2)
			expect(pruned).to be == 1
			
			# Total/retained counts should still reflect original data
			expect(tree.total_allocations).to be == 17
			expect(tree.retained_allocations).to be == 17
			
			# But only 2 paths should remain (top by retained count)
			paths = tree.top_paths(10)
			expect(paths.size).to be == 2
			expect(paths[0][2]).to be == 10  # high.rb
			expect(paths[1][2]).to be == 5   # medium.rb
		end
		
		it "prunes nested levels independently" do
			# Create a tree with branching at different levels
			# Root -> A -> A1 (10 retained)
			#      -> A -> A2 (5 retained)
			#      -> B -> B1 (3 retained)
			
			a = Location.new("a.rb", 1, "a")
			b = Location.new("b.rb", 2, "b")
			a1 = Location.new("a1.rb", 10, "a1")
			a2 = Location.new("a2.rb", 20, "a2")
			b1 = Location.new("b1.rb", 30, "b1")
			
			10.times {tree.record([a, a1])}
			5.times {tree.record([a, a2])}
			3.times {tree.record([b, b1])}
			
			expect(tree.total_allocations).to be == 18
			
			# Prune to top 1 at each level
			tree.prune!(1)
			
			# Should keep only path a -> a1 (highest retained)
			# a has 15 total (10+5), b has 3, so a wins
			# a1 has 10, a2 has 5, so a1 wins
			paths = tree.top_paths(10)
			expect(paths.size).to be == 1
			expect(paths[0][0]).to be == [a.to_s, a1.to_s]
			expect(paths[0][2]).to be == 10  # retained count
		end
		
		it "handles empty tree" do
			tree.prune!(5)
			expect(tree.total_allocations).to be == 0
		end
		
		it "does nothing when children <= limit" do
			3.times {tree.record([Location.new("a.rb", 1, "foo")])}
			2.times {tree.record([Location.new("b.rb", 2, "bar")])}
			
			# Prune with limit >= children count
			tree.prune!(5)
			
			paths = tree.top_paths(10)
			expect(paths.size).to be == 2  # Both kept
		end
		
		it "returns pruned node count" do
			tree.record([Location.new("a.rb", 1, "foo")])
			tree.record([Location.new("b.rb", 2, "bar")])
			tree.record([Location.new("c.rb", 3, "baz")])
			
			# Prune to 1 - should remove 2 nodes
			pruned = tree.prune!(1)
			expect(pruned).to be == 2
		end
		
		it "tracks insertion count" do
			5.times {tree.record([Location.new("a.rb", 1, "foo")])}
			expect(tree.insertion_count).to be == 5
			
			# Pruning doesn't reset counter (manual control)
			tree.prune!(5)
			expect(tree.insertion_count).to be == 5
			
			# Can manually reset
			tree.insertion_count = 0
			expect(tree.insertion_count).to be == 0
			
			3.times {tree.record([Location.new("b.rb", 2, "bar")])}
			expect(tree.insertion_count).to be == 3
		end
		
		it "detaches pruned nodes to aid GC" do
			# Create nodes that will be pruned
			high_node = tree.record([Location.new("high.rb", 1, "keep")])
			10.times {tree.record([Location.new("high.rb", 1, "keep")])}
			
			low_node = tree.record([Location.new("low.rb", 2, "discard")])
			2.times {tree.record([Location.new("low.rb", 2, "discard")])}
			
			# Before pruning, low_node has parent/location references
			expect(low_node.parent).not.to be_nil
			expect(low_node.location).not.to be_nil
			
			# Prune to top 1 - should discard low.rb
			tree.prune!(1)
			
			# After pruning, low_node should be detached (references broken)
			expect(low_node.parent).to be_nil
			expect(low_node.location).to be_nil
			expect(low_node.children).to be_nil
		end
	end
end


