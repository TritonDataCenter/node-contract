#
# Copyright (c) 2012 Joyent, Inc.  All rights reserved.
#

V8PLUS :=       $(shell $(PREFIX_NODE)/bin/node -e 'require("v8plus");')

include $(V8PLUS)/Makefile.v8plus.defs

MODULE =	contract_binding

SRCS =	\
		contracts.c \
		event.c \
		node_contract.c

ERRNO_JSON =	errno.json

CC =		/opt/local/bin/gcc
CXX =		/opt/local/bin/g++

LIBS +=		-lcontract

include $(V8PLUS)/Makefile.v8plus.targ
