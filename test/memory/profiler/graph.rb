# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "memory/profiler/graph"

class LeakyObject
	def initialize
		@children = []
	end
	
	def <<(object)
		@children << object
	end
	
	attr_reader :children
end

class CircularObject
	attr_accessor :next, :data
	
	def initialize(data)
		@data = data
		@next = nil
	end
end

describe Memory::Profiler::Graph do
	let(:graph) {subject.new}
	
	with "#add" do
		it "can add an object" do
			root = LeakyObject.new
			100.times do |i|
				hash = Hash.new
				root << hash
				graph.add(hash)
			end
			
			graph.update!(root)
			roots = graph.roots
			
			expect(roots.first).to have_keys(
				name: be(:end_with?, "LeakyObject.@children"),
				count: be == 100,
				percentage: be == 100.0,
			)
		end
	end
	
	with "circular references" do
		it "handles simple circular reference" do
			# Create A -> B -> A cycle
			obj_a = CircularObject.new("A")
			obj_b = CircularObject.new("B")
			obj_a.next = obj_b
			obj_b.next = obj_a
			
			graph.add(obj_a)
			graph.add(obj_b)
			
			# Should complete without infinite loop
			graph.update!(obj_a)
			
			expect(graph.roots.size).to be >= 1
		end
		
		it "handles self-referential object" do
			obj = CircularObject.new("self")
			obj.next = obj  # Points to itself
			
			graph.add(obj)
			graph.update!(obj)
			
			# Should not crash
			expect(graph.roots).to be_a(Array)
		end
		
		it "handles complex circular graph" do
			# Create a diamond with circular references
			root = LeakyObject.new
			
			objs = 10.times.map{|i| CircularObject.new(i)}
			
			# Create circular chain
			objs.each_with_index do |obj, i|
				obj.next = objs[(i + 1) % objs.size]
				root << obj
				graph.add(obj)
			end
			
			graph.update!(root)
			roots = graph.roots
			
			# With circular references and multi-parent tracking,
			# each object in the cycle has multiple parents
			# The dominator should still account for all 10 objects
			total = roots.sum{|r| r[:count]}
			expect(total).to be == 10
		end
	end
	
	with "nested structures" do
		it "handles deeply nested arrays" do
			root = []
			current = root
			
			10.times do
				inner = []
				current << inner
				graph.add(inner)
				current = inner
			end
			
			graph.update!(root)
			roots = graph.roots
			
			# With idom, each array dominates only its immediate child
			# So we get 10 different dominators, each dominating 1 object
			expect(roots.size).to be == 10
			roots.each do |root_info|
				expect(root_info[:count]).to be == 1
			end
		end
		
		it "handles hash with many keys" do
			root = {}
			
			100.times do |i|
				value = {data: i}
				root["key_#{i}"] = value
				graph.add(value)
			end
			
			graph.update!(root)
			roots = graph.roots
			
			expect(roots.first[:count]).to be == 100
		end
		
		it "handles mixed hash and array structures" do
			root = {arrays: [], hashes: {}}
			
			50.times do |i|
				arr = [i]
				root[:arrays] << arr
				graph.add(arr)
			end
			
			50.times do |i|
				h = {value: i}
				root[:hashes]["key_#{i}"] = h
				graph.add(h)
			end
			
			graph.update!(root)
			roots = graph.roots
			
			# With idom, each immediate container gets its own objects
			# The arrays array dominates 50, the hashes hash dominates 50
			expect(roots.size).to be >= 2
			total_dominated = roots.sum{|r| r[:count]}
			expect(total_dominated).to be == 100
		end
	end
	
	with "multiple roots" do
		it "identifies multiple independent retainers" do
			root1 = LeakyObject.new
			root2 = LeakyObject.new
			
			50.times do
				obj = {owner: :root1}
				root1 << obj
				graph.add(obj)
			end
			
			30.times do
				obj = {owner: :root2}
				root2 << obj
				graph.add(obj)
			end
			
			# Update from a common container
			container = {r1: root1, r2: root2}
			graph.update!(container)
			
			roots = graph.roots
			
			# Should have entries for both root's children arrays
			expect(roots.size).to be >= 2
			expect(roots[0][:count] + roots[1][:count]).to be >= 80
		end
	end
	
	with "name generation" do
		it "generates names for constants" do
			module TestModule
				CONSTANT = {data: "test"}
			end
			
			graph.add(TestModule::CONSTANT[:data])
			graph.update!(TestModule)
			
			roots = graph.roots
			expect(roots.first[:name]).to be =~ /CONSTANT/
		end
		
		it "generates names for instance variables" do
			root = LeakyObject.new
			obj = {test: true}
			root << obj
			graph.add(obj)
			
			graph.update!(root)
			roots = graph.roots
			
			expect(roots.first[:name]).to be =~ /@children/
		end
	end
	
	with "#name_for" do
		it "generates simple path for single parent" do
			root = {child: nil}
			child = {data: "test"}
			root[:child] = child
			
			graph.add(child)
			graph.update!(root)
			
			name = graph.name_for(child)
			
			expect(name).to be == "Hash[:child]"
		end
		
		it "generates names for multiple parents with pipe separator" do
			# Create diamond: root1 -> child, root2 -> child
			root1 = {path1: nil}
			root2 = {path2: nil}
			child = {shared: true}
			
			root1[:path1] = child
			root2[:path2] = child
			root = [root1, root2]
			
			graph.add(child)
			graph.update!(root)
			
			name = graph.name_for(child)
			
			# Should contain both paths separated by |
			expect(name).to be == "[Array[0][:path1]|Array[1][:path2]]"
		end
		
		it "generates names for deeply nested structures" do
			# Create: root -> @level1 -> [0] -> @level2 -> [0] -> child
			root = Array.new
			level1 = [Array.new]
			level2 = [{nested: true}]
			child = "deep value"
			
			root << level1
			level1[0] << level2
			level2[0][:child] = child
			
			graph.add(child)
			graph.update!(root)
			
			name = graph.name_for(child)
			
			# Should show the path but might be very long
			expect(name).to be == "Array[0][0][0][0][:child]"
		end
		
		it "handles array index labels" do
			root = []
			items = 3.times.map{{index: _1}}
			items.each{|item| root << item}
			
			graph.add(items[1])  # Add middle item
			graph.update!(root)
			
			name = graph.name_for(items[1])
			puts "Array index name: #{name}"
			
			expect(name).to be =~ /\[1\]/  # Should show index
		end
		
		it "handles hash key labels" do
			root = {users: nil, posts: nil}
			users = {count: 100}
			posts = {count: 50}
			
			root[:users] = users
			root[:posts] = posts
			
			graph.add(users)
			graph.update!(root)
			
			name = graph.name_for(users)
			puts "Hash key name: #{name}"
			
			expect(name).to be =~ /\[:users\]/
		end
		
		it "limits very long paths" do
			# Create extremely deep nesting
			root = []
			current = root
			
			20.times do
				inner = []
				current << inner
				current = inner
			end
			
			graph.add(current)
			graph.update!(root)
			
			name = graph.name_for(current)
			puts "Very deep path (#{name.length} chars): #{name}"
			
			# Name should exist but hopefully not be thousands of characters
			expect(name).to be_a(String)
		end
		
		it "handles objects with multiple parent paths of different lengths" do
			# Create: 
			#   short_path -> child
			#   long_path -> middle1 -> middle2 -> middle3 -> child
			short_path = {direct: nil}
			long_path = {level1: {level2: {level3: {final: nil}}}}
			child = {shared: true}
			
			short_path[:direct] = child
			long_path[:level1][:level2][:level3][:final] = child
			
			root = [short_path, long_path]
			
			graph.add(child)
			graph.update!(root)
			
			name = graph.name_for(child)
			puts name
			
			# Should show both paths with |
			expect(name).to be =~ /\|/
		end
	end
	
	with "edge cases" do
		it "handles empty object set" do
			graph.update!(Object)
			roots = graph.roots
			
			expect(roots).to be == []
		end
		
		it "handles object with no parents" do
			orphan = {orphan: true}
			graph.add(orphan)
			
			graph.update!(orphan)
			roots = graph.roots
			
			# With idom, orphan is the root and dominates itself
			expect(roots.size).to be == 1
			expect(roots.first[:count]).to be == 1
		end
		
		it "handles very large object counts" do
			root = LeakyObject.new
			
			1000.times do |i|
				obj = {index: i}
				root << obj
				graph.add(obj)
			end
			
			graph.update!(root)
			roots = graph.roots
			
			expect(roots.first[:count]).to be == 1000
		end
	end
	
	with "immediate dominators" do
		it "provides precise attribution showing immediate retainers" do
			# Create: root -> @data -> [50 objects]
			root = {data: []}
			
			50.times do |i|
				object = {value: i}
				root[:data] << object
				graph.add(object)
			end
			
			graph.update!(root)
			
			roots = graph.roots
			expect(roots.first[:count]).to be == 50
			expect(roots.first[:name]).to be == "Hash[:data]"
		end
		
		it "handles orphan objects correctly" do
			orphan = {orphan: true}
			graph.add(orphan)
			
			graph.update!(orphan)
			
			# Orphan dominates itself
			roots = graph.roots
			expect(roots.size).to be == 1
			expect(roots.first[:count]).to be == 1
		end
	end
end
