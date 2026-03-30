#!/usr/bin/env ruby
# frozen_string_literal: true
#
# spin_verify.rb — Comprehensive SPIN model checker driver for Promela models.
#
# Supports: safety, liveness (LTL), non-progress cycles, simulation,
#           trail replay, all major compression/search strategies, multi-core.
#
# Run with --help for complete usage and examples.

require 'optparse'
require 'fileutils'

SCRIPT_DIR = File.expand_path(File.dirname(__FILE__))

# ---------------------------------------------------------------------------
# LTL property descriptions (keys discovered dynamically from model file)
# ---------------------------------------------------------------------------
LTL_DESCRIPTIONS = {
  'every_requested_key_eventually_materializes' =>
    'Every requested key is eventually computed and stored',
  'no_starvation_thread0' => 'Thread 0 (Requester key=0) never starves',
  'no_starvation_thread1' => 'Thread 1 (Requester key=0) never starves',
  'no_starvation_thread2' => 'Thread 2 (Requester key=1) never starves',
  'no_starvation_thread3' => 'Thread 3 (Requester key=2) never starves',
  'no_starvation_thread4' => 'Thread 4 (FindRequester key=0) never starves',
  'no_starvation_thread5' => 'Thread 5 (FindRequester key=1) never starves',
  'all_terminate'         => 'All threads eventually terminate',
  'no_orphaned_computing' => 'All started computations eventually complete',
  'saturation_transient'  => 'Cache saturation is always transient',
}.freeze

