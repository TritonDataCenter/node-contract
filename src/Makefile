#
# Copyright (c) 2012 Joyent, Inc.  All rights reserved.
#

V8PLUS =	./v8plus

include $(V8PLUS)/Makefile.v8plus.defs

MODULE =	contract

SRCS =	\
		node_contract.c

ERRNO_JSON =	errno.json

PREFIX_NODE =	/opt/node
CC =		/opt/local/bin/gcc
CXX =		/opt/local/bin/g++

include $(V8PLUS)/Makefile.v8plus.targ
