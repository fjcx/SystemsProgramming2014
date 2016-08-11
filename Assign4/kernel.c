#include "kernel.h"
#include "lib.h"

// kernel.c
//
//    This is the kernel.


// INITIAL PHYSICAL MEMORY LAYOUT
//
//  +-------------- Base Memory --------------+
//  v                                         v
// +-----+--------------------+----------------+--------------------+---------/
// |     | Kernel      Kernel |       :    I/O | App 1        App 1 | App 2
// |     | Code + Data  Stack |  ...  : Memory | Code + Data  Stack | Code ...
// +-----+--------------------+----------------+--------------------+---------/
// 0  0x40000              0x80000 0xA0000 0x100000             0x140000
//                                             ^
//                                             | \___ PROC_SIZE ___/
//                                      PROC_START_ADDR

#define PROC_SIZE 0x40000       // initial state only

static proc processes[NPROC];   // array of process descriptors
                                // Note that `processes[0]` is never used.
proc* current;                  // pointer to currently executing proc

#define HZ 100                  // timer interrupt frequency (interrupts/sec)
static unsigned ticks;          // # timer interrupts so far

void schedule(void);
void run(proc* p) __attribute__((noreturn));


// PAGEINFO
//
//    The pageinfo[] array keeps track of information about each physical page.
//    There is one entry per physical page.
//    `pageinfo[pn]` holds the information for physical page number `pn`.
//    You can get a physical page number from a physical address `pa` using
//    `PAGENUMBER(pa)`. (This also works for page table entries.)
//    To change a physical page number `pn` into a physical address, use
//    `PAGEADDRESS(pn)`.
//
//    pageinfo[pn].refcount is the number of times physical page `pn` is
//      currently referenced. 0 means it's free.
//    pageinfo[pn].owner is a constant indicating who owns the page.
//      PO_KERNEL means the kernel, PO_RESERVED means reserved memory (such
//      as the console), and a number >=0 means that process ID.
//
//    pageinfo_init() sets up the initial pageinfo[] state.

typedef struct physical_pageinfo {
    int8_t owner;
    int8_t refcount;
} physical_pageinfo;

static physical_pageinfo pageinfo[PAGENUMBER(MEMSIZE_PHYSICAL)];

typedef enum pageowner {
    PO_FREE = 0,                // this page is free
    PO_RESERVED = -1,           // this page is reserved memory
    PO_KERNEL = -2              // this page is used by the kernel
} pageowner_t;

static void pageinfo_init(void);


// Memory functions
void virtual_memory_check(void);
void memshow_physical(void);
void memshow_virtual(x86_pagetable* pagetable, const char* name);
void memshow_virtual_animate(void);

static void process_setup(pid_t pid, int program_number);
static pid_t find_free_process_slot(void);
static void* alloc_free_page(int8_t owner);
static void* find_free_physical_page(void);
static x86_pagetable *copy_pagetable(x86_pagetable* pagetable, int8_t owner);
static pid_t find_page_owner(int pagenum);
static void free_current_process(proc *p);
static int fork_process(void);


// kernel(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.
void kernel(const char* command) {
    hardware_init();
    pageinfo_init();
    console_clear();
    timer_init(HZ);

    // Set up process descriptors
    memset(processes, 0, sizeof(processes));
    for (pid_t i = 0; i < NPROC; i++) {
        processes[i].p_pid = i;
        processes[i].p_state = P_FREE;
    }
    
    // map kernel memory as inaccessible to applications
    virtual_memory_map(kernel_pagetable, 0, 0, PROC_START_ADDR, PTE_P|PTE_W);
    // except console, which is mapped as writable
    virtual_memory_map(kernel_pagetable,(uintptr_t)console,(uintptr_t)console,
        PAGESIZE, PTE_P|PTE_W|PTE_U);

    if (command && strcmp(command, "fork") == 0) {
        process_setup(1, 4);
    } else if (command && strcmp(command, "forkexit") == 0) {
        process_setup(1, 5);
    } else {
        for (pid_t i = 1; i <= 4; ++i) {
            process_setup(i, i - 1);
        }
    }

    // Switch to the first process using run()
    run(&processes[1]);
}


