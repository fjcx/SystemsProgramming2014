#include "io61.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <limits.h>
#include <errno.h>

// io61.c
#define SBUF_SZ 4096          // buffer slot size

// io61_file
//    Data structure for io61 file wrappers. Add your own stuff.
struct io61_file {
    int fd;                   // file descriptor
    int mode;                 // mode file is opened in
};

typedef struct cache {
    char cbuf[SBUF_SZ];
    off_t localoffset;        // local offset from file offset
    int fileoffsetmin;        // File offset, min
    int fileoffsetmax;        // File offset, max
} cachebuf;

cachebuf readbuf;             // read buffer (for non-mappable files)
cachebuf writebuf;            // output buffer

typedef struct memmap memmap;
struct memmap {
    char* mapped_file;        // mmap of file
    off_t filesize;           // filesize of mapped file
    off_t localoffset;        // offset within mmap
    int fd;                   // file descriptor of mapped file
    memmap* next;             // next mmap node in list
    memmap* prev;             // previous mmap node in list
};

// creating a list of memmaps, for case of multiple mapped files
// mmap list operations - start
memmap* mmaphead;              // head of mmap list

void insert_first(memmap* n) {
    n->next = mmaphead;
    n->prev = NULL;
    if (mmaphead)
        mmaphead->prev = n;
    mmaphead = n;
}

void remove_mmap(memmap* n) {
    if (n->next)
        n->next->prev = n->prev;
    if (n->prev)
        n->prev->next = n->next;
    else
        mmaphead = n->next;
}

memmap* search_in_list(int fd) {
    memmap *ptr = mmaphead;
    while(ptr != NULL) {
        if(ptr->fd == fd) {
            return ptr;
        }
        ptr = ptr->next;
    }
    return NULL;
}
// mmap list operations - end


// io61_fdopen(fd, mode)
//    Return a new io61_file that reads from and/or writes to the given
//    file descriptor `fd`. `mode` is either O_RDONLY for a read-only file
//    or O_WRONLY for a write-only file. You need not support read/write
//    files.
io61_file* io61_fdopen(int fd, int mode) {
    assert(fd >= 0);
    io61_file* f = (io61_file*) malloc(sizeof(io61_file));
    f->fd = fd;
    f->mode = mode;
    (void) mode;
    
    // init file for reading
    if (mode == O_RDONLY) {
        // check is file mappable, by looking at filesize (-1 if pipe)
        //memmap newmmap = {NULL, 0, 0, 0, NULL, NULL};
        memmap *newmmap = (memmap*)malloc(sizeof(memmap));
        newmmap->filesize = io61_filesize(f);
        if (newmmap->filesize != -1) {
            size_t fsize = (size_t)newmmap->filesize;
            // map file using mmap
            newmmap->mapped_file = mmap(NULL, fsize, PROT_READ, 
                MAP_SHARED, fd, 0);
            newmmap->localoffset = 0;
            newmmap->fd = fd;
            if (newmmap->mapped_file == MAP_FAILED) {
                perror("mmap");
                exit(1);
            }
            insert_first(newmmap);
        } else {
            // for pipe init the readbuffer
            readbuf.localoffset = 0;
            readbuf.fileoffsetmin = 0;
            readbuf.fileoffsetmax = -1;
        }
    } else {
        // init file for writing
        writebuf.localoffset = 0;
    }
    return f;
}


// io61_close(f)
//    Close the io61_file `f` and release all its resources, including
//    any buffers.
int io61_close(io61_file* f) {
    // only flush writable file...
    if (f->mode == O_WRONLY) {
        io61_flush(f);
    }
    // if file was using mmap, unmap it's accompanying mapping
    if (mmaphead != NULL) {
        memmap* mptr = search_in_list(f->fd);
        munmap(mptr->mapped_file, mptr->filesize);
        //remove from list
        remove_mmap(mptr);
    }
    
    int r = close(f->fd);
    free(f);
    return r;
}

