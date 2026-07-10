#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

# module: transcoders

# Force-include <cstddef> automatically into every compiled file
add_compile_options(-include cstddef)

parabix_add_executable(
NAME
    gb18030
SRC
    gb18030.cpp
DEPS
    pablo.bixnum
    kernel.basis
    kernel.io
    kernel.pipeline
    kernel.streamutils
    kernel.unicode
    kernel.util
    re.adt
)

parabix_add_executable(
NAME
    u8u16
SRC
    u8u16.cpp
DEPS
    pablo.bixnum
    kernel.basis
    kernel.io
    kernel.pipeline
    kernel.streamutils
    kernel.unicode
    kernel.util
    re.adt
)

parabix_add_executable(
NAME
    zawgyi2unicode
SRC
    zawgyi2unicode.cpp
DEPS
    pablo
    pablo.bixnum
    kernel.basis
    kernel.io
    kernel.pipeline
    kernel.streamutils
    kernel.unicode
    re.cc
)