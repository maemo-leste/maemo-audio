# The mixer.
#
# One mixer per channel, one input port per stream, and a single level
# applied to the sum.  It also carries the volume ladder and the cache of the
# table a device publishes, because the slider is expressed in rungs of that
# ladder rather than as a linear factor.
#
# The SIMD kernels are separate translation units, each compiled with the
# flags its own instruction set needs, and the dispatcher is told which ones
# were built so its table lists nothing that is missing.  They live in ops/
# so the DSP and the kernels are not sitting in each other's pocket.

mixer_inc = include_directories('.', 'ops')

cpu = host_machine.cpu_family()

dispatcher_defs = []
kernels = []

# One static library per translation unit, because each kernel needs its own
# target flags and meson has no per-source c_args.  They are linked whole
# into the plugin and into the tests, so a test runs against the binary that
# ships rather than against a second compilation without those flags.
portable = static_library('mix-volume-c', 'ops/mix-volume-c.c',
  dependencies: [spa_dep], include_directories: mixer_inc)

kernels = [portable]

if cpu == 'x86' or cpu == 'x86_64'
  kernels += static_library('mix-volume-sse', 'ops/mix-volume-sse.c',
    c_args: ['-O3', '-msse', '-DHAVE_SSE'], dependencies: [spa_dep],
    include_directories: mixer_inc)
  kernels += static_library('mix-volume-avx2', 'ops/mix-volume-avx2.c',
    c_args: ['-O3', '-mavx2', '-mfma', '-DHAVE_AVX2'], dependencies: [spa_dep],
    include_directories: mixer_inc)
  dispatcher_defs = ['-DHAVE_SSE', '-DHAVE_SSE2', '-DHAVE_AVX2']
elif cpu == 'arm'
  # 32-bit ARM: NEON is not part of the baseline here, so ask for it.
  kernels += static_library('mix-volume-neon', 'ops/mix-volume-neon.c',
    c_args: ['-O3', '-mfpu=neon', '-DHAVE_NEON'], dependencies: [spa_dep],
    include_directories: mixer_inc)
  dispatcher_defs = ['-DHAVE_NEON']
elif cpu == 'aarch64'
  # NEON is baseline on aarch64, nothing to ask for.
  kernels += static_library('mix-volume-neon', 'ops/mix-volume-neon.c',
    c_args: ['-O3', '-DHAVE_NEON'], dependencies: [spa_dep],
    include_directories: mixer_inc)
  dispatcher_defs = ['-DHAVE_NEON']
endif

# The dispatcher is rebuilt with the defines for the kernels that exist, so
# its table lists nothing that was not compiled in.
dispatcher = static_library('mix-volume-table', 'ops/mix-volume.c',
  c_args: dispatcher_defs, dependencies: [spa_dep],
  include_directories: mixer_inc)

kernels += dispatcher

mixer_lib = library('spa-maemo-flat-volume',
  [
    'plugin.c',
    'mixer-node.c',
    'mixer-port.c',
    'mixer-process.c',
    'mixer-factory.c',
    'maemo-vol-map.c',
    'maemo-ladder-cache.c',
    'maemo-vol-state.c',
    'maemo-swstep.c',
  ],
  link_whole: kernels,
  dependencies: [spa_dep, pipewire_dep, math_dep],
  include_directories: mixer_inc,
  # Everything is hidden by default and what has to be reachable is marked
  # MAEMO_EXPORT; this .so is dlopened and the module resolves against it, so
  # an unmarked public function is a module that fails to load.
  gnu_symbol_visibility: 'hidden',
  # The module links this by name and the core dlopens it by path.  Without a
  # soname the two are two mappings of one file.
  link_args: ['-Wl,-soname,libspa-maemo-flat-volume.so'],
  install: true,
  install_dir: spa_plugindir / 'maemo-flat-volume')

mixer_dep = declare_dependency(link_with: mixer_lib,
                               include_directories: mixer_inc)

# ---- tests ---------------------------------------------------------------
#
# The volume invariant (hardware times software equals the virtual level,
# across the hardware handoff) and the ladder cache are checked here, so a
# regression fails the package build rather than shipping.

test('mix-volume', executable('test-mix-volume', 'tests/test-mix-volume.c',
  link_with: kernels,
  dependencies: [spa_dep, math_dep],
  include_directories: [mixer_inc, include_directories('tests')]))

test('vol-map', executable('test-vol-map',
  ['tests/test-vol-map.c', 'maemo-vol-map.c'],
  dependencies: [spa_dep, math_dep],
  include_directories: [mixer_inc, include_directories('tests')]))

test('ladder-cache', executable('test-ladder-cache',
  ['tests/test-ladder-cache.c', 'maemo-ladder-cache.c', 'maemo-vol-map.c'],
  dependencies: [spa_dep, math_dep],
  include_directories: [mixer_inc, include_directories('tests')]))
