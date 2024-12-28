/**
 * Tony Givargis
 * Copyright (C), 2023
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * scm.c
 */

/**
 * Needs:
 *   fstat()
 *   S_ISREG()
 *   open()
 *   close()
 *   sbrk()
 *   mmap()
 *   munmap()
 *   msync()
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include "scm.h"

#define VIRT_ADDR 0x600000000000

#define INT_SIZE 2 * sizeof(int)
#define METADATA_SIZE 2 * sizeof(size_t)

struct scm {
    int fd;   /* File descriptor */
    void *base; /* begining of mapped memory space */
    void *mapped;   /* Pointer to mapped address wjere the files content are linked */
    size_t capacity; /* size of mapped memory area */
    size_t utilized; 
};


/* Helper function to sync the memory */
void sync_memory(struct scm *scm) {
    if (msync(scm->mapped, scm->capacity, MS_SYNC) == -1) {
        perror("Error while msync");
    }
}


/* Helper function to unmap memory */
void unmap_memory(struct scm *scm) {
    if (scm->mapped != MAP_FAILED) {
        /* Perform memory sync before unmapping */
        sync_memory(scm);
        
        if (munmap(scm->mapped, scm->capacity) == -1) {
            perror("Error while munmap");
        }
    }
}


/* Helper function to close file descriptor */
void close_file(struct scm *scm) {
    if (scm->fd != -1) {
        close(scm->fd);
    }
}

void scm_close(struct scm *scm) {
    /* Return early if scm is NULL, nothing to close */
    if (scm == NULL) return;

    /* First, unmap memory if it was mapped successfully */
    unmap_memory(scm);

    /* Close file descriptor if open */
    close_file(scm);

    /* Free the scm structure */
    free(scm);
}


struct scm *scm_open(const char *pathname, int truncate) {
    struct scm *scm = NULL;
    struct stat st;
    int *size_info;

    /* Allocate memory for the scm structure */
    scm = (struct scm *)malloc(sizeof(struct scm));
    if (!scm) {
        TRACE("Error allocating memory for scm\n");
        return NULL;
    }

    /* Open the file in read-write mode */
    scm->fd = open(pathname, O_RDWR);
    if (scm->fd == -1) {
        TRACE("Error opening file descriptor\n");
        free(scm);  /* Free the scm structure */
        return NULL;
    }

    /* Get file information to check its size */
    if (fstat(scm->fd, &st) == -1) {
        TRACE("Error with fstat\n");
        close(scm->fd);
        free(scm);
        return NULL;
    }

    /* Ensure that the file is a regular file */
    if (!S_ISREG(st.st_mode)) {
        TRACE("The file is not a regular file\n");
        close(scm->fd);
        free(scm);
        return NULL;
    }

    /* Set the capacity of the scm based on the file size */
    scm->capacity = st.st_size;
    scm->utilized = 0;

    /* Map the file into memory */
    scm->mapped = mmap((void *)VIRT_ADDR, scm->capacity, 
                       PROT_EXEC | PROT_READ | PROT_WRITE, 
                       MAP_FIXED | MAP_SHARED, scm->fd, 0);
    if (scm->mapped == MAP_FAILED) {
        printf("Error mapping file into memory\n");
        printf("File size: %d\n", (int)scm->capacity);
        free(scm);  /* Free the scm structure */
        return NULL;
    }

    /* Set the base address, skipping the first INT_SIZE bytes */
    scm->base = (void *)((char *)scm->mapped + INT_SIZE);

    /* Read size information from the mapped area */
    size_info = (int *)scm->mapped;

    /* Handle truncation and size initialization */
    if ((size_info[0] != 1) || truncate) {
        size_info[0] = 1;
        size_info[1] = 0;
        scm->utilized = 0;
    } else {
        scm->utilized = size_info[1];
    }

    /* Close the file descriptor after mapping */
    close(scm->fd);

    return scm;
}


