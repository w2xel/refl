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

test_skeleton = executable(
  'test_skeleton',
  'test_skeleton.cpp',
  include_directories: inc,
  install: false,
)
test('skeleton', test_skeleton)
