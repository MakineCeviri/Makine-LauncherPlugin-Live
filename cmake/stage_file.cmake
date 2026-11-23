# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.
#
# Copies SRC to DST when SRC exists (an optional package file, such as the Makine TV APK);
# otherwise warns and leaves the package without it.
#   cmake -DSRC=<file> -DDST=<file> -P stage_file.cmake

if(EXISTS "${SRC}")
    file(COPY_FILE "${SRC}" "${DST}" ONLY_IF_DIFFERENT)
else()
    message(WARNING "${SRC} not found: the package has no ${DST}. Build it first (see CMakeLists.txt).")
endif()
