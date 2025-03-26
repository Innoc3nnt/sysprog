#include "userfs.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

enum {
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
	/** Block memory. */
	char *memory;
	/** How many bytes are occupied. */
	int occupied;
	/** Next block in the file. */
	struct block *next;
	/** Previous block in the file. */
	struct block *prev;

	/* PUT HERE OTHER MEMBERS */
};

struct file {
	/** Double-linked list of file blocks. */
	struct block *block_list;
	/**
	 * Last block in the list above for fast access to the end
	 * of file.
	 */
	struct block *last_block;
	/** How many file descriptors are opened on the file. */
	int refs;
	/** File name. */
	char *name;
	/** Files are stored in a double-linked list. */
	struct file *next;
	struct file *prev;
	/** File size. */
	size_t size;

	/* PUT HERE OTHER MEMBERS */
};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc {
	struct file *file;
	/** Current block for read/write operations. */
	struct block *block;
	/** Position in the current block. */
	int pos;
	/** Total file position. */
	size_t offset;
#if NEED_OPEN_FLAGS
	/** Open flags. */
	int flags;
#endif
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static struct filedesc **file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

enum ufs_error_code
ufs_errno()
{
	return ufs_error_code;
}

/**
 * Find a file by its name.
 */
static struct file *
find_file(const char *filename)
{
	struct file *file = file_list;
	while (file != NULL) {
		if (strcmp(file->name, filename) == 0)
			return file;
		file = file->next;
	}
	return NULL;
}

/**
 * Create a new file with the given name.
 */
static struct file *
create_file(const char *filename)
{
	struct file *file = malloc(sizeof(struct file));
	if (file == NULL) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return NULL;
	}
	file->name = strdup(filename);
	if (file->name == NULL) {
		free(file);
		ufs_error_code = UFS_ERR_NO_MEM;
		return NULL;
	}
	file->block_list = NULL;
	file->last_block = NULL;
	file->refs = 0;
	file->next = NULL;
	file->prev = NULL;
	file->size = 0;
	if (file_list == NULL) {
		file_list = file;
	} else {
		file->next = file_list;
		file_list->prev = file;
		file_list = file;
	}
	return file;
}

/**
 * Create a new block.
 */
static struct block *
create_block(void)
{
	struct block *block = malloc(sizeof(struct block));
	if (block == NULL) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return NULL;
	}
	block->memory = malloc(BLOCK_SIZE);
	if (block->memory == NULL) {
		free(block);
		ufs_error_code = UFS_ERR_NO_MEM;
		return NULL;
	}
	memset(block->memory, 0, BLOCK_SIZE);
	block->occupied = 0;
	block->next = NULL;
	block->prev = NULL;
	return block;
}

/**
 * Add a new file descriptor to the array.
 */
static int
add_descriptor(struct filedesc *fd)
{
	if (file_descriptor_count == file_descriptor_capacity) {
		int new_capacity = file_descriptor_capacity == 0 ? 16 : file_descriptor_capacity * 2;
		struct filedesc **new_descriptors = realloc(file_descriptors, 
			new_capacity * sizeof(struct filedesc *));
		if (new_descriptors == NULL) {
			ufs_error_code = UFS_ERR_NO_MEM;
			return -1;
		}
		file_descriptors = new_descriptors;
		file_descriptor_capacity = new_capacity;
		for (int i = file_descriptor_count; i < file_descriptor_capacity; ++i)
			file_descriptors[i] = NULL;
	}
	
	int fd_id = -1;
	for (int i = 0; i < file_descriptor_capacity; ++i) {
		if (file_descriptors[i] == NULL) {
			fd_id = i;
			break;
		}
	}
	
	if (fd_id == -1) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}
	
	file_descriptors[fd_id] = fd;
	++file_descriptor_count;
	return fd_id;
}

/**
 * Get file descriptor by id.
 */
