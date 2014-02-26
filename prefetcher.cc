#include "interface.hh"
#include <stdlib.h>
#include <stdio.h>

/* Bits per entry: 28*3 + 16*n + roof(sqrt(n)) */
/* n = 16 gives 344 bits (43 bytes) which allows 188 rows (8096 B / 43 B = 188.28) */

/* Magic Numbers */
#define VERBOSE 1
#define CALIBRATION_INTERVAL (1024)
#define RATE_FACTOR 1000000
#define DCPT_SIZE 180
#define DCPT_DELTAS 16
#define DCPT_DELTA_BITS 16
#define DCPT_DELTA_DISCARD_BITS 4 /* 2^4 = 32, block size is 64 */
#define DCPT_DELTA_MAX ((1 << (DCPT_DELTA_BITS - 1)) - 1)
#define DCPT_DELTA_MIN (0 - DCPT_DELTA_MAX)
#define DCPT_DISCARD_ENABLED 0
#define PREFETCH_DEGREE_MAX 4

/* Prototypes */
void prefetcher_init();
void prefetcher_access(AccessStat stat);
void prefetcher_calibrate();

/*============*/
/* Statistics */
/*============*/

int64_t stat_read, stat_read_hits;
int64_t stat_issued, stat_issued_hits;

void stats_reset()
{
    stat_read = 1;
    stat_read_hits = 1;
    stat_issued = 1;
    stat_issued_hits = 1;
}

int64_t stats_hit_rate()
{
    return (stat_read_hits*RATE_FACTOR) / (stat_read);
}

int64_t stats_issued_hit_rate()
{
    return (stat_issued_hits*RATE_FACTOR) / stat_issued;
}

int64_t stats_rate(int64_t rate_a, int64_t rate_b)
{
    return (rate_a*RATE_FACTOR) / (rate_b);
}

/*=========*/
/* Helpers */
/*=========*/

void issue_if_needed(Addr addr)
{
    if (!in_cache(addr) && !in_mshr_queue(addr)
        && 0 <= addr && addr < MAX_PHYS_MEM_ADDR
        && current_queue_size() < MAX_QUEUE_SIZE)
    {
        issue_prefetch(addr);
        if (VERBOSE >= 2) printf("Prefetch issued for address %d\n", (int)addr);
    }
}

/*===========*/
/* Framework */
/*===========*/

int counter = 0;

void prefetch_init()
{
    stats_reset();
    prefetcher_init();
}

void prefetch_access(AccessStat stat)
{
    /* Count reads */
    stat_read++;
    
    /* Count hits */
    if (!stat.miss) 
    {
        stat_read_hits++;
    }
    
    /* Count hits on prefetched blocks */
    if (!stat.miss && get_prefetch_bit(stat.mem_addr))
    {
        stat_issued_hits++;
    }
    
    /* Run prefetcher logic */
    prefetcher_access(stat);
    
    /* Clear prefetch tag */
    clear_prefetch_bit(stat.mem_addr);
    
    /* Recalibrate occationally */
    if (++counter == CALIBRATION_INTERVAL)
    {
        counter = 0;
        prefetcher_calibrate();
    }
}

void prefetch_complete(Addr addr)
{
    /* Tag block as prefetched */
    set_prefetch_bit(addr);
    stat_issued++;
}

/*======*/
/* DCPT */
/*======*/

typedef int32_t DCPT_PC;
typedef int32_t DCPT_Addr;
typedef int16_t DCPT_Delta;
typedef int8_t DCPT_Index;

typedef struct {
    DCPT_PC pc;
    DCPT_Addr last_address;
    DCPT_Addr last_prefetch;
    DCPT_Delta delta[DCPT_DELTAS];
    DCPT_Index delta_head;
} DCPT_Entry;

int dcpt_head;
int dcpt_size;
DCPT_Entry *dcpt;
DCPT_Addr dcpt_candidates[DCPT_DELTAS];

/* Initializes table */
void dcpt_init(int size)
{
    dcpt_head = -1;
    dcpt_size = size;
    dcpt = (DCPT_Entry*) calloc(sizeof(DCPT_Entry), size);
}

