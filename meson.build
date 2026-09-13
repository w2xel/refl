project(
  'cpp_runtime_reflection',
  'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++26',
    'warning_level=3',      # -Wall -Wextra
    'werror=true',
    # GCC 16.2 crashes in lto_read_decls for the reflection-heavy Dyn tests.
    # Keep LTO opt-in until the configured compiler can link that coverage.
    'b_lto=false',
    'optimization=2',
  ],
)

cpp = meson.get_compiler('cpp')

# C++26 compile-time reflection (P2996) — GCC 16, -freflection.
# Probe with a real reflection snippet: -freflection is rejected unless
# -std=c++26 is active, and the snippet confirms the <meta> header is usable.
has_reflection = cpp.compiles(
  '#include <meta>\nconstexpr auto r = ^^int;\nint main() {}',
  args: ['-std=c++26', '-freflection'],
  name: 'C++26 reflection (-freflection)',
)
assert(has_reflection,
       'compiler has no C++26 reflection; need GCC 16+ built with -freflection')
add_project_arguments('-freflection', language: 'cpp')

# GCC static analyzer — the flagship built-in linter, no clang required.
if cpp.has_argument('-fanalyzer')
  add_project_arguments('-fanalyzer', language: 'cpp')
endif

# Extra GCC warnings beyond -Wall -Wextra that catch real bugs.
#
# ponytail: -Wshadow is deliberately disabled. GCC 16.2's `template for`
# expansion-statement lowering creates a hidden variable that the named loop
# variable shadows, so -Wshadow fires a false positive on EVERY template-for
# (confirmed: it points the shadow at its own declaration). Since template-for
# over reflected members is the framework's core idiom, -Wshadow + -Werror
# would make the codebase unbuildable. meson's warning_level=3 adds -Wshadow,
# so we explicitly turn it back off. Re-enable once GCC fixes the lowering.
add_project_arguments('-Wno-shadow', language: 'cpp')

add_project_arguments(
  '-Wpedantic',
  '-Wconversion',
  '-Wsign-conversion',
  '-Wnon-virtual-dtor',
  '-Wold-style-cast',
  '-Wcast-align',
  '-Wdouble-promotion',
  '-Wformat=2',
  '-Wlogical-op',
  '-Wduplicated-cond',
  '-Wduplicated-branches',
  '-Wnull-dereference',
  '-Wuseless-cast',
  '-Wextra-semi',
  '-Wmismatched-tags',
  language: 'cpp',
)

# Header-only framework: subprojects/tests/samples include via their own
# include_directories('../include').
subdir('tests')
subdir('samples')
