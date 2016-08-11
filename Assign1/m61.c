#define M61_DISABLE     1
#define ALIGN_SZ        8    // assignment seems suggest to use size 8 align
#define HHITTER_ARR_SZ  5    // size of heavy hitter tracking array
#include "m61.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

// global stats struct
static struct m61_statistics gstats = {
    0, 0, 0, 0, 0, 0, NULL, NULL
};

// arrays tracking, heavist and most frequent allocators
m61_hhitter heavy_alloc[HHITTER_ARR_SZ] = {
    {0, 0, NULL}
};
m61_hhitter heavy_freq[HHITTER_ARR_SZ] = {
    {0, 0, NULL}
};

int alloc_neg_bias = 0; // negative bias for FREQUENT algorithm
int freq_neg_bias = 0;  // negative bias for FREQUENT algorithm
char* first_alloc = NULL; // first allocated address
char* curr_alloc = NULL;  // last allocated address

/** @brief Allocates @sz bytes of memory and return a pointer to the 
    dynamically allocated memory. @file and @line refer to the filename
    and line in the code where malloc is called. */
void* m61_malloc(size_t sz, const char* file, int line) {
    (void) file, (void) line;   // avoid uninitialized variable warnings
    
    void* new_ptr = NULL;    // default ptr to NULL
    
    // align size of malloced block, (including boundary check element)
    size_t aligned_sz = sz + sizeof(uintptr_t);
    if (sz % ALIGN_SZ != 0) {
        aligned_sz = sz + (ALIGN_SZ - (sz % ALIGN_SZ));
    }
    
    if ((sz < (SIZE_MAX - sizeof(m61_mdata))) &&  
        (new_ptr = malloc(aligned_sz + sizeof(m61_mdata))) != NULL) {

        ++gstats.ntotal;
        ++gstats.nactive;
        gstats.total_size += sz;  // sz cnt doesn't include metadata or padding
        gstats.active_size += sz;
        
        if (first_alloc == NULL) {
            first_alloc = new_ptr; // allocs linked via ptrs to metadata
        }
        // setting metadata header
        m61_mdata meta_d = {sz, line, file, curr_alloc, NULL,
            (uintptr_t)(new_ptr + sizeof(m61_mdata))};
        memcpy(new_ptr, &meta_d, sizeof(m61_mdata));
        // set previous node's next_addr, if not at start of list
        if (curr_alloc != NULL) {
            m61_mdata *prev_m_data = (m61_mdata *)(curr_alloc);
            prev_m_data->next_addr = new_ptr;
        }
        curr_alloc = new_ptr;
        new_ptr = new_ptr + sizeof(m61_mdata); // set return ptr to payld addr
        
        // inserting end boundary check
        uintptr_t bcheck = (uintptr_t)(new_ptr);
        memcpy(new_ptr + sz, &bcheck, sizeof(uintptr_t));       
        
        // adding to statistics 
        if (gstats.heap_min == NULL 
            || gstats.heap_min > (char *)(new_ptr - sizeof(m61_mdata))) {
            gstats.heap_min = (char *)(new_ptr - sizeof(m61_mdata));
        } 
        if (gstats.heap_max == NULL || 
            gstats.heap_max < (char *)(new_ptr + sz)) {
            gstats.heap_max = (char *)(new_ptr + sz);
        }

        // track heaviest byte allocators, sz is passed as byte amount
        trackheavyhitters(heavy_alloc, &alloc_neg_bias, sz, file, line);
        // track most frequent allocators, sz is passed as 1,(a single alloc)
        trackheavyhitters(heavy_freq, &freq_neg_bias, 1, file, line);
    } else {
        // allocation failed
        ++gstats.nfail;
        gstats.fail_size += sz;
    }
    return new_ptr;
}

/** @brief Frees a single block of memory previously allocated by malloc,
    at address pointed to by @ptr. @file and @line refer to the filename
    and line in the code where free is called. */
void m61_free(void *ptr, const char *file, int line) {
    (void) file, (void) line;   // avoid uninitialized variable warnings
    // no need to free if already NULL
    if (ptr != NULL) {
        // validate that free is ok to do
        validateinheap(ptr, file, line);
        validateisallocated(ptr, file, line);
        validateboundarycheck(ptr, file, line);
        
        // All validation passed successfully
        m61_mdata *meta_d = (m61_mdata *)(ptr - sizeof(m61_mdata));
        
        // adjust stats accounting for free
        --gstats.nactive;
        gstats.active_size -= meta_d->payload_size;

        // Block is marked as freed by setting metadata payload_addr to 0
        meta_d->payload_addr = 0;
        
        // set next allocated block to point to previous block
        m61_mdata *prev_meta_d = (m61_mdata *)meta_d->prev_addr;
        m61_mdata *next_meta_d = (m61_mdata *)meta_d->next_addr;
        if (prev_meta_d != NULL) {
            prev_meta_d->next_addr = meta_d->next_addr;
        } else {
            first_alloc = meta_d->next_addr;
        }
        
        if (next_meta_d != NULL) {
            next_meta_d->prev_addr = meta_d->prev_addr;
        } else {
            curr_alloc = meta_d->prev_addr;
        }
        
        free(ptr - sizeof(m61_mdata));
    }
}

