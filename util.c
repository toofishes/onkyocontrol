/*
 *  util.c - Onkyo receiver utility functions
 *
 *  Copyright (c) 2008-2010 Dan McGee <dpmcgee@gmail.com>
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
#include <sys/time.h> /* struct timeval */
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
		return fd;
	}
}

int xclose(int fd)
{
	int nr;
	while(1) {
		nr = close(fd);
		if((nr < 0) && errno == EINTR)
			continue;
		return nr;
	}
}

ssize_t xread(int fd, void *buf, size_t len)
{
	ssize_t nr;
	while(1) {
		nr = read(fd, buf, len);
		if ((nr < 0) && (errno == EAGAIN || errno == EINTR))
			continue;
		return nr;
	}
}

ssize_t xwrite(int fd, const void *buf, size_t len)
{
	ssize_t nr;
	while(1) {
		nr = write(fd, buf, len);
		if ((nr < 0) && (errno == EAGAIN || errno == EINTR))
			continue;
		return nr;
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
	if(!str)
		return hash;
	while((c = *str++))
		hash = c + (hash << 6) + (hash << 16) - hash;

	return hash;
}

void timeval_diff(struct timeval * restrict a,
		struct timeval * restrict b, struct timeval * restrict result)
{
	/* Calculate time difference as `a - b`.
	 * Make sure we end up with an in-range usecs value. */
	result->tv_sec = a->tv_sec - b->tv_sec;
	result->tv_usec = a->tv_usec - b->tv_usec;
	if(result->tv_usec < 0) {
		result->tv_usec += 1000000;
		result->tv_sec -= 1;
	}
}

struct timeval timeval_min(struct timeval *restrict a,
		struct timeval * restrict b)
{
	if(a->tv_sec == 0 && a->tv_usec == 0) {
		return *b;
	} if(a->tv_sec < b->tv_sec) {
		return *a;
	} else if(a->tv_sec > b->tv_sec) {
		return *b;
	}
	/* getting here means seconds are equal */
	return a->tv_usec < b->tv_usec ? *a : *b;
}

/* vim: set ts=4 sw=4 noet: */