// io61_readc(f)
//    Read a single (unsigned) character from `f` and return it. Returns EOF
//    (which is -1) on error or end-of-file.
int io61_readc(io61_file* f) {
    if (mmaphead == NULL) {        // non-mapped files
        off_t bytes_read = -1;
        if ((readbuf.fileoffsetmin + readbuf.localoffset) 
                >= readbuf.fileoffsetmax) {
            // if not in buffer, then read next chunk into the buffer
            bytes_read = io61_read(f, readbuf.cbuf, SBUF_SZ);
            readbuf.fileoffsetmin = readbuf.fileoffsetmin + readbuf.localoffset;
            readbuf.fileoffsetmax = readbuf.fileoffsetmin + bytes_read;
            readbuf.localoffset = 0;
        }
        
        if (bytes_read != 0) {
            unsigned char bufch[1];
             bufch[0] = readbuf.cbuf[readbuf.localoffset];
            ++readbuf.localoffset;
            // read char from buffer and increament the localoffset
            return bufch[0];
        } else {
            // attempt to read into the buffer, but reached EOF instead
            return EOF;
        }
    } else {    // read char in mapped file, just take char from mmap
		memmap* mptr = search_in_list(f->fd);
		if (mptr->localoffset < mptr->filesize) {
			unsigned char bufch[1];
			memcpy(bufch, &(mptr->mapped_file[mptr->localoffset]), 1);
			mptr->localoffset += 1;
			return bufch[0];
		} else {
			return EOF;
		}
    }
    return -1;    // something went wrong while attemping read
}


// io61_read(f, buf, sz)
//    Read up to `sz` characters from `f` into `buf`. Returns the number of
//    characters read on success; normally this is `sz`. Returns a short
//    count if the file ended before `sz` characters could be read. Returns
//    -1 an error occurred before any characters were read.
ssize_t io61_read(io61_file* f, char* buf, size_t sz) {
    ssize_t nread = 0;
    if (mmaphead == NULL) {    // reading for non-mapped file
        ssize_t nread = read(f->fd, buf, sz);
        if (nread == -1) {
            perror("read");
            exit(1);
        }
        if (nread != 0 || sz == 0 || io61_eof(f)) {
            return nread;
        }
    } else {    // read from mapped file
		memmap* mptr = search_in_list(f->fd);
		if ((mptr->localoffset + 
			(ssize_t)sz) <= mptr->filesize) {
			nread = sz;
		} else {
			// read everything left in file, if sz is not available
			nread = mptr->filesize - mptr->localoffset;
		}
		memcpy(buf, &(mptr->mapped_file[mptr->localoffset]), nread);
		mptr->localoffset += nread;
		return nread;
    }
    return -1;    // an error was encountered during read
}


// io61_writec(f)
//    Write a single character `ch` to `f`. Returns 0 on success or
//    -1 on error.
int io61_writec(io61_file* f, int ch) {
    int ret = 0;
    // if write buffer is not full just append to buffer
    if (writebuf.localoffset < SBUF_SZ) {
        writebuf.cbuf[writebuf.localoffset] = ch;
        ++writebuf.localoffset;
    }
    
    // if write buffer is full then write it to the file
    if (writebuf.localoffset == SBUF_SZ) {
        ret = io61_write(f, writebuf.cbuf, writebuf.localoffset);
        writebuf.localoffset = 0;
    }
    return ret;
}


// io61_write(f, buf, sz)
//    Write `sz` characters from `buf` to `f`. Returns the number of
//    characters written on success; normally this is `sz`. Returns -1 if
//    an error occurred before any characters were written.
ssize_t io61_write(io61_file* f, const char* buf, size_t sz) {
    size_t nwritten = 0;
    while (nwritten < sz) {
        ssize_t r = write(f->fd, buf, sz);
        if (r == -1) {
            perror("write");
            exit(1);
        } else {
            nwritten += r;
            if (r != (ssize_t) sz) {    // didn't write enough
                perror("write");
                exit(1);
            }
        }
    }

    if (nwritten != 0 || sz == 0) {
        return nwritten;
    } else {
        return -1;
    }
}


