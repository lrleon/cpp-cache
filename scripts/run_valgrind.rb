#!/usr/bin/env ruby
# frozen_string_literal: true

#
# run_valgrind.rb -- Run cache_valgrind_test under Helgrind, DRD, and Memcheck
#
# Usage:
#   scripts/run_valgrind.rb                  # all three tools
#   scripts/run_valgrind.rb helgrind         # only Helgrind
#   scripts/run_valgrind.rb drd              # only DRD
#   scripts/run_valgrind.rb memcheck         # only Memcheck
#   scripts/run_valgrind.rb helgrind drd     # specific tools
#
# The script builds the target first, then runs each tool sequentially.
# Logs are written to build-v2/valgrind-<tool>.log
#

require 'fileutils'
require 'open3'

ROOT     = File.expand_path('..', __dir__)
BUILD    = File.join(ROOT, 'build-v2')
BINARY   = File.join(BUILD, 'cache_valgrind_test')
LOG_DIR  = BUILD
SUPP     = File.join(ROOT, 'scripts', 'valgrind-shared_ptr.supp')

TOOLS = {
  'helgrind' => %w[
    --tool=helgrind
    --history-level=full
    --conflict-cache-size=2000000
  ],
  'drd' => %w[
    --tool=drd
    --check-stack-var=yes
  ],
  'memcheck' => %w[
    --tool=memcheck
    --leak-check=full
    --show-leak-kinds=all
    --track-origins=yes
  ]
}.freeze

def run(cmd, label)
  puts "\n#{'=' * 60}"
  puts "  #{label}"
  puts "#{'=' * 60}\n\n"

  log_file = File.join(LOG_DIR, "valgrind-#{label}.log")
  full_cmd = cmd + ['2>&1']

  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)

  output = String.new
  error_count = 0
  IO.popen(cmd, err: [:child, :out]) do |io|
    io.each_line do |line|
      print line
      output << line
      # Parse Valgrind's ERROR SUMMARY to detect real (unsuppressed) errors
      if line =~ /ERROR SUMMARY:\s+(\d+)\s+errors/
        error_count = $1.to_i
      end
    end
  end
  status = $?
  elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0

  File.write(log_file, output)

  ok = status.success? && error_count.zero?
  tag = ok ? 'OK' : "FAIL (#{error_count} errors)"
  puts "\n--- #{label}: #{tag}  elapsed=#{'%.1f' % elapsed}s  log=#{log_file}"

  ok
end

# -- Main --

requested = ARGV.map(&:downcase)
requested = TOOLS.keys if requested.empty?

unknown = requested - TOOLS.keys
unless unknown.empty?
  $stderr.puts "Unknown tool(s): #{unknown.join(', ')}"
  $stderr.puts "Available: #{TOOLS.keys.join(', ')}"
  exit 1
end

# Build
puts "Building cache_valgrind_test ..."
unless system('cmake', '--build', BUILD, '--target', 'cache_valgrind_test', chdir: ROOT)
  $stderr.puts 'Build failed.'
  exit 1
end

unless File.executable?(BINARY)
  $stderr.puts "Binary not found: #{BINARY}"
  exit 1
end

# Run each tool
results = {}
supp_args = File.exist?(SUPP) ? ["--suppressions=#{SUPP}"] : []
requested.each do |tool|
  args = ['valgrind'] + supp_args + TOOLS[tool] + [BINARY]
  results[tool] = run(args, tool)
end

# Summary
puts "\n#{'=' * 60}"
puts '  SUMMARY'
puts "#{'=' * 60}"
results.each do |tool, ok|
  status = ok ? "\e[32mPASS\e[0m" : "\e[31mFAIL\e[0m"
  puts "  %-10s %s" % [tool, status]
end
puts

exit(results.values.all? ? 0 : 1)