static struct filedesc *
get_descriptor(int fd)
{
	if (fd < 0 || fd >= file_descriptor_capacity || file_descriptors[fd] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return NULL;
	}
	return file_descriptors[fd];
}

int
ufs_open(const char *filename, int flags)
{
	ufs_error_code = UFS_ERR_NO_ERR;
	struct file *file = find_file(filename);
	
	if (file == NULL) {
		if (!(flags & UFS_CREATE)) {
			ufs_error_code = UFS_ERR_NO_FILE;
			return -1;
		}
		file = create_file(filename);
		if (file == NULL) {
			return -1;
        }
	}
	
	struct filedesc *fd = malloc(sizeof(struct filedesc));
	if (fd == NULL) {
		ufs_error_code = UFS_ERR_NO_MEM;
		if (file->refs == 0) {
			if (file->prev)
				file->prev->next = file->next;
			else
				file_list = file->next;
				
			if (file->next)
				file->next->prev = file->prev;
				
			free(file->name);
			free(file);
		}
		return -1;
	}
	
	fd->file = file;
	fd->block = file->block_list;
	fd->pos = 0;
	fd->offset = 0;
#if NEED_OPEN_FLAGS
	fd->flags = flags;
	if ((flags & (UFS_READ_ONLY | UFS_WRITE_ONLY | UFS_READ_WRITE)) == 0) {
		fd->flags |= UFS_READ_WRITE;
	}
#endif
	
	++file->refs;
	int fd_id = add_descriptor(fd);
	if (fd_id == -1) {
		free(fd);
		--file->refs;
		if (file->refs == 0) {
			if (file->prev)
				file->prev->next = file->next;
			else
				file_list = file->next;
				
			if (file->next)
				file->next->prev = file->prev;
				
			free(file->name);
			free(file);
		}
		return -1;
	}
	
	return fd_id;
}

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
    ufs_error_code = UFS_ERR_NO_ERR;
    struct filedesc *file_desc = get_descriptor(fd);
    if (file_desc == NULL) {
        return -1;
    }
        
#if NEED_OPEN_FLAGS
    if ((file_desc->flags & (UFS_WRITE_ONLY | UFS_READ_WRITE)) == 0) {
        ufs_error_code = UFS_ERR_NO_PERMISSION;
        return -1;
    }
