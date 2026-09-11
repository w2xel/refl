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

# Dyn layer not yet updated for the ObjectRef/Object transport change.
# test_dyn = executable(
#   'test_dyn',
#   'test_dyn.cpp',
#   include_directories: inc,
#   install: false,
# )
# test('dyn_api', test_dyn)
