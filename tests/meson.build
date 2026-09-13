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