#endif

    if (file_desc->offset + size > MAX_FILE_SIZE) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
    }
                
    size_t bytes_written = 0;
    while (bytes_written < size) {
        if (file_desc->block == NULL || file_desc->pos >= BLOCK_SIZE) {
            struct block *new_block = NULL;
            
            if (file_desc->file->block_list == NULL) {
                new_block = create_block();
                if (new_block == NULL) {
                    return bytes_written > 0 ? (ssize_t)bytes_written : -1;
                }
                    
                file_desc->file->block_list = new_block;
                file_desc->file->last_block = new_block;
                file_desc->block = new_block;
                file_desc->pos = 0;
            } else if (file_desc->block == NULL || file_desc->block->next == NULL) {
                new_block = create_block();
                if (new_block == NULL) {
                    return bytes_written > 0 ? (ssize_t)bytes_written : -1;
                }
                
                if (file_desc->block != NULL) {
                    file_desc->block->next = new_block;
                    new_block->prev = file_desc->block;
                } else {
                    file_desc->file->last_block->next = new_block;
                    new_block->prev = file_desc->file->last_block;
                }
                
                file_desc->file->last_block = new_block;
                file_desc->block = new_block;
                file_desc->pos = 0;
            } else {
                file_desc->block = file_desc->block->next;
                file_desc->pos = 0;
            }
        }
        
        size_t to_write = BLOCK_SIZE - file_desc->pos;
        if (to_write > size - bytes_written)
            to_write = size - bytes_written;
            
        memcpy(file_desc->block->memory + file_desc->pos, buf + bytes_written, to_write);
        
        file_desc->pos += (int)to_write;
        if (file_desc->block->occupied < file_desc->pos)
            file_desc->block->occupied = file_desc->pos;
            
        bytes_written += to_write;
        file_desc->offset += to_write;
    }
    
    if (file_desc->offset > file_desc->file->size) {
        file_desc->file->size = file_desc->offset;
    }
    
    return (ssize_t)bytes_written;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{
    ufs_error_code = UFS_ERR_NO_ERR;
    struct filedesc *file_desc = get_descriptor(fd);
    if (file_desc == NULL) {
        return -1;
    }
        
#if NEED_OPEN_FLAGS
    if ((file_desc->flags & (UFS_READ_ONLY | UFS_READ_WRITE)) == 0) {
        ufs_error_code = UFS_ERR_NO_PERMISSION;
        return -1;
    }
#endif
    
    if (file_desc->file->block_list == NULL) {
        return 0;
    }
    
    if (file_desc->block == NULL && file_desc->file->block_list != NULL) {
        file_desc->block = file_desc->file->block_list;
        file_desc->pos = 0;
        file_desc->offset = 0;
    }
        
    size_t bytes_read = 0;
    
    while (bytes_read < size) {
        if (file_desc->block == NULL) {
            break;
        }
            
        size_t to_read = file_desc->block->occupied - file_desc->pos;
                    
        if (to_read == 0) {
            if (file_desc->block->next == NULL) {
                break;
            }
                
            file_desc->block = file_desc->block->next;
            file_desc->pos = 0;
            continue;
        }
        
        if (to_read > size - bytes_read)
            to_read = size - bytes_read;
        
        memcpy(buf + bytes_read, file_desc->block->memory + file_desc->pos, to_read);
        
        file_desc->pos += (int)to_read;
        bytes_read += to_read;
        file_desc->offset += to_read;
        
        if (file_desc->pos >= file_desc->block->occupied) {
            if (file_desc->block->next == NULL) {
                break;
            }
                
            file_desc->block = file_desc->block->next;
            file_desc->pos = 0;
        }
    }
    
    return (ssize_t)bytes_read;
}

/**
 * Check if a file is in the file_list.
 */
static bool
is_file_in_list(struct file *file)
{
    struct file *current = file_list;
    while (current != NULL) {
        if (current == file)
            return true;
        current = current->next;
    }
    return false;
}

