# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "memory/tracker/call_tree"

Location = Struct.new(:path, :lineno, :label) do
	def to_s
		"#{path}:#{lineno}#{label ? " in '#{label}'" : ""}"
	end
end

describe Memory::Tracker::CallTree do
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
			expect(hotspots[a_rb.to_s]).to be == 15  # Both paths share this
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
			
			expect(paths.first[1]).to be == 10  # Top path has 10 allocations
			expect(paths.last[1]).to be == 5     # Second path has 5
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
			expect(hotspots[a_rb.to_s]).to be == 3
			expect(hotspots[b_rb.to_s]).to be == 2
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
end