// io61_flush(f)
//    Forces a write of all buffered data written to `f`.
//    If `f` was opened read-only, io61_flush(f) may either drop all
//    data buffered for reading, or do nothing.
int io61_flush(io61_file* f) {
    (void) f;
    int ret = 0;
    // if anything left in the write buffer, then flush it out
    ret = io61_write(f, writebuf.cbuf, writebuf.localoffset);
    writebuf.localoffset = 0;
    return ret;
}


// io61_seek(f, pos)
//    Change the file pointer for file `f` to `pos` bytes into the file.
//    Returns 0 on success and -1 on failure.
int io61_seek(io61_file* f, off_t pos) {
    if (f->mode == O_RDONLY) {        // do seek in readable file
        if (mmaphead == NULL) {       // seeking in non-mappable file
            if (pos >= readbuf.fileoffsetmin && pos <= readbuf.fileoffsetmax) {
                // found pos in buffer, so move localoffset instead of lseek
                readbuf.localoffset = pos - readbuf.fileoffsetmin;
                return 0;
            } else {
                readbuf.fileoffsetmin = pos - (pos%SBUF_SZ);
                readbuf.localoffset = pos - readbuf.fileoffsetmin;
                readbuf.fileoffsetmax = -1;
                // not in buffer, so do lseek, and update read cache
                off_t r = lseek(f->fd, (off_t) readbuf.fileoffsetmin, SEEK_SET);
                if (r == (off_t) readbuf.fileoffsetmin) {
                    off_t bytes_read = io61_read(f, readbuf.cbuf, SBUF_SZ);
                    readbuf.fileoffsetmax = readbuf.fileoffsetmin + bytes_read;
                    return 0;
                }
            }
        } else {    // seeking in a mapped file, just return mapped pos
			memmap* mptr = search_in_list(f->fd);
			if (pos < mptr->filesize) {
				mptr->localoffset = pos;
				return 0;
			} else {
				return -1;
			}
        }
    } else {            // do seek in writable file
        io61_flush(f);  // flush write buffer before moving elsewhere
        off_t r = lseek(f->fd, (off_t) pos, SEEK_SET);
        if (r == (off_t) pos) {
            return 0;
        }
    }
    return -1;    // pos not found as intended
}


// You shouldn't need to change these functions.

// io61_open_check(filename, mode)
//    Open the file corresponding to `filename` and return its io61_file.
//    If `filename == NULL`, returns either the standard input or the
//    standard output, depending on `mode`. Exits with an error message if
//    `filename != NULL` and the named file cannot be opened.
io61_file* io61_open_check(const char* filename, int mode) {
    int fd;
    if (filename) {
        fd = open(filename, mode, 0666);
    } else if ((mode & O_ACCMODE) == O_RDONLY) {
        fd = STDIN_FILENO;
    } else {
        fd = STDOUT_FILENO;
    }
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", filename, strerror(errno));
        exit(1);
    }
    return io61_fdopen(fd, mode & O_ACCMODE);
}


// io61_filesize(f)
//    Return the size of `f` in bytes. Returns -1 if `f` does not have a
//    well-defined size (for instance, if it is a pipe).
off_t io61_filesize(io61_file* f) {
    struct stat s;
    int r = fstat(f->fd, &s);
    if (r >= 0 && S_ISREG(s.st_mode)) {
        return s.st_size;
    } else {
        return -1;
    }
}


// io61_eof(f)
//    Test if readable file `f` is at end-of-file. Should only be called
//    immediately after a `read` call that returned 0 or -1.
int io61_eof(io61_file* f) {
    char x;
    ssize_t nread = read(f->fd, &x, 1);
    if (nread == 1) {
        fprintf(stderr, "Error: io61_eof called improperly\n\
  (Only call immediately after a read() that returned 0 or -1.)\n");
        abort();
    }
    return nread == 0;
}