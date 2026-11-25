# Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
# SPDX-License-Identifier: Apache-2.0

# sources go into Zephyr's drivers__mfd library (named relative to zephyr/)
zephyr_library_amend(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr)
zephyr_library_sources_ifdef(CONFIG_MFD_FRI3D_COMMUNICATOR mfd_fri3d_communicator.c)
zephyr_library_sources_ifdef(CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE mfd_fri3d_communicator_update.c)
zephyr_library_sources_ifdef(CONFIG_MFD_FRI3D_COMMUNICATOR_SHELL mfd_fri3d_communicator_shell.c)

# The update uses the zephyr-rvswd module (west project, module name rvswd)
if(CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE AND NOT CONFIG_RVSWD_CH32X03X)
  if(NOT ZEPHYR_RVSWD_MODULE_DIR)
    message(FATAL_ERROR "CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE needs the zephyr-rvswd module: "
            "add github.com/bertouttier/zephyr-rvswd to the west manifest")
  endif()
  message(FATAL_ERROR "CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE needs an enabled wch,rvswd node "
          "as the communicator's rvswd property")
endif()

# Communicator firmware image to link in (CONFIG_MFD_FRI3D_COMMUNICATOR_FIRMWARE). Without one
# the update API stays: the application can get an image some other way.
if(CONFIG_MFD_FRI3D_COMMUNICATOR_UPDATE AND NOT CONFIG_MFD_FRI3D_COMMUNICATOR_FIRMWARE STREQUAL "")
  get_filename_component(fri3d_communicator_fw ${CONFIG_MFD_FRI3D_COMMUNICATOR_FIRMWARE} ABSOLUTE
                         BASE_DIR ${APPLICATION_SOURCE_DIR})
endif()
if(fri3d_communicator_fw AND NOT EXISTS ${fri3d_communicator_fw})
  message(WARNING "CONFIG_MFD_FRI3D_COMMUNICATOR_FIRMWARE: ${fri3d_communicator_fw} not found, "
          "building without a bundled image")
elseif(fri3d_communicator_fw)
  generate_inc_file_for_target(${ZEPHYR_CURRENT_LIBRARY} ${fri3d_communicator_fw}
                               ${ZEPHYR_BINARY_DIR}/include/generated/fri3d_communicator_firmware.inc)
  zephyr_library_compile_definitions(FRI3D_COMMUNICATOR_HAS_FIRMWARE)
endif()