// pid_t find_free_process_slot(void)
//    searches for a free process slot.
//    If none available it returns -1.
pid_t find_free_process_slot(void) {
    // ignoring slot 0
    for (pid_t i = 1; i < NPROC; i++) {
        if (processes[i].p_state == P_FREE) {
            return i;
        }
    }
    return -1;
}


// void* alloc_free_page(void)
//    searches for a free physical page, if found it tries to
//    allocate it. returns NULL on failure.
void* alloc_free_page(int8_t owner) {
    // search for a free physical page to alloc
    void* pg_addr = find_free_physical_page();
    if (pg_addr == NULL) {
        return NULL;
    }
    // try and allocate the found address
    if (physical_page_alloc((uintptr_t)pg_addr, owner) == -1) {
        return NULL;
    }
    return pg_addr;
}


// void* find_free_physical_page(void)
//    searches for a free physical page, if found it returns the
//    address. If none available it returns NULL
void* find_free_physical_page(void) {
    for (int i = 0; i < NPAGES; ++i) {
        if(pageinfo[i].owner == 0 && pageinfo[i].refcount == 0) {
            return (void*)PAGEADDRESS(i);
        }
    }
    return NULL;
}


// x86_pagetable *copy_pagetable(x86_pagetable* pagetable, int8_t owner)
//    allocates and returns a new page table, initialized as a copy 
//    of pagetable.
x86_pagetable *copy_pagetable(x86_pagetable* pagetable, int8_t owner) {
    // find and allocate page address for new l1table
    void* pgtl1_addr = alloc_free_page(owner);
    if (pgtl1_addr == NULL) {
        return NULL;
    }
    x86_pagetable *pgtl1 = (x86_pagetable *)pgtl1_addr;
    
    // find and allocate page address for new l2table
    void* pgtl2_addr = alloc_free_page(owner);
    if (pgtl2_addr == NULL) {
        return NULL;
    }
    x86_pagetable *pgtl2 = (x86_pagetable *)pgtl2_addr;
    
    // init l1 and l2 tables by zeroing out
    memset(pgtl1->entry, 0, sizeof(pgtl1->entry));
    memset(pgtl2->entry, 0, sizeof(pgtl2->entry)); 
    
    // setting first entry in l1table to point to l2table
    pgtl1->entry[0] = (uintptr_t)pgtl2_addr | PTE_P | PTE_W | PTE_U;
    
    // get address of entries array for kernel pgtable
    x86_pageentry_t *kptl20 = (x86_pageentry_t *)PTE_ADDR(pagetable->entry[0]);
    int proc_strt_off = PAGENUMBER(PROC_START_ADDR) * sizeof(x86_pageentry_t);
    // use memcpy, to copy entries from kptb to new l2table
    memcpy(pgtl2, kptl20, proc_strt_off); 

    return pgtl1;
}


// process_setup(pid, program_number)
//    Load application program `program_number` as process number `pid`.
//    This loads the application's code and data into memory, sets its
//    %eip and %esp, gives it a stack page, and marks it as runnable.
void process_setup(pid_t pid, int program_number) {
    process_init(&processes[pid], 0);
    // create new pagetable as copy of kernel for each process
    processes[pid].p_pagetable = copy_pagetable(kernel_pagetable, pid);
    pageinfo[PAGENUMBER(processes[pid].p_pagetable)].refcount = 1;
    int r = program_load(&processes[pid], program_number);
    assert(r >= 0);
    
    // start process stack at MEMSIZE_VIRTUAL
    processes[pid].p_registers.reg_esp = MEMSIZE_VIRTUAL;
    uintptr_t stack_page = (uintptr_t)alloc_free_page(pid);
    virtual_memory_map(processes[pid].p_pagetable, processes[pid].p_registers.reg_esp - PAGESIZE,
        stack_page, PAGESIZE, PTE_P|PTE_W|PTE_U);
    
    processes[pid].p_state = P_RUNNABLE;
}


