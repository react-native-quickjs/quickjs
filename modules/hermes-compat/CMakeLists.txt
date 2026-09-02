# Copyright (c) Ammar Ahmed.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# The Hermes compatibility shim. See README.md.

add_library(hermes_compat STATIC src/HermesCompat.cpp)

# PUBLIC: a consumer writing `#include <hermes/hermes.h>` must resolve it here,
# and must resolve it BEFORE any real Hermes headers.
target_include_directories(hermes_compat PUBLIC include)

target_link_libraries(hermes_compat PUBLIC quickjs_jsi)

set_target_properties(hermes_compat PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
)
