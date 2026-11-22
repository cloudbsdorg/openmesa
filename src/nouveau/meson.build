# Copyright © 2021 Ilia Mirkin
# SPDX-License-Identifier: MIT

subdir('headers')
# CloudBSD fork: nouveau DRM nvif headers removed; NVK runs on libnvrm.
# CloudBSD fork: nouveau winsys removed; NVK runs on libnvrm.
if with_nouveau_vk
  subdir('rust')
  subdir('nil')
  subdir('compiler')
endif
# CloudBSD fork: nouveau DRM shim removed; NVK runs on libnvrm.
if with_nouveau_vk
  subdir('cubin')
  subdir('mme')
  subdir('vulkan')
endif
