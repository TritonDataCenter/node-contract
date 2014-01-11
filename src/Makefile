#
# Copyright (c) 2012 Joyent, Inc.  All rights reserved.
#

PREFIX_NODE :=	$(shell dirname $$(bash -c 'hash node; hash -t node'))/..
V8PLUS :=       $(shell $(PREFIX_NODE)/bin/node -e 'require("v8plus");')

include $(V8PLUS)/Makefile.v8plus.defs

MODULE =	contract_binding

SRCS =	\
		contracts.c \
		event.c \
		node_contract.c

CC =		/opt/local/bin/gcc
CXX =		/opt/local/bin/g++
STD_DEFS +=	-D__EXTENSIONS__

LIBS +=		-lcontract -lumem

include $(V8PLUS)/Makefile.v8plus.targ