// physical_page_alloc(addr, owner)
//    Allocates the page with physical address `addr` to the given owner.
//    Fails if physical page `addr` was already allocated. Returns 0 on
//    success and -1 on failure. Used by the program loader.
int physical_page_alloc(uintptr_t addr, int8_t owner) {
    if ((addr & 0xFFF) != 0
        || addr >= MEMSIZE_PHYSICAL
        || pageinfo[PAGENUMBER(addr)].refcount != 0)
        return -1;
    else {
        pageinfo[PAGENUMBER(addr)].refcount = 1;
        pageinfo[PAGENUMBER(addr)].owner = owner;
        return 0;
    }
}


// pid_t find_page_owner(void)
//    looks for references to page, to find an owner
pid_t find_page_owner(int pagenum) {
    for (int pid = 1; pid < NPROC; ++pid) {
        if (processes[pid].p_state == P_RUNNABLE) {
            proc *p = &processes[pid];
            // search virtual mappings for a ref to physical page
            for (int k = 0; k < NPAGES; ++k) {
                vamapping vam = virtual_memory_lookup(p->p_pagetable,
                        PAGEADDRESS(k));
                if (vam.pa == PAGEADDRESS(pagenum)) {
                    // found another ref to page, set proc as owner
                    return pid;
                }
            }
        }
    }
    // refcount is > 1, but didn't find another owner.
    // must be an error !
    return -1;
}


// void exit_and_free(void)
//    tries to free memory for the current process.
//    looks through all pages, if mulitple references to page
//    then searches for another owner.
void free_current_process(proc *p) {
    // block the process while freeing
    p->p_state=P_BLOCKED;
    // look for all pages with specified process as owner
    for (int i = 0; i < PAGENUMBER(MEMSIZE_PHYSICAL); ++i) {
        if (pageinfo[i].owner == p->p_pid){
            int refcount = pageinfo[i].refcount;
            if (refcount > 1) {
                // multiple owners, decrement refcount by 1
                --pageinfo[i].refcount;
                // check which proc owns page after free
                pid_t new_owner = find_page_owner(i);
                if (new_owner != -1) {
                    pageinfo[i].owner = find_page_owner(i);
                } else {
                    // orphan page, set ref to 0 ??
                    // shouldn't really get to here
                    pageinfo[i].owner = PO_FREE;
                    pageinfo[i].refcount = 0;
                }
            } else if (refcount == 1) {
                // only owned by specified process, decrement refcnt to 0
                // and set owner to free
                --pageinfo[i].refcount;
                pageinfo[i].owner = PO_FREE;
            } else {
                // setting owner to free, refcnt is already 0
                // shouldn't really get to here
                pageinfo[i].owner = PO_FREE;
            }
        }
    }
    p->p_state = P_FREE;
}


// int fork_process(void)
//    creates child process from the current process by copying the
//    current process. The parent is returned the childs pid.
//    The child is returned 0. The caller is returned -1 in case of 
//    failure.
int fork_process(void) {
    pid_t chld_pid = find_free_process_slot();
    if (chld_pid != -1) {
        proc* copy = &processes[chld_pid];
        x86_pagetable *chld_pgtb = copy_pagetable(current->p_pagetable, chld_pid);
        if (chld_pgtb == NULL) {
            // copy pgtable failed due to not enough free memory,
            // so need to free the child's memory
            free_current_process(copy);
            // return -1 to parent, as we had to free the child memory
            return -1; 
        }
        copy->p_pagetable = chld_pgtb;
        copy->p_pid = chld_pid;
        
        for (uintptr_t va = PROC_START_ADDR; va < MEMSIZE_VIRTUAL; va += PAGESIZE) {
            vamapping vam = virtual_memory_lookup(current->p_pagetable, va);
            // check is address writable
            if (vam.perm && (vam.perm & PTE_W)) {
                // allocate new physical page
                void* chld_page = alloc_free_page(chld_pid);
                if (chld_page == NULL) {
                    // allocation failed due to not enough free memory,
                    // so need to free the child's memory
                    free_current_process(copy);
                    // return -1 to parent, as we had to free the child memory
                    return -1; 
                }
                
                // copy the data from V into P, using memcpy; 
                memcpy(chld_page, (void*) vam.pa, PAGESIZE);
                
                // map page P at address V in the child process’s page table.
                virtual_memory_map(copy->p_pagetable, va, (uintptr_t)chld_page,
                        PAGESIZE, vam.perm);
            } else {
                virtual_memory_map(copy->p_pagetable, va, vam.pa, PAGESIZE, vam.perm);
                ++pageinfo[vam.pn].refcount;
            }
        }

        copy->p_registers = current->p_registers;
        copy->p_registers.reg_eax = 0;
        copy->p_state = P_RUNNABLE;
    }

    return chld_pid;
}