# Auto-discover ltl property names from the model file
def discover_ltl_properties(model_path)
  props = {}
  return props unless File.exist?(model_path)

  File.foreach(model_path) do |line|
    if line =~ /^\s*ltl\s+(\w+)\s*\{/
      name = Regexp.last_match(1)
      props[name] = LTL_DESCRIPTIONS[name] || '(user-defined property)'
    end
  end
  props
end

# ---------------------------------------------------------------------------
# Terminal colors (disabled when not a tty)
# ---------------------------------------------------------------------------
module Color
  module_function

  def enabled?  = $stdout.tty?
  def wrap(text, code) = enabled? ? "\e[#{code}m#{text}\e[0m" : text.to_s
  def bold(t)    = wrap(t, '1')
  def red(t)     = wrap(t, '1;31')
  def green(t)   = wrap(t, '1;32')
  def yellow(t)  = wrap(t, '1;33')
  def cyan(t)    = wrap(t, '36')
  def dim(t)     = wrap(t, '2')
end

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
DEFAULTS = {
  model:       'cache_model.pml',
  depth:       100_000,
  memlim:      4096,
  nfair:       8,
  width:       nil,         # nil → let pan choose (default 2^24)
  errors:      1,
  cc:          'cc',
  opt:         '-O2',
  sim_steps:   5000,
  sim_seed:    nil,
  freq:        nil,         # progress frequency (states); nil → SPIN default
  cores:       nil,
  hash_seed:   nil,
  hash_bits:   nil,         # -kN for bitstate
  timeout:     nil,         # minutes
  vectorsz:    nil,
}.freeze

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def banner(msg)
  w = 72
  puts ''
  puts Color.cyan('═' * w)
  puts Color.bold("  #{msg}")
  puts Color.cyan('═' * w)
end

def cmd_line(cmd)
  puts "  #{Color.dim('$')} #{cmd}"
end

# Stream output in real-time while capturing it for post-analysis
def run_streaming(cmd, quiet: false)
  output = []
  io = IO.popen("#{cmd} 2>&1")
  io.each_line do |line|
    print line unless quiet
    output << line
  end
  io.close  # waits for child, sets $?
  [output.join, ($?.exitstatus rescue 1)]
end

def run!(cmd)
  cmd_line(cmd)
  out, code = run_streaming(cmd)
  return out if code == 0

  $stderr.puts Color.red("\n[ERROR] exit #{code}: #{cmd}")
  exit code
end

def cleanup(dir, pan_bin)
  FileUtils.rm_f(Dir.glob("#{dir}/pan.*"))
  FileUtils.rm_f("#{dir}/_spin_nvr.tmp")
  FileUtils.rm_f(pan_bin)
end

# Check tool availability
def require_tool!(name)
  return if system("command -v #{name} >/dev/null 2>&1")

  $stderr.puts Color.red("[ERROR] '#{name}' not found in PATH.")
  exit 1
end

# ---------------------------------------------------------------------------
# Compile-time flags (-D...) for pan.c
# ---------------------------------------------------------------------------
def build_cflags(opts, mode)
  flags = [opts[:opt]]

  # Memory limit
  flags << "-DMEMLIM=#{opts[:memlim]}"

  # Fairness support (needed at compile time for -f at runtime)
  flags << "-DNFAIR=#{opts[:nfair]}" unless mode == :safety && !opts[:weak_fairness]

  # === Mode-specific flags ===

  # Safety optimization: disables cycle-detection code → faster
  flags << '-DSAFETY' if mode == :safety && opts[:safety_opt] && !opts[:bfs_compile]

  # BFS (compile-time algorithm change, safety only)
  flags << '-DBFS' if opts[:bfs_compile]

  # Non-progress cycle detection
  flags << '-DNP' if mode == :non_progress

  # === Compression strategies (mutually exclusive) ===
  case opts[:compress]
  when :bitstate  then flags << '-DBITSTATE'
  when :hc        then flags << '-DHC'
  when :hc0       then flags << '-DHC0'
  when :hc1       then flags << '-DHC1'
  when :hc2       then flags << '-DHC2'
  when :hc3       then flags << '-DHC3'
  when :hc4       then flags << '-DHC4'
  when :collapse  then flags << '-DCOLLAPSE'
  end
  flags << "-DMA=#{opts[:ma]}" if opts[:ma]

  # === Partial order reduction ===
  flags << '-DNOREDUCE' if opts[:no_reduce]

  # === State vector ===
  flags << '-DFULL'   if opts[:full]
  flags << '-DLONG64' if opts[:long64]
  flags << "-DVECTORSZ=#{opts[:vectorsz]}" if opts[:vectorsz]
  flags << '-DNOCOMP' if opts[:no_comp]
  flags << '-DSPACE'  if opts[:space]

  # === Multi-core ===
  flags << "-DNCORE=#{opts[:cores]}" if opts[:cores]

  # === Bounded reachability ===
  flags << '-DREACH' if opts[:reach]

  # === No claims ===
  flags << '-DNOCLAIM' if opts[:no_claim]

  # === Randomization ===
  flags << '-DP_RAND'  if opts[:p_rand]
  flags << '-DT_RAND'  if opts[:t_rand]
  flags << '-DREVERSE' if opts[:reverse]

  # === Debug / instrumentation ===
  flags << '-DPRINTF'      if opts[:printf_on]
  flags << '-DVAR_RANGES'  if opts[:var_ranges]
  flags << '-DCHECK'       if opts[:check]
  flags << '-DNOBOUNDCHECK' if opts[:no_bound_check]
  flags << "-DFREQ=#{opts[:freq]}" if opts[:freq]

  flags.compact.join(' ')
end

# ---------------------------------------------------------------------------
# Runtime arguments for ./pan
# ---------------------------------------------------------------------------
def build_pan_args(opts, mode:, prop: nil)
  args = ["-m#{opts[:depth]}"]

  # Hash table size
  args << "-w#{opts[:width]}" if opts[:width]

  # Bitstate memory override (-M MB, -G GB)
  args << "-M#{opts[:bitstate_mb]}" if opts[:bitstate_mb]
  args << "-G#{opts[:bitstate_gb]}" if opts[:bitstate_gb]

  # Output control
  args << '-n' unless opts[:show_unreached]  # suppress unreached-state listing
  args << '-E' unless opts[:check_end_states] # ignore invalid end states

  # Error control
  args << "-c#{opts[:errors]}"
  args << '-e' if opts[:all_trails]  # trail for every error

  # === Mode-specific ===
  case mode
  when :safety
    args << '-b' if opts[:bounded]   # depth-exceeded = error
    args << '-i' if opts[:idfs]      # iterative deepening
    args << '-I' if opts[:idfs_fast] # approximate iterative deepening
  when :liveness
    args << '-a'                     # acceptance cycles (Büchi)
    args << '-f' unless opts[:no_fair]
    args << "-N #{prop}" if prop
  when :non_progress
    args << '-l'                     # non-progress cycles
    args << '-f' unless opts[:no_fair]
  end

  # Hash seed (useful for multi-run bitstate coverage)
  args << "-h#{opts[:hash_seed]}" if opts[:hash_seed]

  # Bitstate: bits per state
  args << "-k#{opts[:hash_bits]}" if opts[:hash_bits]

  # Timeout in minutes
  args << "-Q#{opts[:timeout]}" if opts[:timeout]

  # Verbose (full file names in unreached listing)
  args << '-v' if opts[:verbose]

  args.join(' ')
end

# ---------------------------------------------------------------------------
# Compile: spin -a → cc pan.c → pan binary
# ---------------------------------------------------------------------------
def compile(model, pan_bin, opts, mode)
  cflags = build_cflags(opts, mode)
  banner("Compiling verifier")
  puts "  Mode:  #{Color.bold(mode.to_s)}"
  puts "  Flags: #{cflags}"
  puts ''

  require_tool!('spin')
  require_tool!(opts[:cc])

  cmd_line("spin -a #{model}")
  spin_out, spin_code = run_streaming("spin -a #{model}")
  unless spin_code == 0
    $stderr.puts Color.red('[ERROR] spin -a failed.')
    exit spin_code
  end

  cc_cmd = "#{opts[:cc]} #{cflags} -o #{pan_bin} pan.c"
  cmd_line(cc_cmd)
  _, cc_code = run_streaming(cc_cmd)
  unless cc_code == 0
    $stderr.puts Color.red('[ERROR] Compilation failed.')
    exit cc_code
  end
  puts ''
end

# ---------------------------------------------------------------------------
# Result parsing
# ---------------------------------------------------------------------------
def parse_pan_result(output)
  errors = output[/errors:\s*(\d+)/, 1]&.to_i
  states = output[/states,\s*stored\s*$|(\S+)\s+states,\s*stored/m, 1]
  depth  = output[/depth reached\s+(\d+)/, 1]
  mem    = output[/total actual memory usage\s*$|(\S+)\s+total actual memory/m, 1]

  { errors: errors, states: states, depth: depth, memory: mem }
end

def report_result(result, label)
  puts ''
  if result[:errors] && result[:errors] == 0
    puts Color.green("  ✓ #{label}: PASSED")
  elsif result[:errors] && result[:errors] > 0
    puts Color.red("  ✗ #{label}: FAILED (#{result[:errors]} error(s))")
  else
    puts Color.yellow("  ? #{label}: result unclear — check output above")
  end

  parts = []
  parts << "states=#{result[:states]}" if result[:states]
  parts << "depth=#{result[:depth]}" if result[:depth]
  parts << "memory=#{result[:memory]}" if result[:memory]
  puts "  #{Color.dim(parts.join(', '))}" unless parts.empty?
end

# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------
def cmd_safety(model, pan_bin, opts)
  compile(model, pan_bin, opts, :safety)
  pan_args = build_pan_args(opts, mode: :safety)
  banner('Safety check — assertions & deadlocks')
  cmd_line("#{pan_bin} #{pan_args}")
  out, _code = run_streaming("#{pan_bin} #{pan_args}")
  result = parse_pan_result(out)
  report_result(result, 'Safety')
  result[:errors] == 0
end

def cmd_ltl(model, pan_bin, opts, prop, ltl_props)
  unless ltl_props.key?(prop)
    $stderr.puts Color.red("[ERROR] Unknown LTL property: '#{prop}'")
    $stderr.puts "Available:"
    ltl_props.each { |k, v| $stderr.puts "  #{k.ljust(50)} #{v}" }
    exit 1
  end

  compile(model, pan_bin, opts, :liveness)
  pan_args = build_pan_args(opts, mode: :liveness, prop: prop)
  desc = ltl_props[prop]
  banner("LTL: #{prop}")
  puts "  #{Color.dim(desc)}" if desc
  puts ''
  cmd_line("#{pan_bin} #{pan_args}")
  out, _code = run_streaming("#{pan_bin} #{pan_args}")
  result = parse_pan_result(out)
  report_result(result, "LTL '#{prop}'")
  result[:errors] == 0
end

def cmd_liveness(model, pan_bin, opts, props, ltl_props)
  passed = 0
  failed = 0
  props.each do |prop|
    ok = cmd_ltl(model, pan_bin, opts, prop, ltl_props)
    ok ? passed += 1 : failed += 1
    cleanup(SCRIPT_DIR, pan_bin)
  end
  puts ''
  banner('Liveness summary')
  puts "  #{Color.green("#{passed} passed")}, #{failed > 0 ? Color.red("#{failed} failed") : "#{failed} failed"}"
  failed == 0
end

def cmd_non_progress(model, pan_bin, opts)
  compile(model, pan_bin, opts, :non_progress)
  pan_args = build_pan_args(opts, mode: :non_progress)
  banner('Non-progress cycle detection')
  cmd_line("#{pan_bin} #{pan_args}")
  out, _code = run_streaming("#{pan_bin} #{pan_args}")
  result = parse_pan_result(out)
  report_result(result, 'Non-progress')
  result[:errors] == 0
end

def cmd_sim(model, opts)
  banner('Simulation')
  require_tool!('spin')

  args = []
  if opts[:interactive]
    args << '-i'
  else
    args += ['-p', '-g', '-l']
  end
  args << "-u#{opts[:sim_steps]}"
  args << "-n#{opts[:sim_seed]}" if opts[:sim_seed]
  args << '-T' if opts[:no_indent]

  cmd = "spin #{args.join(' ')} #{model}"
  cmd_line(cmd)
  run_streaming(cmd)
end

def cmd_trail(model, opts)
  banner('Trail replay')
  require_tool!('spin')

  trail_file = opts[:trail_file]
  unless trail_file
    # Find most recent trail file
    base = File.basename(model)
    candidates = Dir.glob("#{SCRIPT_DIR}/#{base}*.trail").sort_by { |f| File.mtime(f) }
    if candidates.empty?
      $stderr.puts Color.red('[ERROR] No trail file found. Run a verification first.')
      exit 1
    end
    trail_file = candidates.last
  end

  unless File.exist?(trail_file)
    $stderr.puts Color.red("[ERROR] Trail file not found: #{trail_file}")
    exit 1
  end

  puts "  Trail: #{trail_file}"
  args = ["-t#{trail_file}", '-p', '-g', '-l']
  args << '-v' if opts[:verbose]
  cmd = "spin #{args.join(' ')} #{model}"
  cmd_line(cmd)
  run_streaming(cmd)
end

def cmd_all(model, pan_bin, opts, ltl_props)
  banner('FULL VERIFICATION SUITE')
  total_pass = 0
  total_fail = 0

  # Safety
  ok = cmd_safety(model, pan_bin, opts)
  ok ? total_pass += 1 : total_fail += 1
  cleanup(SCRIPT_DIR, pan_bin)

  # All LTL properties
  ltl_props.each_key do |prop|
    ok = cmd_ltl(model, pan_bin, opts, prop, ltl_props)
    ok ? total_pass += 1 : total_fail += 1
    cleanup(SCRIPT_DIR, pan_bin)
  end

  puts ''
  banner('SUITE COMPLETE')
  puts "  #{Color.green("#{total_pass} passed")}, " \
       "#{total_fail > 0 ? Color.red("#{total_fail} failed") : "#{total_fail} failed"} " \
       "(#{total_pass + total_fail} total)"
  total_fail == 0
end

def cmd_list_props(ltl_props)
  puts ''
  puts Color.bold("  LTL properties (#{ltl_props.size} total):")
  puts ''
  max_len = ltl_props.keys.map(&:length).max || 0
  ltl_props.each do |name, desc|
    puts "    #{Color.cyan(name.ljust(max_len + 2))} #{desc}"
  end
  puts ''
end

# ---------------------------------------------------------------------------
# Option parser
# ---------------------------------------------------------------------------
def parse_options(argv) # rubocop:disable Metrics/MethodLength, Metrics/AbcSize
  opts = DEFAULTS.dup

  # Compression
  opts[:compress]        = :none
  opts[:ma]              = nil

  # POR / state vector
  opts[:no_reduce]       = false
  opts[:full]            = false
  opts[:long64]          = false
  opts[:no_comp]         = false
  opts[:space]           = false

  # Safety optimization
  opts[:safety_opt]      = true   # auto for safety command

  # Search
  opts[:bfs_compile]     = false
  opts[:bounded]         = false
  opts[:idfs]            = false
  opts[:idfs_fast]       = false
  opts[:reach]           = false

  # Fairness
  opts[:weak_fairness]   = false
  opts[:no_fair]         = false

  # Simulation
  opts[:interactive]     = false
  opts[:no_indent]       = false

  # Trail
  opts[:trail_file]      = nil

  # Hash / bitstate
  opts[:bitstate_mb]     = nil
  opts[:bitstate_gb]     = nil

  # Error output
  opts[:all_trails]      = false
  opts[:show_unreached]  = false
  opts[:check_end_states] = false

  # Multi-core
  # (opts[:cores] from DEFAULTS is nil)

  # Randomization
  opts[:p_rand]          = false
  opts[:t_rand]          = false
  opts[:reverse]         = false

  # Debug
  opts[:printf_on]       = false
  opts[:var_ranges]      = false
  opts[:check]           = false
  opts[:no_bound_check]  = false
  opts[:verbose]         = false
  opts[:quiet]           = false

  # Misc
  opts[:no_claim]        = false
  opts[:keep]            = false

  prog = File.basename($PROGRAM_NAME)

  op = OptionParser.new do |o|
    o.banner = <<~BANNER
      #{Color.bold('spin_verify.rb')} — Comprehensive SPIN model checker driver

      #{Color.bold('USAGE')}
          #{prog} [options] <command> [args...]

      #{Color.bold('COMMANDS')}
          #{Color.cyan('safety')}                Check assertions and deadlock freedom
          #{Color.cyan('liveness')} [props...]   Verify LTL properties (default: all)
          #{Color.cyan('ltl')} <property>        Verify a single named LTL property
          #{Color.cyan('non-progress')}          Detect non-progress cycles (-DNP, -l)
          #{Color.cyan('sim')}                   Random or interactive simulation
          #{Color.cyan('trail')}                 Replay error trail from last verification
          #{Color.cyan('all')}                   Full suite: safety + all LTL properties
          #{Color.cyan('list-props')}            List LTL properties defined in the model

    BANNER

    o.separator Color.bold('  MODEL')
    o.on('--model FILE', "Promela model file [#{DEFAULTS[:model]}]") { |v| opts[:model] = v }

    # --- Search -----------------------------------------------------------
    o.separator ''
    o.separator Color.bold('  SEARCH')
    o.on('--depth N', Integer,
         "Max search depth [#{DEFAULTS[:depth]}]") { |v| opts[:depth] = v }
    o.on('--width N', Integer,
         'Hash table size 2^N [pan default: 24]') { |v| opts[:width] = v }
    o.on('--errors N', Integer,
         "Stop after N errors; 0 = find all [#{DEFAULTS[:errors]}]") { |v| opts[:errors] = v }
    o.on('--all-trails',
         'Write a trail file for every error found (-e)') { opts[:all_trails] = true }
    o.on('--bfs',
         'Breadth-first search (compile-time -DBFS, safety only)') { opts[:bfs_compile] = true }
    o.on('--bounded',
         'Treat depth-exceeded as an error (pan -b)') { opts[:bounded] = true }
    o.on('--idfs',
         'Iterative deepening DFS (pan -i)') { opts[:idfs] = true }
    o.on('--idfs-fast',
         'Approximate iterative deepening (pan -I)') { opts[:idfs_fast] = true }
    o.on('--reach',
         'Bounded reachability: guarantee within depth limit (-DREACH)') { opts[:reach] = true }
    o.on('--timeout N', Integer,
         'Stop after N minutes (pan -Q)') { |v| opts[:timeout] = v }

    # --- Compression ------------------------------------------------------
    o.separator ''
    o.separator Color.bold('  STATE-SPACE COMPRESSION  (choose at most one)')
    o.on('--bitstate',
         'Supertrace/bitstate hashing — huge models, lossy') { opts[:compress] = :bitstate }
    o.on('--hash-compact [BITS]', Integer,
         'Hash compaction: 0-4 selects HC0..HC4; omit for HC [lossless]') do |v|
      opts[:compress] = v ? :"hc#{v}" : :hc
    end
    o.on('--collapse',
         'Collapse compression — lossless, best for shallow stacks') { opts[:compress] = :collapse }
    o.on('--ma N', Integer,
         'Minimized automaton with N-byte bound (-DMA=N)') { |v| opts[:ma] = v }

    # --- Bitstate tuning --------------------------------------------------
    o.separator ''
    o.separator Color.bold('  BITSTATE TUNING  (only with --bitstate)')
    o.on('--hash-seed N', Integer,
         'Hash function seed 0..499 (vary for coverage)') { |v| opts[:hash_seed] = v }
    o.on('--hash-bits N', Integer,
         'Bits stored per state (pan -k, default 3)') { |v| opts[:hash_bits] = v }
    o.on('--bitstate-mb N', Integer,
         'MB for bitstate hash array (pan -M)') { |v| opts[:bitstate_mb] = v }
    o.on('--bitstate-gb N', Integer,
         'GB for bitstate hash array (pan -G)') { |v| opts[:bitstate_gb] = v }

    # --- POR / state vector -----------------------------------------------
    o.separator ''
    o.separator Color.bold('  PARTIAL ORDER REDUCTION & STATE VECTOR')
    o.on('--no-reduce',
         'Disable POR entirely (-DNOREDUCE) — slower, more thorough') { opts[:no_reduce] = true }
    o.on('--full',
         'Full state-vector storage (-DFULL) — no lossy compression') { opts[:full] = true }
    o.on('--no-comp',
         'Disable state compression (-DNOCOMP) — trades memory for speed') { opts[:no_comp] = true }
    o.on('--long64',
         '64-bit state vectors (-DLONG64)') { opts[:long64] = true }
    o.on('--vectorsz N', Integer,
         'State-vector size in bytes (-DVECTORSZ=N)') { |v| opts[:vectorsz] = v }
    o.on('--space',
         'Optimize for memory over speed (-DSPACE)') { opts[:space] = true }

    # --- Safety optimization ----------------------------------------------
    o.separator ''
    o.separator Color.bold('  SAFETY OPTIMIZATION')
    o.on('--[no-]safety-opt',
         'Compile with -DSAFETY for safety cmd [default: on]') { |v| opts[:safety_opt] = v }

    # --- Fairness ---------------------------------------------------------
    o.separator ''
    o.separator Color.bold('  LIVENESS / FAIRNESS')
    o.on('--fair [N]', Integer,
         "Set NFAIR bound [#{DEFAULTS[:nfair]}]; fairness is ON by default for liveness") do |v|
      opts[:nfair] = v || DEFAULTS[:nfair]
      opts[:weak_fairness] = true
    end
    o.on('--no-fair',
         'Disable weak fairness for liveness (may produce spurious cycles)') { opts[:no_fair] = true }
    o.on('--show-unreached',
         'Show unreached states in output (pan without -n)') { opts[:show_unreached] = true }
    o.on('--check-end-states',
         'Check invalid end states (pan without -E)') { opts[:check_end_states] = true }
    o.on('--no-claim',
         'Ignore all never claims (-DNOCLAIM)') { opts[:no_claim] = true }

    # --- Memory -----------------------------------------------------------
    o.separator ''
    o.separator Color.bold('  MEMORY')
    o.on('--memlim N', Integer,
         "Memory limit in MB [#{DEFAULTS[:memlim]}]") { |v| opts[:memlim] = v }

    # --- Multi-core -------------------------------------------------------
    o.separator ''
    o.separator Color.bold('  MULTI-CORE')
    o.on('--cores N', Integer,
         'Use N CPU cores (-DNCORE=N)') { |v| opts[:cores] = v }

    # --- Randomization ----------------------------------------------------
    o.separator ''
    o.separator Color.bold('  RANDOMIZATION')
    o.on('--p-rand',
         'Randomize process scheduling (-DP_RAND)') { opts[:p_rand] = true }
    o.on('--t-rand',
         'Randomize transition order (-DT_RAND)') { opts[:t_rand] = true }
    o.on('--reverse',
         'Reverse exploration order (-DREVERSE)') { opts[:reverse] = true }

    # --- Compiler ---------------------------------------------------------
    o.separator ''
    o.separator Color.bold('  COMPILER')
    o.on('--cc CMD',
         "C compiler [#{DEFAULTS[:cc]}]") { |v| opts[:cc] = v }
    o.on('--cflags FLAGS',
         "Optimization flags [#{DEFAULTS[:opt]}]") { |v| opts[:opt] = v }

    # --- Simulation -------------------------------------------------------
    o.separator ''
    o.separator Color.bold('  SIMULATION')
    o.on('--steps N', Integer,
         "Max simulation steps [#{DEFAULTS[:sim_steps]}]") { |v| opts[:sim_steps] = v }
    o.on('--seed N', Integer,
         'Random seed for simulation') { |v| opts[:sim_seed] = v }
    o.on('--interactive',
         'Interactive/guided simulation (spin -i)') { opts[:interactive] = true }

    # --- Trail replay -----------------------------------------------------
    o.separator ''
    o.separator Color.bold('  TRAIL REPLAY')
    o.on('--trail-file FILE',
         'Specific trail file to replay') { |v| opts[:trail_file] = v }

    # --- Debug / instrumentation ------------------------------------------
    o.separator ''
    o.separator Color.bold('  DEBUG & INSTRUMENTATION')
    o.on('--printf',
         'Enable printf during verification (-DPRINTF)') { opts[:printf_on] = true }
    o.on('--var-ranges',
         'Report variable value ranges (-DVAR_RANGES)') { opts[:var_ranges] = true }
    o.on('--check',
         'Extra search-progress reporting (-DCHECK)') { opts[:check] = true }
    o.on('--no-bound-check',
         'Disable array-bounds checking (-DNOBOUNDCHECK)') { opts[:no_bound_check] = true }
    o.on('--freq N', Integer,
         'Progress report every N stored states (-DFREQ=N)') { |v| opts[:freq] = v }

    # --- Misc -------------------------------------------------------------
    o.separator ''
    o.separator Color.bold('  MISC')
    o.on('--keep',
         'Keep generated pan.* files after run') { opts[:keep] = true }
    o.on('-v', '--verbose',
         'Verbose output') { opts[:verbose] = true }
    o.on('-q', '--quiet',
         'Suppress real-time pan output (show only result)') { opts[:quiet] = true }

    o.separator ''
    o.on('-h', '--help', 'Show this help') do
      puts o
      print_examples(prog)
      exit 0
    end
  end

  begin
    remaining = op.parse!(argv)
  rescue OptionParser::InvalidOption, OptionParser::MissingArgument => e
    $stderr.puts Color.red("[ERROR] #{e.message}")
    $stderr.puts "Run '#{prog} --help' for usage."
    exit 1
  end

  validate_options!(opts, remaining.first)
  [opts, remaining]
end

# ---------------------------------------------------------------------------
# Validation: catch incompatible option combinations early
# ---------------------------------------------------------------------------
def validate_options!(opts, command)
  if opts[:bfs_compile] && command && !%w[safety all].include?(command)
    $stderr.puts Color.red('[ERROR] --bfs is only valid with the safety (or all) command.')
    exit 1
  end

  if opts[:no_claim] && command && %w[liveness ltl].include?(command)
    $stderr.puts Color.red('[ERROR] --no-claim is incompatible with liveness/ltl commands.')
    exit 1
  end

  if opts[:compress] == :bitstate && opts[:full]
    $stderr.puts Color.yellow('[WARN] --full is ignored when --bitstate is active.')
    opts[:full] = false
  end

  if opts[:cores] && opts[:ma]
    $stderr.puts Color.yellow('[WARN] --ma is incompatible with multi-core; disabling --cores.')
    opts[:cores] = nil
  end
end

# ---------------------------------------------------------------------------
# Examples block (printed with --help)
# ---------------------------------------------------------------------------
def print_examples(prog)
  puts <<~EXAMPLES

    #{Color.bold('EXAMPLES')}

      #{Color.cyan('# Basic verification')}
      #{prog} safety                           # exhaustive safety check
      #{prog} liveness                         # all LTL properties (fairness auto-on)
      #{prog} ltl all_terminate                # single LTL property
      #{prog} all                              # full suite: safety + all LTL
      #{prog} list-props                       # show available properties

      #{Color.cyan('# Large state spaces (compression strategies)')}
      #{prog} safety --bitstate                # supertrace — fast, approximate
      #{prog} safety --bitstate --hash-seed 1  # vary hash seed for extra coverage
      #{prog} safety --bitstate --hash-seed 2  # each seed explores differently
      #{prog} safety --hash-compact            # lossless HC (≈50% memory saving)
      #{prog} safety --hash-compact 4          # HC4 variant (more bits, more reliable)
      #{prog} safety --collapse                # collapse compression (lossless)
      #{prog} safety --ma 200                  # minimized automaton (exact, small models)

      #{Color.cyan('# Search strategies')}
      #{prog} safety --bfs                     # BFS: finds shortest counterexample
      #{prog} safety --depth 500000            # deeper DFS search
      #{prog} safety --idfs                    # iterative deepening DFS
      #{prog} safety --bounded --depth 200000  # error if depth-limit reached
      #{prog} safety --reach --depth 200000    # guarantee within depth bound
      #{prog} safety --errors 0                # find ALL errors, don't stop at first
      #{prog} safety --errors 0 --all-trails   # save trail for each error found
      #{prog} safety --timeout 30              # stop after 30 minutes

      #{Color.cyan('# Liveness / fairness')}
      #{prog} liveness                         # fairness on by default
      #{prog} ltl saturation_transient         # single property check
      #{prog} liveness --no-fair               # disable fairness (see unfair traces)
      #{prog} liveness --fair 16               # increase NFAIR bound
      #{prog} liveness all_terminate no_orphaned_computing  # subset of properties

      #{Color.cyan('# Non-progress cycles')}
      #{prog} non-progress                     # detect non-progress loops

      #{Color.cyan('# Maximum coverage (slow but thorough)')}
      #{prog} safety --no-reduce               # disable POR — all interleavings
      #{prog} safety --no-reduce --full        # POR off + full state vectors
      #{prog} all --no-reduce --memlim 16384   # full suite, no POR, 16 GB

      #{Color.cyan('# Multi-core')}
      #{prog} safety --cores 4                 # use 4 CPU cores
      #{prog} safety --cores 8 --memlim 32768  # 8 cores, 32 GB

      #{Color.cyan('# Randomized exploration (swarm-like)')}
      #{prog} safety --bitstate --p-rand --hash-seed 0
      #{prog} safety --bitstate --t-rand --hash-seed 1
      #{prog} safety --bitstate --reverse --hash-seed 2

      #{Color.cyan('# Simulation')}
      #{prog} sim                              # random simulation
      #{prog} sim --steps 50000                # longer run
      #{prog} sim --seed 42                    # reproducible simulation
      #{prog} sim --interactive                # step-by-step guided simulation

      #{Color.cyan('# Trail replay (after a verification finds an error)')}
      #{prog} trail                            # replay most recent trail
      #{prog} trail --trail-file my.trail      # replay specific trail file

      #{Color.cyan('# Debug & instrumentation')}
      #{prog} safety --var-ranges              # report variable value ranges
      #{prog} safety --printf                  # enable printf during verification
      #{prog} safety --check                   # extra progress diagnostics
      #{prog} safety --freq 500000             # progress every 500K states
      #{prog} safety --verbose --show-unreached

      #{Color.cyan('# Compiler')}
      #{prog} safety --cc gcc --cflags "-O3 -march=native"
      #{prog} safety --cc clang

      #{Color.cyan('# Retain build artifacts for inspection')}
      #{prog} safety --keep                    # keep pan.* files after run

    #{Color.bold('COMPRESSION STRATEGIES')}

      Strategy         Flag              Memory   Completeness
      ─────────────────────────────────────────────────────────────
      Exhaustive       (default)         high     complete (100%)
      Bitstate         --bitstate        low      approximate (may miss errors)
      Hash-compact     --hash-compact    medium   lossless (complete)
      Hash-compact N   --hash-compact N  medium   lossless (N=0..4, more bits)
      Collapse         --collapse        medium   lossless (complete)
      Min. automaton   --ma N            lowest   exact (needs N-byte bound)

    #{Color.bold('PARTIAL ORDER REDUCTION (POR)')}

      Enabled by default. Prunes equivalent interleavings for dramatic speedup.
      Disable with --no-reduce when:
        • Debugging a suspected concurrency bug hidden by POR
        • Checking a model with rendezvous channels (POR can be unsound)
        • You need full state coverage for certification

    #{Color.bold('FAIRNESS')}

      Automatically enabled for liveness/ltl/non-progress commands.
      NFAIR (default 8) bounds how many times an enabled transition may be
      skipped before it is forced. Without fairness, non-terminating processes
      (Background, Invalidator, etc.) can starve Requesters, producing
      spurious acceptance-cycle counterexamples.

      --no-fair    Disable (reproduce specific unfair traces)
      --fair N     Override NFAIR bound (higher → slower, stricter fairness)

    #{Color.bold('MULTI-RUN BITSTATE (Swarm-like Coverage)')}

      Bitstate hashing is approximate: a single run may miss errors.
      To increase coverage, run multiple times with different hash seeds:

        for seed in 0 1 2 3 4; do
          #{prog} safety --bitstate --hash-seed $seed
        done

      Combining --p-rand / --t-rand / --reverse further diversifies exploration.

  EXAMPLES
end

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main
  opts, args = parse_options(ARGV)

  command = args.shift
  unless command
    $stderr.puts Color.red('[ERROR] No command specified.')
    $stderr.puts "Run '#{File.basename($PROGRAM_NAME)} --help' for usage."
    exit 1
  end

  model_file = opts[:model]
  model = if File.absolute_path?(model_file) == model_file
            model_file
          else
            File.join(SCRIPT_DIR, model_file)
          end
  pan = File.join(SCRIPT_DIR, 'pan')

  unless File.exist?(model)
    $stderr.puts Color.red("[ERROR] Model not found: #{model}")
    exit 1
  end

  ltl_props = discover_ltl_properties(model)

  Dir.chdir(SCRIPT_DIR)

  ok = true
  begin
    case command
    when 'safety'
      ok = cmd_safety(model, pan, opts)
    when 'liveness'
      props = args.empty? ? ltl_props.keys : args
      ok = cmd_liveness(model, pan, opts, props, ltl_props)
    when 'ltl'
      prop = args.shift
      unless prop
        $stderr.puts Color.red('[ERROR] ltl requires a property name.')
        cmd_list_props(ltl_props)
        exit 1
      end
      ok = cmd_ltl(model, pan, opts, prop, ltl_props)
    when 'non-progress'
      ok = cmd_non_progress(model, pan, opts)
    when 'sim'
      cmd_sim(model, opts)
    when 'trail'
      cmd_trail(model, opts)
    when 'all'
      ok = cmd_all(model, pan, opts, ltl_props)
    when 'list-props'
      cmd_list_props(ltl_props)
    else
      $stderr.puts Color.red("[ERROR] Unknown command: '#{command}'")
      $stderr.puts "Run '#{File.basename($PROGRAM_NAME)} --help' for usage."
      exit 1
    end
  ensure
    cleanup(SCRIPT_DIR, pan) unless opts[:keep]
  end

  exit(ok ? 0 : 1)
end

main