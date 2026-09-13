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

test_dispatch_table = executable(
  'test_dispatch_table',
  'test_dispatch_table.cpp',
  include_directories: inc,
  install: false,
)
test('dispatch_table_api', test_dispatch_table)

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

core_contracts = executable(
  'test_core_contracts', ['test_core_contracts.cpp', 'type_identity_peer.cpp'],
  include_directories: inc,
  cpp_args: ['-fno-reflection'],
  override_options: ['cpp_std=c++23'],
  install: false,
)
test('core_contracts', core_contracts)

runtime_contracts = executable('test_runtime_contracts', 'test_runtime_contracts.cpp',
  include_directories: inc, install: false)
test('runtime_contracts', runtime_contracts)

registry_contracts = executable('test_registry', 'test_registry.cpp',
  include_directories: inc, install: false)
test('registry_contracts', registry_contracts)

binding_contracts = executable('test_binding', 'test_binding.cpp',
  include_directories: inc, install: false)
test('binding_contracts', binding_contracts)

dynamic_state = executable('test_dynamic_state', 'test_dynamic_state.cpp',
  include_directories: inc, install: false)
test('dynamic_state', dynamic_state)