// exception(reg)
//    Exception handler (for interrupts, traps, and faults).
//
//    The register values from exception time are stored in `reg`.
//    The processor responds to an exception by saving application state on
//    the kernel's stack, then jumping to kernel assembly code (in
//    k-exception.S). That code saves more registers on the kernel's stack,
//    then calls exception().
//
//    Note that hardware interrupts are disabled whenever the kernel is running.

void exception(x86_registers* reg) {
    // Copy the saved registers into the `current` process descriptor
    // and always use the kernel's page table.
    current->p_registers = *reg;
    set_pagetable(kernel_pagetable);

    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.
    /*log_printf("proc %d: exception %d\n", current->p_pid, reg->reg_intno);*/

    // Show the current cursor location and memory state.
    console_show_cursor(cursorpos);
    virtual_memory_check();
    memshow_physical();
    memshow_virtual_animate();

    // If Control-C was typed, exit the virtual machine.
    check_keyboard();

    // Actually handle the exception.
    switch (reg->reg_intno) {

    case INT_SYS_PANIC:
        panic(NULL);
        break;                  // will not be reached

    case INT_SYS_GETPID:
        current->p_registers.reg_eax = current->p_pid;
        break;

    case INT_SYS_YIELD:
        schedule();
        break;                  /* will not be reached */

    case INT_SYS_PAGE_ALLOC: {
        uintptr_t addr = (uintptr_t)current->p_registers.reg_eax;
        void* p_addr = alloc_free_page(current->p_pid);
        if (p_addr != NULL 
            && (addr % PAGESIZE) == 0
            && addr >= PROC_START_ADDR 
            && addr < MEMSIZE_VIRTUAL) {
            virtual_memory_map(current->p_pagetable, addr, (uintptr_t)p_addr,
                    PAGESIZE, PTE_P|PTE_W|PTE_U);
            current->p_registers.reg_eax = 0;
        } else {
            // if we can't alloc, then return -1
            current->p_registers.reg_eax = -1;
			// print to console informing lack of memory
			console_printf(CPOS(24, 0), 0x0C00, "Out of physical memory!");
        }
        break;
    }

    case INT_TIMER:
        ++ticks;
        schedule();
        break;                  /* will not be reached */

    case INT_PAGEFAULT: {
        // Analyze faulting address and access type.
        uintptr_t addr = rcr2();
        const char* operation = reg->reg_err & PFERR_WRITE
                ? "write" : "read";
        const char* problem = reg->reg_err & PFERR_PRESENT
                ? "protection problem" : "missing page";

        if (!(reg->reg_err & PFERR_USER))
            panic("Kernel page fault for 0x%08X (%s %s, eip=%p)!\n",
                  addr, operation, problem, reg->reg_eip);
        console_printf(CPOS(24, 0), 0x0C00,
                       "Process %d page fault for 0x%08X (%s %s, eip=%p)!\n",
                       current->p_pid, addr, operation, problem, reg->reg_eip);
        current->p_state = P_BROKEN;
        break;
    }
    
    case INT_SYS_FORK: {
        // do fork, set eax to return value (which will be child_pid or -1)
        current->p_registers.reg_eax = fork_process();
        break;  
    }
    
    case INT_SYS_EXIT: {
        // free process memory to exit
        free_current_process(current);
        break;  
    }
    
    default:
        panic("Unexpected exception %d!\n", reg->reg_intno);
        break;                  /* will not be reached */
    }

    // Return to the current process (or run something else).
    if (current->p_state == P_RUNNABLE) {
        run(current);
    } else {
        schedule();
    }
}