/** @brief validates whether @ptr is actually in heap. @file and @line refer to
    the filename and line in the code where free is called. */
static void validateinheap(void* ptr, const char* file, int line) {
    // check for trying to free ptr outside heap
    if ((uintptr_t)ptr < (uintptr_t)(gstats.heap_min - sizeof(m61_mdata)) 
        || (uintptr_t)ptr > (uintptr_t)gstats.heap_max) {
        printf("MEMORY BUG: %s:%d: invalid free of pointer %p, not in heap\n",
            file, line, ptr);
        abort();
    }
}

/** @brief validates whether @ptr is allocated. @file and @line refer to
    the filename and line in the code where free is called. */
static void validateisallocated(void* ptr, const char* file, int line) {
    int valid = 0;
    m61_mdata *itr_m_data = (m61_mdata *)first_alloc;
    while (itr_m_data != NULL) {
        if ((uintptr_t)itr_m_data == (uintptr_t)(ptr - sizeof(m61_mdata))) {
            // found pointer in list, so valid
            valid = 1;
            break;
        } else if (((uintptr_t)ptr > (uintptr_t)itr_m_data) &&
            (uintptr_t)ptr < (uintptr_t)(itr_m_data->payload_addr 
                + itr_m_data->payload_size)) {
            // requested ptr is inside an allocated block
            printf("MEMORY BUG: %s:%d: invalid free of pointer %p, not allocated\n",
                file, line, ptr);
            int byt_inside = (uintptr_t)ptr - (uintptr_t)(itr_m_data->payload_addr);
            printf("  %s:%d: %p is %d bytes inside a %d byte region allocated here\n",
                itr_m_data->filename, itr_m_data->line_num, ptr, 
                byt_inside, itr_m_data->payload_size);
            abort();
        }        
        itr_m_data = (m61_mdata *)itr_m_data->next_addr;
    }
    
    if (!valid) {
        printf("MEMORY BUG: %s:%d: invalid free of pointer %p, not allocated\n",
                file, line, ptr);
        abort();
    }
}

/** @brief checks for wild writes after payload by validating boundary check
    element has not been altered. @ptr is addr of freed block. @file and @line 
    refer to the filename and line in the code where free is called. */
static void validateboundarycheck(void* ptr, const char* file, int line) {
    m61_mdata *meta_d = (m61_mdata *)(ptr - sizeof(m61_mdata));
    uintptr_t bcheck_addr;
    memcpy(&bcheck_addr, ptr + meta_d->payload_size, sizeof(uintptr_t));

    if ((uintptr_t)ptr != bcheck_addr) {
        // ptr does not match boundary write validation check
        printf("MEMORY BUG: %s:%d: detected wild write during free of pointer %p\n",
        file, line, ptr);
        abort();
    }
}

/** @brief allocates, frees, or resizes memory depending on its arguments.
    If @sz is specified, the space is malloc of that size.
    If @ptr is specified, that space is freed. 
    If both are specified, then, data in @ptr is resized into new ptr.
    @file and @line refer to the filename and line in the code where realloc
    is called.*/
void* m61_realloc(void* ptr, size_t sz, const char* file, int line) {
    void* new_ptr = NULL;
    if (sz) {
        new_ptr = m61_malloc(sz, file, line);
    }
    if (ptr && new_ptr) {
        // Copy the data from `ptr` into `new_ptr`.
        m61_mdata *meta_d = (m61_mdata *)(ptr - sizeof(m61_mdata));
        if (meta_d->payload_size < sz) {
            memcpy(new_ptr, ptr, meta_d->payload_size);
        } else {
            memcpy(new_ptr, ptr, sz);
        }
    }
    m61_free(ptr, file, line);
    return new_ptr;
}

/** @brief allocates memory of size @nmemb * @sz and clears it to zero. 
    @file and @line refer to the filename and line in the code where calloc
    is called.*/
void* m61_calloc(size_t nmemb, size_t sz, const char* file, int line) {   
    void* ptr = NULL;
    if ((SIZE_MAX / nmemb) > sz) {
        ptr = m61_malloc(nmemb * sz, file, line);
        if (ptr) {
            memset(ptr, 0, nmemb * sz);
        }
    } else {
        ++gstats.nfail;
    }
    return ptr;
}