int
ufs_close(int fd)
{
	ufs_error_code = UFS_ERR_NO_ERR;
	if (fd < 0 || fd >= file_descriptor_capacity || file_descriptors[fd] == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	
	struct filedesc *file_desc = file_descriptors[fd];
	struct file *file = file_desc->file;
	
	--file->refs;
	
	if (file->refs == 0 && !is_file_in_list(file)) {
		struct block *block = file->block_list;
		while (block != NULL) {
			struct block *next = block->next;
			free(block->memory);
			free(block);
			block = next;
		}
		free(file->name);
		free(file);
	}
	
	free(file_desc);
	file_descriptors[fd] = NULL;
	--file_descriptor_count;
	return 0;
}

int
ufs_delete(const char *filename)
{
	ufs_error_code = UFS_ERR_NO_ERR;
	struct file *file = find_file(filename);
	if (file == NULL) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	
	if (file->prev != NULL)
		file->prev->next = file->next;
	else
		file_list = file->next;
		
	if (file->next != NULL)
		file->next->prev = file->prev;
		
	if (file->refs == 0) {
		struct block *block = file->block_list;
		while (block != NULL) {
			struct block *next = block->next;
			free(block->memory);
			free(block);
			block = next;
		}
		free(file->name);
		free(file);
	}
	
	return 0;
}

#if NEED_RESIZE

static void
find_position(struct filedesc *fd, size_t pos)
{
    if (fd->file->block_list == NULL) {
        fd->block = NULL;
        fd->pos = 0;
        fd->offset = 0;
        return;
    }
    
    fd->block = fd->file->block_list;
    fd->pos = 0;
    fd->offset = 0;
    
    while (fd->block != NULL) {
        if (pos < (size_t)fd->block->occupied) {
            fd->pos = (int)pos;
            fd->offset = pos;
            return;
        }
        
        pos -= fd->block->occupied;
        fd->offset += fd->block->occupied;
        fd->block = fd->block->next;
    }
    
    if (fd->file->last_block != NULL) {
        fd->block = fd->file->last_block;
        fd->pos = fd->block->occupied;
        fd->offset = fd->file->size;
    }
}

int
ufs_resize(int fd, size_t new_size)
{
	ufs_error_code = UFS_ERR_NO_ERR;
	struct filedesc *file_desc = get_descriptor(fd);
	if (file_desc == NULL) {
		return -1;
    }
		
#if NEED_OPEN_FLAGS
	if ((file_desc->flags & (UFS_WRITE_ONLY | UFS_READ_WRITE)) == 0) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
#endif

    if (new_size > MAX_FILE_SIZE) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
    }
	
	size_t current_size = file_desc->file->size;
	
	if (new_size < current_size) {
		size_t remaining = new_size;
		struct block *block = file_desc->file->block_list;
		
		while (block != NULL && remaining > 0) {
			if (remaining >= (size_t)block->occupied) {
				remaining -= block->occupied;
				block = block->next;
			} else {
				block->occupied = (int)remaining;
				remaining = 0;
				break;
			}
		}
		
		if (block != NULL) {
			struct block *next_block = block->next;
			block->next = NULL;
			file_desc->file->last_block = block;
			
			while (next_block != NULL) {
				block = next_block;
				next_block = block->next;
				free(block->memory);
				free(block);
			}
		}
		
		for (int i = 0; i < file_descriptor_capacity; ++i) {
			struct filedesc *fd = file_descriptors[i];
			if (fd != NULL && fd->file == file_desc->file && fd->offset > new_size) {
				find_position(fd, new_size);
			}
		}
	} else if (new_size > current_size) {
		size_t to_add = new_size - current_size;
		
		if (file_desc->file->block_list == NULL) {
			file_desc->file->block_list = create_block();
			if (file_desc->file->block_list == NULL) {
				return -1;
            }
			file_desc->file->last_block = file_desc->file->block_list;
		}
		
		struct block *block = file_desc->file->last_block;
		if (block->occupied < BLOCK_SIZE) {
			size_t available = BLOCK_SIZE - block->occupied;
			size_t fill = available < to_add ? available : to_add;
			memset(block->memory + block->occupied, 0, fill);
			block->occupied += (int)fill;
			to_add -= fill;
		}
		
		while (to_add > 0) {
			struct block *new_block = create_block();
			if (new_block == NULL) {
				return -1;
            }
				
			size_t fill = BLOCK_SIZE < to_add ? BLOCK_SIZE : to_add;
			memset(new_block->memory, 0, fill);
			new_block->occupied = (int)fill;
			to_add -= fill;
			
			block->next = new_block;
			new_block->prev = block;
			block = new_block;
			file_desc->file->last_block = new_block;
		}
	}
	
	file_desc->file->size = new_size;
	
	return 0;
}

#endif

void
ufs_destroy(void)
{
	struct file *file = file_list;
	while (file != NULL) {
		struct file *next_file = file->next;
		struct block *block = file->block_list;
		
		while (block != NULL) {
			struct block *next_block = block->next;
			free(block->memory);
			free(block);
			block = next_block;
		}
		
		free(file->name);
		free(file);
		file = next_file;
	}
	
	for (int i = 0; i < file_descriptor_capacity; ++i) {
		if (file_descriptors[i] != NULL) {
			free(file_descriptors[i]);
			file_descriptors[i] = NULL;
		}
	}
	
	free(file_descriptors);
	file_descriptors = NULL;
	file_descriptor_count = 0;
	file_descriptor_capacity = 0;
	file_list = NULL;
}
