# Tests
#
# Each test is a standalone executable that returns non-zero on failure;
# meson's test() propagates that. No external test framework yet — kept
# dependency-free per the project's clang-free / minimal-deps stance.
# Add doctest or Google Test here later if structured assertions are needed.

inc = include_directories('../include')

selfcheck = executable(
  'selfcheck',
  'selfcheck.cpp',
  include_directories: inc,
  install: false,
)
test('selfcheck', selfcheck)

test_refl = executable(
  'test_refl',
  'test_refl.cpp',
  include_directories: inc,
  install: false,
)
test('refl_api', test_refl)

test_dyn = executable(
  'test_dyn',
  'test_dyn.cpp',
  include_directories: inc,
  install: false,
)
test('dyn_api', test_dyn)

test_mockable = executable(
  'test_mockable',
  'test_mockable.cpp',
  include_directories: inc,
  install: false,
)
test('mockable_api', test_mockable)

test_hooks = executable(
  'test_hooks',
  'test_hooks.cpp',
  include_directories: inc,
  install: false,
)
test('hooks_api', test_hooks)

# Each extracted contract compiles by itself, without reflection extensions.
foreach header : ['error', 'descriptor', 'type', 'value', 'call']
  assert(cpp.compiles(
    '#include <refl/core/' + header + '.hpp>\nint main() {}',
    include_directories: inc,
    args: ['-std=c++23', '-fno-reflection'],
    name: 'standalone core/' + header + '.hpp without reflection',
  ), 'core contract header is not independently consumable')
endforeach

core_headers = executable(
  'test_core_headers',
  'test_core_headers.cpp',
  include_directories: inc,
  cpp_args: ['-fno-reflection'],
  override_options: ['cpp_std=c++23'],
  install: false,
)
test('core_headers', core_headers)

test('core_include_boundaries', find_program('check_core_includes.py'))

metadata_lifetime = executable(
  'test_metadata_lifetime',
  'test_metadata_lifetime.cpp',
  include_directories: inc,
  install: false,
)
test('metadata_lifetime', metadata_lifetime)

call_contract = executable(
  'test_call_contract', 'test_call_contract.cpp',
  include_directories: inc,
  cpp_args: ['-fno-reflection'],
  override_options: ['cpp_std=c++23'],
  install: false,
)
test('call_contract', call_contract)

benchmark_calls = executable(
  'benchmark_calls', 'benchmark_calls.cpp',
  include_directories: inc,
  cpp_args: ['-fno-reflection'],
  override_options: ['cpp_std=c++23'],
  install: false,
)
test('call_benchmark_smoke', benchmark_calls)