/* Create a new entry in the table */
DCPT_Entry *dcpt_new(DCPT_PC pc, DCPT_Addr addr)
{
    /* Grab next entry (FIFO) */
    dcpt_head = (dcpt_head + 1) % dcpt_size;
    DCPT_Entry *entry = &dcpt[dcpt_head];
    
    /* Prepare the entry */
    entry->pc = pc;
    entry->last_address = addr;
    entry->last_prefetch = 0;
    for (int i = 0; i < DCPT_DELTAS; i++)
    {
        entry->delta[i] = 0;
    }
    entry->delta_head = 0;
    
    return entry;
}

/* Find an entry in the table */
DCPT_Entry *dcpt_find(DCPT_PC pc)
{
    for (int i = 0; i < DCPT_SIZE; i++)
    {
        if (dcpt[i].pc == pc)
        {
            return &dcpt[i];
        }
    }
    return NULL;
}

/* Get a delta within an entry */
DCPT_Delta dcpt_delta_get(DCPT_Entry *entry, DCPT_Index index)
{
    return entry->delta[(entry->delta_head - index + DCPT_DELTAS) % DCPT_DELTAS];
}

/* Store a delta in an entry */
void dcpt_delta_store(DCPT_Entry *entry, DCPT_Delta delta)
{
    entry->delta_head = (entry->delta_head + 1) % DCPT_DELTAS;
    entry->delta[entry->delta_head] = delta;
}

/* Finds candidate prefetch addresses */
/* Returns number of candidates */
int dcpt_candidates_find(DCPT_Entry *entry)
{
    DCPT_Delta delta_a = dcpt_delta_get(entry, 0);
    DCPT_Delta delta_b = dcpt_delta_get(entry, 1);
    if (delta_a == 0 || delta_b == 0) return 0; /* Overflow */
    
    for (int i = 1; i < DCPT_DELTAS-1; i++)
    {
        if (dcpt_delta_get(entry, i) == delta_a && dcpt_delta_get(entry, i+1) == delta_b)
        {
            /* Number of candidates */
            int x = 0;
            
            DCPT_Addr addr = entry->last_address;
            
            for (int k = 0; k < i; k++)
            {
                DCPT_Delta delta = dcpt_delta_get(entry, i-k-1);
                if (delta == 0) break; /* Overflow */
                
                /* Add candidate */
                addr += delta << DCPT_DELTA_DISCARD_BITS;
                dcpt_candidates[x++] = addr;
                
                /* Discard all candidates if previous prefetch found */
                if (addr == entry->last_prefetch && DCPT_DISCARD_ENABLED)
                {
                    x = 0;
                }
            }
            return x;
        }
    }
    return 0;
}

/*============*/
/* Prefetcher */
/*============*/

void prefetcher_init()
{
    dcpt_init(DCPT_SIZE);
}

void prefetcher_access(AccessStat stat)
{
    /* Get data */
    DCPT_Addr pc = stat.pc;
    DCPT_Addr addr = stat.mem_addr;
    
    /* Find entry */
    DCPT_Entry *entry = dcpt_find(pc);
    
    /* Create if missing */
    if (entry == NULL)
    {
        entry = dcpt_new(pc, addr);
    }
    
    /* Store new delta */
    DCPT_Addr delta = (addr - entry->last_address) >> DCPT_DELTA_DISCARD_BITS;
    if (delta < DCPT_DELTA_MIN || delta > DCPT_DELTA_MAX)
    {
        dcpt_delta_store(entry, 0); /* Overflow */
        
        /* Update last address */
        entry->last_address = addr;
    }
    else if (delta != 0)
    {
        dcpt_delta_store(entry, (DCPT_Delta) delta);
        
        /* Update last address */
        entry->last_address = addr;
        
        /* Find and prefetch candidates */
        int c = dcpt_candidates_find(entry);
        for (int i = 0; i < c && i < PREFETCH_DEGREE_MAX; i++)
        {
            DCPT_Addr addr = dcpt_candidates[i];
            issue_if_needed(addr);
            entry->last_prefetch = addr;
        }
    }
}

void prefetcher_calibrate()
{
    /* TODO */
    
    /* Get statistics */
    int hit_rate = stats_hit_rate();
    int issued_hit_rate = stats_issued_hit_rate();
    
    /* Dump some info */
    if (VERBOSE >= 1)
    {
        printf("[] Calibrating...\n");
        printf(" - Hit rate: %d\n", hit_rate);
        printf(" - Issued hit rate: %d\n", issued_hit_rate);
    }

    // Reset stats
    stats_reset();
}