// schedule
//    Pick the next process to run and then run it.
//    If there are no runnable processes, spins forever.
void schedule(void) {
    pid_t pid = current->p_pid;
    while (1) {
        pid = (pid + 1) % NPROC;
        if (processes[pid].p_state == P_RUNNABLE)
            run(&processes[pid]);
        // If Control-C was typed, exit the virtual machine.
        check_keyboard();
    }
}


// run(p)
//    Run process `p`. This means reloading all the registers from
//    `p->p_registers` using the `popal`, `popl`, and `iret` instructions.
//
//    As a side effect, sets `current = p`.
void run(proc* p) {
    assert(p->p_state == P_RUNNABLE);
    current = p;

    set_pagetable(p->p_pagetable);
    asm volatile("movl %0,%%esp\n\t"
                 "popal\n\t"
                 "popl %%es\n\t"
                 "popl %%ds\n\t"
                 "addl $8, %%esp\n\t"
                 "iret"
                 :
                 : "g" (&p->p_registers)
                 : "memory");

 spinloop: goto spinloop;       // should never get here
}


// pageinfo_init
//    Initialize the `pageinfo[]` array.
void pageinfo_init(void) {
    extern char end[];

    for (uintptr_t addr = 0; addr < MEMSIZE_PHYSICAL; addr += PAGESIZE) {
        int owner;
        if (physical_memory_isreserved(addr))
            owner = PO_RESERVED;
        else if ((addr >= KERNEL_START_ADDR && addr < (uintptr_t) end)
                 || addr == KERNEL_STACK_TOP - PAGESIZE)
            owner = PO_KERNEL;
        else
            owner = PO_FREE;
        pageinfo[PAGENUMBER(addr)].owner = owner;
        pageinfo[PAGENUMBER(addr)].refcount = (owner != PO_FREE);
    }
}


// virtual_memory_check
//    Check operating system invariants about virtual memory. Panic if any
//    of the invariants are false.
void virtual_memory_check(void) {
    // Process 0 must never be used.
    assert(processes[0].p_state == P_FREE);

    // The kernel page table should be owned by the kernel;
    // its reference count should equal 1, plus the number of processes
    // that don't have their own page tables.
    // Active processes have their own page tables. A process page table
    // should be owned by that process and have reference count 1.
    // All level-2 page tables must have reference count 1.

    // Calculate expected kernel refcount
    int expected_kernel_refcount = 1;
    for (int pid = 0; pid < NPROC; ++pid)
        if (processes[pid].p_state != P_FREE
            && processes[pid].p_pagetable == kernel_pagetable)
            ++expected_kernel_refcount;

    for (int pid = -1; pid < NPROC; ++pid) {
        if (pid >= 0 && processes[pid].p_state == P_FREE)
            continue;

        x86_pagetable* pagetable;
        int expected_owner, expected_refcount;
        if (pid < 0 || processes[pid].p_pagetable == kernel_pagetable) {
            pagetable = kernel_pagetable;
            expected_owner = PO_KERNEL;
            expected_refcount = expected_kernel_refcount;
        } else {
            pagetable = processes[pid].p_pagetable;
            expected_owner = pid;
            expected_refcount = 1;
        }

        // Check main (level-1) page table
        assert(PTE_ADDR(pagetable) == (uintptr_t) pagetable);
        assert(PAGENUMBER(pagetable) < NPAGES);
        assert(pageinfo[PAGENUMBER(pagetable)].owner == expected_owner);
        assert(pageinfo[PAGENUMBER(pagetable)].refcount == expected_refcount);

        // Check level-2 page tables
        for (int pn = 0; pn < PAGETABLE_NENTRIES; ++pn)
            if (pagetable->entry[pn] & PTE_P) {
                x86_pageentry_t pte = pagetable->entry[pn];
                assert(PAGENUMBER(pte) < NPAGES);
                assert(pageinfo[PAGENUMBER(pte)].owner == expected_owner);
                assert(pageinfo[PAGENUMBER(pte)].refcount == 1);
            }
    }

    // Check that all referenced pages refer to active processes
    for (int pn = 0; pn < PAGENUMBER(MEMSIZE_PHYSICAL); ++pn)
        if (pageinfo[pn].refcount > 0 && pageinfo[pn].owner >= 0)
            assert(processes[pageinfo[pn].owner].p_state != P_FREE);
}


