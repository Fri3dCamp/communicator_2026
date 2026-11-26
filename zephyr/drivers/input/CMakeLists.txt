# Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
# SPDX-License-Identifier: Apache-2.0

zephyr_library_amend(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr)
zephyr_library_sources_ifdef(CONFIG_INPUT_FRI3D_COMMUNICATOR input_fri3d_communicator.c)
