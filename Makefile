#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#

CC = gcc
CPPFLAGS = -D__EXTENSIONS__
CFLAGS = -std=c99 -g

mancvt: mancvt.o
	$(CC) -o mancvt mancvt.o
