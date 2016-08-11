#ifndef M61_H
#define M61_H 1
#include <stdlib.h>
#include <inttypes.h>

void* m61_malloc(size_t sz, const char* file, int line);
void m61_free(void* ptr, const char* file, int line);
void* m61_realloc(void* ptr, size_t sz, const char* file, int line);
void* m61_calloc(size_t nmemb, size_t sz, const char* file, int line);

// struct for statics of alloction
struct m61_statistics {
    unsigned long long nactive;         // # active allocations
    unsigned long long active_size;     // # bytes in active allocations
    unsigned long long ntotal;          // # total allocations
    unsigned long long total_size;      // # bytes in total allocations
    unsigned long long nfail;           // # failed allocation attempts
    unsigned long long fail_size;       // # bytes in failed alloc attempts
    char* heap_min;                     // smallest allocated addr
    char* heap_max;                     // largest allocated addr
};

// struct for metadata of allocated block
typedef struct m61_metadata {
    unsigned int payload_size;          // # bytes in payload
    unsigned int line_num;              // line num
    const char* filename;               // ptr to filename
    char* prev_addr;                    // previous allocated block
    char* next_addr;                    // next allocated block
    uintptr_t payload_addr;             // ptr to payload as int
} m61_mdata;

// struct for elements in hhitter tracking array
typedef struct m61_heavyhitter {
    long long total_pload_size;         // # bytes in payload
    int line_num;                       // line num
    const char* filename;               // ptr to filename
} m61_hhitter;

void m61_getstatistics(struct m61_statistics* stats);
void m61_printstatistics(void);
void m61_printleakreport(void);
void m61_printheavyhitters(void);
static void validateinheap(void* ptr, const char* file, int line);
static void validateisallocated(void* ptr, const char* file, int line);
static void validateboundarycheck(void* ptr, const char* file, int line);
static void sortheavyhitters(m61_hhitter* hhitters);
static void trackheavyhitters(m61_hhitter* hhitters, int* neg_bias, 
    size_t sz, const char* file, int line);

#if !M61_DISABLE
#define malloc(sz)              m61_malloc((sz), __FILE__, __LINE__)
#define free(ptr)               m61_free((ptr), __FILE__, __LINE__)
#define realloc(ptr, sz)        m61_realloc((ptr), (sz), __FILE__, __LINE__)
#define calloc(nmemb, sz)       m61_calloc((nmemb), (sz), __FILE__, __LINE__)
#endif

#endif