// memshow_physical
//    Draw a picture of physical memory on the CGA console.
static const uint16_t memstate_colors[] = {
    'K' | 0x0D00, 'R' | 0x0700, '.' | 0x0700, '1' | 0x0C00,
    '2' | 0x0A00, '3' | 0x0900, '4' | 0x0E00, '5' | 0x0F00,
    '6' | 0x0C00, '7' | 0x0A00, '8' | 0x0900, '9' | 0x0E00,
    'A' | 0x0F00, 'B' | 0x0C00, 'C' | 0x0A00, 'D' | 0x0900,
    'E' | 0x0E00, 'F' | 0x0F00
};

void memshow_physical(void) {
    console_printf(CPOS(0, 32), 0x0F00, "PHYSICAL MEMORY");
    for (int pn = 0; pn < PAGENUMBER(MEMSIZE_PHYSICAL); ++pn) {
        if (pn % 64 == 0)
            console_printf(CPOS(1 + pn / 64, 3), 0x0F00, "0x%06X ", pn << 12);

        int owner = pageinfo[pn].owner;
        if (pageinfo[pn].refcount == 0)
            owner = PO_FREE;
        uint16_t color = memstate_colors[owner - PO_KERNEL];
        // darker color for shared pages
        if (pageinfo[pn].refcount > 1)
            color &= 0x77FF;

        console[CPOS(1 + pn / 64, 12 + pn % 64)] = color;
    }
}


// memshow_virtual(pagetable, name)
//    Draw a picture of the virtual memory map `pagetable` (named `name`) on
//    the CGA console.
void memshow_virtual(x86_pagetable* pagetable, const char* name) {
    assert((uintptr_t) pagetable == PTE_ADDR(pagetable));

    console_printf(CPOS(10, 26), 0x0F00, "VIRTUAL ADDRESS SPACE FOR %s", name);
    for (uintptr_t va = 0; va < MEMSIZE_VIRTUAL; va += PAGESIZE) {
        vamapping vam = virtual_memory_lookup(pagetable, va);
        uint16_t color;
        if (vam.pn < 0)
            color = ' ';
        else {
            assert(vam.pa < MEMSIZE_PHYSICAL);
            int owner = pageinfo[vam.pn].owner;
            if (pageinfo[vam.pn].refcount == 0)
                owner = PO_FREE;
            color = memstate_colors[owner - PO_KERNEL];
            // reverse video for user-accessible pages
            if (vam.perm & PTE_U)
                color = ((color & 0x0F00) << 4) | ((color & 0xF000) >> 4)
                    | (color & 0x00FF);
            // darker color for shared pages
            if (pageinfo[vam.pn].refcount > 1)
                color &= 0x77FF;
        }
        uint32_t pn = PAGENUMBER(va);
        if (pn % 64 == 0)
            console_printf(CPOS(11 + pn / 64, 3), 0x0F00, "0x%06X ", va);
        console[CPOS(11 + pn / 64, 12 + pn % 64)] = color;
    }
}


// memshow_virtual_animate
//    Draw a picture of process virtual memory maps on the CGA console.
//    Starts with process 1, then switches to a new process every 0.25 sec.
void memshow_virtual_animate(void) {
    static unsigned last_ticks = 0;
    static int showing = 1;

    // switch to a new process every 0.25 sec
    if (last_ticks == 0 || ticks - last_ticks >= HZ / 2) {
        last_ticks = ticks;
        ++showing;
    }

    // the current process may have died -- don't display it if so
    while (showing <= 2*NPROC && processes[showing % NPROC].p_state == P_FREE)
        ++showing;
    showing = showing % NPROC;

    if (processes[showing].p_state != P_FREE) {
        char s[4];
        snprintf(s, 4, "%d ", showing);
        memshow_virtual(processes[showing].p_pagetable, s);
    }
}