#
# Copyright (c) 2012, Joyent, Inc. All rights reserved.
#
# Makefile: basic Makefile for template API service
#
# This Makefile is a template for new repos. It contains only repo-specific
# logic and uses included makefiles to supply common targets (javascriptlint,
# jsstyle, restdown, etc.), which are used by other repos as well. You may well
# need to rewrite most of this file, but you shouldn't need to touch the
# included makefiles.
#
# If you find yourself adding support for new targets that could be useful for
# other projects too, you should add these to the original versions of the
# included Makefiles (in eng.git) so that other teams can use them too.
#

#
# Tools
#
NPM		:= npm
TAP		:= ./node_modules/.bin/tap

#
# Files
#
DOC_FILES	 = \
		index.restdown

JS_FILES	:= \
		lib/contract.js \
		test.js

CLEAN_FILES	+= \
		lib/contract_binding.node \
		src/*.o \
		src/v8plus_errno.c \
		src/v8plus_errno.h \
		src/mapfile_node
		
JSL_CONF_NODE	 = tools/jsl.node.conf
JSL_FILES_NODE   = $(JS_FILES)
JSSTYLE_FILES	 = $(JS_FILES)
JSSTYLE_FLAGS    = -o indent=tab,doxygen,unparenthesized-return=1

#
# Repo-specific targets
#
.PHONY: all
all: rebuild

.PHONY: rebuild
rebuild:
	(cd src && $(MAKE))

.PHONY: test
test: $(TAP)
	TAP=1 $(TAP) test

include ./Makefile.deps
include ./Makefile.targ
