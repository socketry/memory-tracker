#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "mkmf"

extension_name = "Memory_Tracker"

append_cflags(["-Wall", "-Wno-unknown-pragmas", "-std=c99"])

if ENV.key?("RUBY_DEBUG")
	$stderr.puts "Enabling debug mode..."
	
	append_cflags(["-DRUBY_DEBUG", "-O0"])
end

$srcs = ["memory/tracker/tracker.c", "memory/tracker/capture.c"]
$VPATH << "$(srcdir)/memory/tracker"

# Check for required headers
have_header("ruby/debug.h") or abort "ruby/debug.h is required"
have_func("rb_ext_ractor_safe")

if ENV.key?("RUBY_SANITIZE")
	$stderr.puts "Enabling sanitizers..."
	
	# Add address and undefined behaviour sanitizers:
	$CFLAGS << " -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer"
	$LDFLAGS << " -fsanitize=address -fsanitize=undefined"
end

create_header

# Generate the makefile to compile the native binary into `lib`:
create_makefile(extension_name)
