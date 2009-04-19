/*
 *  util.c - Onkyo receiver utility functions
 *
 *  Copyright (c) 2008 Dan McGee <dpmcgee@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h> /* malloc */
#include <sys/stat.h> /* open */
#include <fcntl.h>  /* open */
#include <unistd.h> /* close, read, write */
#include <errno.h>  /* for errno refs */
#include <string.h> /* memcpy */

#include "onkyo.h"

int xopen(const char *path, int oflag)
{
	int fd;
	while(1) {
		fd = open(path, oflag);
		if ((fd < 0) && errno == EINTR)
			continue;
		return(fd);
	}
}

int xclose(int fd)
{
	int nr;
	while(1) {
		nr = close(fd);
		if((nr < 0) && errno == EINTR)
			continue;
		return(nr);
	}
}

ssize_t xread(int fd, void *buf, size_t len)
{
	ssize_t nr;
	while(1) {
		nr = read(fd, buf, len);
		if ((nr < 0) && (errno == EAGAIN || errno == EINTR))
			continue;
		return(nr);
	}
}

ssize_t xwrite(int fd, const void *buf, size_t len)
{
	ssize_t nr;
	while(1) {
		nr = write(fd, buf, len);
		if ((nr < 0) && (errno == EAGAIN || errno == EINTR))
			continue;
		return(nr);
	}
}

/**
 * Hash the given string to an unsigned long value.
 * This is the standard sdbm hashing algorithm.
 * @param str string to hash
 * @return the hash value of the given string
 */
unsigned long hash_sdbm(const char *str)
{
	unsigned long hash = 0;
	int c;
	while((c = *str++))
		hash = c + (hash << 6) + (hash << 16) - hash;

	return(hash);
}

/* if using ISO C, strdup() is not actually defined, provide our own */
#ifndef strdup
char *strdup(const char *s)
{
	char *ret = NULL;
	if(s)  {
		const size_t len = strlen(s) + 1;
		ret = malloc(len * sizeof(char));
		if(ret) {
			if(len > 16) {
				memcpy(ret, s, len);
			} else {
				strcpy(ret, s);
			}
		}
	}
	return(ret);
}
#endif /* strdup */

/* vim: set ts=4 sw=4 noet: */
