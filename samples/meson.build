# Samples — runnable usage examples.
# Run as smoke tests so examples cannot silently stop working.

inc = include_directories('../include')

basic_sample = executable(
  'basic_sample',
  'basic.cpp',
  include_directories: inc,
  install: false,
)

dynamic_sample = executable(
  'dynamic_sample',
  'dynamic.cpp',
  include_directories: inc,
  install: false,
)

proxy_sample = executable(
  'proxy_sample',
  'proxy.cpp',
  include_directories: inc,
  install: false,
)

test('basic_sample', basic_sample, suite: 'samples')
test('dynamic_sample', dynamic_sample, suite: 'samples')
test('proxy_sample', proxy_sample, suite: 'samples')