/** @brief populates @stats with the statistics about allocations. */
void m61_getstatistics(struct m61_statistics* stats) {
    *stats = gstats;
}

/** @brief prints statistics about allocations. */
void m61_printstatistics(void) {
    struct m61_statistics stats;
    m61_getstatistics(&stats);

    printf("malloc count: active %10llu   total %10llu   fail %10llu\n",
           stats.nactive, stats.ntotal, stats.nfail);
    printf("malloc size:  active %10llu   total %10llu   fail %10llu\n",
           stats.active_size, stats.total_size, stats.fail_size);
}

/** @brief prints details about any allocations that have not been freed. */
void m61_printleakreport(void) {
    // iterating through the allocated ptr list
    m61_mdata *itr_m_data = (m61_mdata *)first_alloc;
    while (itr_m_data != NULL) {
        printf("LEAK CHECK: %s:%d: allocated object %p with size %d\n",
            itr_m_data->filename, itr_m_data->line_num, 
            (void *)itr_m_data->payload_addr, itr_m_data->payload_size);
        itr_m_data = (m61_mdata *)itr_m_data->next_addr;
    }
}

/** @brief prints details about which allocations that are the heaviest, and 
    which are the most frequent. */
void m61_printheavyhitters(void) {
    // sort tracking arrays in decesending order
    sortheavyhitters(heavy_alloc);
    sortheavyhitters(heavy_freq);

    float largest_pcent = ((float)heavy_alloc[0].total_pload_size
                / gstats.total_size) * 100;
    if (largest_pcent > 20) {
        printf("\nMOST ALLOCATED BYTES:\n");
        for (int i = 0; i < HHITTER_ARR_SZ; i++) {
            float tot_percent = ((float)heavy_alloc[i].total_pload_size
                / gstats.total_size) * 100;
            // only printing if an element is higher than 20%
            if (tot_percent > 10) {
            printf("HEAVY HITTER: %s:%d %llu bytes (~%.1f%%)\n",
                heavy_alloc[i].filename, heavy_alloc[i].line_num, 
                heavy_alloc[i].total_pload_size, tot_percent);
            }
        }
    }
    largest_pcent = ((float)heavy_freq[0].total_pload_size
        / gstats.ntotal) * 100;
    if (largest_pcent > 20) {
        printf("\nMOST FREQUNTLY ALLOCATED:\n");
        for (int i = 0; i < HHITTER_ARR_SZ; i++) {
            float tot_percent = ((float)heavy_freq[i].total_pload_size
                / gstats.ntotal) * 100;
            // only printing if an element is higher than 20%
            if (tot_percent > 10) {
            printf("HEAVY FREQ: %s:%d allocated %llu times (~%.1f%%)\n", 
                heavy_freq[i].filename, heavy_freq[i].line_num, 
                heavy_freq[i].total_pload_size, tot_percent);
            }
        } 
    }
}

/** @brief sorts @hhitters array in descending order. */
static void sortheavyhitters(m61_hhitter* hhitters) {
    // sort output in descending order
    m61_hhitter temp;
    for (int i = 0; i < HHITTER_ARR_SZ; i++) {
        for (int j = i; j < HHITTER_ARR_SZ; j++) {
            if (hhitters[i].total_pload_size < hhitters[j].total_pload_size) {
                temp = hhitters[i];
                hhitters[i] = hhitters[j];
                hhitters[j] = temp;
            }
        }
    }
}

/** @brief adds to heavy hitter tracking array based on the "FREQUENT"
    algorithm. @hhitters is array being tracked. @neg_bias is overall
    negative bias for the algo.    @sz, @file and @line refer to the size of the
    malloc, the filenameand line in the code where the malloc is called.*/
static void trackheavyhitters(m61_hhitter* hhitters, int* neg_bias, size_t sz,
    const char* file, int line) {
    int empty_c = -1;
    int added_existing = 0;
  
    for (int i = 0; i < HHITTER_ARR_SZ; i++) {
        /* Using variation on FREQUENT algorithm. Instead of decrementing all
        elements of the array the bytes are added to the "negative bias", 
        which is then considered at the if check against zero */
        if (hhitters[i].total_pload_size - *neg_bias <= 0) {
            if (empty_c == -1) {
                empty_c = i;
            }
        }
        // if found element, then add sz to that element
        if (hhitters[i].line_num == line 
            && strcmp(file, hhitters[i].filename) == 0) {
            hhitters[i].total_pload_size += sz;
            added_existing = 1;
        }
    }
   
    // if found empty element, then add sz to that element
    if (added_existing != 1) {
        if (empty_c != -1) {
            hhitters[empty_c].total_pload_size = sz;
            hhitters[empty_c].line_num = line;
            hhitters[empty_c].filename = file;
        } else {
            // else add sz to the overall negative bias
            *neg_bias += sz;
        }
    }
}
