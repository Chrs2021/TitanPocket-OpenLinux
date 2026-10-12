# SPDX-License-Identifier: GPL-2.0
hostprogs-y	:= genheaders
HOST_EXTRACFLAGS += \
	-I$(srctree)/include/uapi -I$(srctree)/include \
	-I$(srctree)/security/selinux/include \
	-I$(srctree)/arch/arm64/include/uapi

always		:= $(hostprogs-y)