void *scm_malloc(struct scm *scm, size_t size) {
    size_t *block_meta;
    void *allocated_addr;
    int *scm_meta;

    /* Validate input parameters and state of the scm */
    if (!scm || scm->base == MAP_FAILED || size == 0) {
        return NULL;
    }

    /* Check if there is enough space for the requested memory + metadata */
    if (scm->utilized + size + METADATA_SIZE > scm->capacity) {
        printf("SCM Utilized: %d\n", (int)scm->utilized);
        printf("SCM Capacity: %d\n", (int)scm->capacity);
        printf("Requested Size: %d\n", (int)size);
        perror("Capacity exceeded");
        return NULL;
    }

    /* Set up the metadata for the allocated block */
    block_meta = (size_t *)((char *)scm->base + scm->utilized);
    block_meta[0] = 1;       /* Mark this block as in use */
    block_meta[1] = size;    /* Store the size of the requested allocation */

    /* Update the utilized space in the scm structure */
    scm->utilized += size + METADATA_SIZE;

    /* Update the total utilized memory metadata in the mapped region */
    scm_meta = (int *)scm->mapped;
    scm_meta[1] = scm->utilized;

    /* Return the address of the allocated memory (skipping metadata) */
    allocated_addr = (void *)((char *)block_meta + METADATA_SIZE);
    return allocated_addr;
}


char *scm_strdup(struct scm *scm, const char *input_str) {
    char *duplicated_str;
    int *scm_metadata;
    size_t *allocation_metadata = (size_t *)((char *) scm->base + scm->utilized);

    /* Validate input parameters and check for invalid states */
    if (input_str == NULL || scm == NULL || scm->base == MAP_FAILED) {
        return NULL;
    }

    /* Set the metadata for the new string allocation */
    allocation_metadata[0] = 1; /* Mark this block as in use */
    allocation_metadata[1] = strlen(input_str) + 1; /* Store the length of the string + null terminator */

    /* Get the memory location where the string will be copied */
    duplicated_str = (char *) allocation_metadata + METADATA_SIZE;

    /* Copy the input string to the allocated space */
    strcpy(duplicated_str, input_str);

    /* Update the utilized space in the scm structure */
    scm->utilized += allocation_metadata[1] + METADATA_SIZE;

    /* Update the total utilized memory metadata in the mapped region */
    scm_metadata = (int *) scm->mapped;
    scm_metadata[1] = scm->utilized;

    /* Return the pointer to the duplicated string */
    return duplicated_str;
}


void scm_free(struct scm *scm, void *memory_ptr) {
    size_t *block_metadata;
    void *current_address;
    void *base_address;
    int block_found = 0;

    /* Get the base address of the allocated memory */
    current_address = base_address = scm_mbase(scm);

    /* Traverse the allocated memory blocks to find the matching address */
    while (!block_found && (current_address < (void *)((char *)base_address + scm->utilized))) {
        if (current_address == memory_ptr) {
            block_found = 1;
            break;
        }
        /* Move to the next block by using its metadata size */
        current_address = (void *)((char *)current_address + ((size_t *)current_address - 2)[1] + METADATA_SIZE);
    }

    /* If the memory block is not found, return */
    if (!block_found) return;

    /* Mark the block as free by updating its metadata */
    block_metadata = (size_t *) memory_ptr - 2;
    block_metadata[0] = 0; /* Set the first metadata to 0 indicating free */
}

/* Return the amount of memory used in the scm structure */
size_t scm_utilized(const struct scm *scm) {
    if (scm == NULL) {
        return 0;
    }
    return scm->utilized;
}

/* Return the total capacity of the scm structure */
size_t scm_capacity(const struct scm *scm) {
    if (scm == NULL) {
        return 0;
    }
    return scm->capacity;
}


void *scm_mbase(struct scm *scm) {
    if(scm == NULL) {
        return NULL;
    }
    
    if(0 != scm->utilized)
        return (void *)((size_t *) scm->base + 2);
    
    return scm->base;
}