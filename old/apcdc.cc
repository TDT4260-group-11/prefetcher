#include "interface.hh"
#include <stdlib.h>
#include <stdio.h>

// For GHB sizes up to 1024, KB_SIZE+GHB_SIZE can be 1724 (8KB) : 28+10 bits per line
// For GHB sizes up to 2048, KB_SIZE+GHB_SIZE can be 1680 (8KB) : 28+11 bits per line

// Magic Numbers
#define VERBOSE 1
#define CALIBRATION_INTERVAL (2*1024)
#define KB_SIZE 512
#define GHB_SIZE 1024
#define MATCH_DEGREE 2
#define LOOKBACK_AMOUNT 64
#define PREFETCH_DEGREE_DEFAULT 1
#define PREFETCH_DEGREE_MAX 4
#define STORE_MISSES_ONLY 0
#define CZONE_MODE 0
#define CZONE_BITS_DEFAULT 16
#define COUNTDOWN_SHORT 2
#define COUNTDOWN_LONG 16
#define RATE_FACTOR 1000000

// Prototypes
void prefetcher_init();
void prefetcher_access(AccessStat stat);
void prefetcher_delta_correlate();
void prefetcher_calibrate();

//============
// Statistics
//============

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

//=========
// Helpers
//=========

void issue_if_needed(Addr addr)
{
    if (!in_cache(addr) && !in_mshr_queue(addr) && 0 <= addr && addr < MAX_PHYS_MEM_ADDR)
    {
        issue_prefetch(addr);
    }
}

//===========
// Framework 
//===========

int counter = 0;

void prefetch_init()
{
    stats_reset();
    prefetcher_init();
}

void prefetch_access(AccessStat stat)
{
    // Count reads
    stat_read++;
    
    // Count hits
    if (!stat.miss) 
    {
        stat_read_hits++;
    }
    
    // Count hits on prefetched blocks
    if (!stat.miss && get_prefetch_bit(stat.mem_addr))
    {
        stat_issued_hits++;
    }
    
    // Run prefetcher logic
    prefetcher_access(stat);
    
    // Clear prefetch tag
    clear_prefetch_bit(stat.mem_addr);
    
    // Recalibrate occationally
    if (++counter == CALIBRATION_INTERVAL)
    {
        counter = 0;
        prefetcher_calibrate();
    }
}

void prefetch_complete(Addr addr)
{
    // Tag block as prefetched
    set_prefetch_bit(addr);
    stat_issued++;
}

//============
// Key Buffer
//============

typedef int32_t KB_Key;
typedef int16_t KB_Index;
typedef struct {
    KB_Key key;
    KB_Index index;
} KB_Entry;

KB_Entry *kb;
KB_Index kb_head;
KB_Index kb_size;

void kb_init(int size)
{
    kb_head = -1;
    kb_size = size;
    kb = (KB_Entry*) calloc(sizeof(KB_Entry), size);
    int bytes = sizeof(KB_Entry)*size;
    if (VERBOSE >= 1) printf("KB initialized to %d rows (%d bytes)\n", size, bytes);
}

void kb_store(KB_Key key, KB_Index index)
{
    kb_head = (kb_head + 1) % kb_size;
    kb[kb_head].key = key;
    kb[kb_head].index = index;
    if (VERBOSE >= 3) printf("KB[%d] now stores [%d,%d]\n", kb_head, key, index);
}

//=======================
// Global History Buffer
//=======================

typedef int32_t GHB_Address;
typedef int16_t GHB_Index;
typedef struct {
    GHB_Address address;
    GHB_Index previous;
} GHB_Entry;

GHB_Entry *ghb;
GHB_Index ghb_head;
GHB_Index ghb_size;

void ghb_init(int size)
{
    ghb_head = -1;
    ghb_size = size;
    ghb = (GHB_Entry*) calloc(sizeof(GHB_Entry), size);
    int bytes = sizeof(GHB_Entry)*size;
    if (VERBOSE >= 1) printf("GHB initialized to %d rows (%d bytes)\n", size, bytes);
}

void ghb_store(GHB_Address address, GHB_Index previous)
{
    ghb_head = (ghb_head + 1) % ghb_size;
    ghb[ghb_head].address = address;
    ghb[ghb_head].previous = previous;
    if (VERBOSE >= 3) printf("GHB[%d] now stores [%d,%d]\n", ghb_head, address, previous);
}

//============
// Prefetcher
//============

int prefetch_degree = PREFETCH_DEGREE_DEFAULT;
int czone_bits = CZONE_BITS_DEFAULT;

void prefetcher_init(void)
{
    kb_init(KB_SIZE);
    ghb_init(GHB_SIZE);
}

void prefetcher_access(AccessStat stat)
{
    // Exit if not miss
    if (!stat.miss && STORE_MISSES_ONLY)
    {
        return;
    }
    
    // Calculate key
    KB_Key key;
    if (CZONE_MODE)
    {
        key = stat.mem_addr >> czone_bits;
    }
    else
    {
        key = stat.pc;
    }
    
    // Find index
    KB_Index key_i = -1;
    KB_Index index = -1;
    for (KB_Index i = 0; i < kb_size; i++)
    {
        if (kb[i].key == key)
        {
            index = kb[i].index;
            key_i = i;
            break;
        }
    }
    
    // Create if not found
    if (index == -1)
    {
        kb_store(key, -1);
        key_i = kb_head;
    }
    
    // Store miss
    ghb_store(stat.mem_addr, index);
    
    // Update key
    kb[key_i].index = ghb_head;
    
    // Run Delta Correlation
    prefetcher_delta_correlate();
}

void prefetcher_delta_correlate()
{
    // Skip if no prefetching
    if (prefetch_degree == 0)
    {
        return;
    }
    
    // Create a buffer for deltas
    int buffer_head = -1;
    int buffer_size = prefetch_degree + MATCH_DEGREE;
    GHB_Address buffer[buffer_size];
    
    // For storing the ghb indexes
    GHB_Index index_current = ghb_head;
    GHB_Index index_previous = 0;
    
    // For storing the comparison deltas
    GHB_Address deltas[MATCH_DEGREE]; //TODO: GHB_Index better than GHB_Address???
    
    // Get latest address
    GHB_Address address = ghb[ghb_head].address;
    
    for (int i = 0; i < LOOKBACK_AMOUNT; i++)
    {
        // Get previous
        index_previous = ghb[index_current].previous;
        
        // Exit if invalid
        if (index_previous == -1)
        {
            break;
        }
        
        // Calculate delta
        GHB_Address delta = ghb[index_current].address - ghb[index_previous].address;
        
        // Add to buffer
        buffer_head = (buffer_head + 1) % buffer_size;
        buffer[buffer_head] = delta;
        
        // Store comparison deltas
        if (i < MATCH_DEGREE)
        {
            deltas[i] = buffer[i];
        }
        
        // Check for correlation once buffer is filled
        if (i > buffer_size - 2)
        {
            int buffer_i = buffer_head;
            int match = 1;
            for (int k = 0; k < MATCH_DEGREE; k++)
            {
                if (buffer[buffer_i] != deltas[MATCH_DEGREE-k-1])
                {
                    match = 0;
                }
                buffer_i = (buffer_i - 1 + buffer_size) % buffer_size;
            }
            if (match)
            {
                // Prefetch
                for (int k = 0; k < prefetch_degree; k++)
                {
                    address += buffer[buffer_i];
                    issue_if_needed(address);
                    buffer_i = (buffer_i - 1 + buffer_size) % buffer_size;
                }
                if (VERBOSE >= 2) printf("Prefetching blocks! (degree %d)\n", prefetch_degree);
                break;
            }
        }
    }
}

#define BLOCKED_TIME 8
#define INCREASE_THRESHOLD ((RATE_FACTOR*95)/100)
#define DECREASE_THRESHOLD ((RATE_FACTOR*50)/100)

int action = 0;
int last_hit_rate = 0;
int blocked[PREFETCH_DEGREE_MAX + 1];
int first_run = 1;

void prefetcher_calibrate()
{
    // First run stuff
    if (first_run)
    {
        // Clear calibration
        for (int i = 0; i <= PREFETCH_DEGREE_MAX; i++)
        {
            blocked[i] = BLOCKED_TIME;
        }
        
        // Reset stats
        stats_reset();
        
        first_run = 0;
        return;
    }
    
    // Get stats
    int hit_rate = stats_hit_rate();
    int issued_hit_rate = stats_issued_hit_rate();
    
    int better = hit_rate > ((last_hit_rate*103)/100);
    int worse = hit_rate < ((last_hit_rate*97)/100);
    
    // Increased PFD last
    if (action == 1)
    {
        if (better)
        {
            for (int i = 0; i < prefetch_degree; i++)
            {
                blocked[i] = BLOCKED_TIME;
            }
        }
        if (worse)
        {
            for (int i = prefetch_degree; i <= PREFETCH_DEGREE_MAX; i++)
            {
                blocked[i] = BLOCKED_TIME;
            }
        }
    }
    
    // Decreased PFD last
    if (action == -1)
    {
        if (better)
        {
            for (int i = prefetch_degree + 1; i <= PREFETCH_DEGREE_MAX; i++)
            {
                blocked[i] = BLOCKED_TIME;
            }
        }
        if (worse)
        {
            for (int i = 0; i <= prefetch_degree; i++)
            {
                blocked[i] = BLOCKED_TIME;
            }
        }
    }
    
    // Explore if able
    action = 0;
    if (blocked[prefetch_degree + 1] <= 0 && prefetch_degree < PREFETCH_DEGREE_MAX)
    {
        action = 1;
    }
    if (blocked[prefetch_degree - 1] <= 0 && prefetch_degree > 0)
    {
        action = -1;
    }
    
    // Override if issued hit rate is very high or low
    int issued_override = 0;
    /*
    if (issued_hit_rate > INCREASE_THRESHOLD && prefetch_degree < PREFETCH_DEGREE_MAX)
    {
        action = 1;
        issued_override = 1;
    }
    if (issued_hit_rate < DECREASE_THRESHOLD && prefetch_degree > 0)
    {
        action = -1;
        issued_override = 1;
    }
    */
    
    if (VERBOSE >= 1)
    {
        printf("[] Calibrating...\n");
        printf(" - Old PFD: %d\n", prefetch_degree);
        printf(" - New PFD: %d\n", prefetch_degree + action);
        printf(" - Old hit rate: %d\n", last_hit_rate);
        printf(" - New hit rate: %d\n", hit_rate);
        printf(" - Better: %d\n", better);
        printf(" - Worse: %d\n", worse);
        printf(" - Issued hit rate: %d\n", issued_hit_rate);
        printf(" - Issued Override: %d\n", issued_override);
        printf(" - Blocked:");
        for (int i = 0; i <= PREFETCH_DEGREE_MAX; i++)
        {
            printf(" %d,", blocked[i]);
        }
        printf("\n");
    }
    
    // Countdown blocks
    for (int i = 0; i <= PREFETCH_DEGREE_MAX; i++)
    {
        blocked[i]--;
    }
    
    // Updates stuff
    prefetch_degree = prefetch_degree + action;
    last_hit_rate = hit_rate;
    
    // Reset stats
    stats_reset();
}


/*
#define TIME_THRESHOLD 32

// Data structure to store hit rates
int hit_rates[PREFETCH_DEGREE_MAX+1];
int hit_times[PREFETCH_DEGREE_MAX+1];
int first_run = 1;

void prefetcher_calibrate()
{
    // First run stuff
    if (first_run)
    {
        // Clear calibration
        for (int i = 0; i <= PREFETCH_DEGREE_MAX; i++)
        {
            hit_rates[i] = 0;
            hit_times[i] = TIME_THRESHOLD;
        }
        
        // Reset stats
        stats_reset();
        
        first_run = 0;
        return;
    }

    // Get stats
    int hit_rate = stats_hit_rate();
    //int issued_hit_rate = stats_issued_hit_rate();
    
    // Store rates and update time
    hit_rates[prefetch_degree] = hit_rate;
    for (int i = 0; i <= PREFETCH_DEGREE_MAX; i++)
    {
        hit_times[i] += 1;
    }
    hit_times[prefetch_degree] = 0;
    
    
    int next_prefetch_degree = prefetch_degree;
    
    // Time to check another prefetch degree?
    if (prefetch_degree == 0)
    {
        if (hit_times[prefetch_degree + 1] > TIME_THRESHOLD)
        {
            next_prefetch_degree = prefetch_degree + 1;
        }
        else
        {
            if (hit_rates[prefetch_degree + 1] > hit_rates[prefetch_degree])
            {
                next_prefetch_degree = prefetch_degree + 1;
            }
        }
    }
    else if (prefetch_degree == PREFETCH_DEGREE_MAX)
    {
        if (hit_times[prefetch_degree - 1] > TIME_THRESHOLD)
        {
            next_prefetch_degree = prefetch_degree - 1;
        }
        else
        {
            if (hit_rates[prefetch_degree - 1] > hit_rates[prefetch_degree])
            {
                next_prefetch_degree = prefetch_degree - 1;
            }
        }
    }
    else
    {
        if (hit_times[prefetch_degree + 1] > TIME_THRESHOLD)
        {
            next_prefetch_degree = prefetch_degree + 1;
        }
        else
        if (hit_times[prefetch_degree - 1] > TIME_THRESHOLD)
        {
            next_prefetch_degree = prefetch_degree - 1;
        }
        else
        {
            if (hit_rates[prefetch_degree + 1] > hit_rates[next_prefetch_degree])
            {
                next_prefetch_degree = prefetch_degree + 1;
            }
            if (hit_rates[prefetch_degree - 1] > hit_rates[next_prefetch_degree])
            {
                next_prefetch_degree = prefetch_degree - 1;
            }
        }
    }
    
    if (VERBOSE >= 1)
    {
        printf("[] Calibrating...\n");
        printf(" - Old PFD: %d\n", prefetch_degree);
        printf(" - New PFD: %d\n", next_prefetch_degree);
        printf(" - Hit rates:");
        for (int i = 0; i <= PREFETCH_DEGREE_MAX; i++)
        {
            printf(" %d,", hit_rates[i]);
        }
        printf("\n");
        printf(" - Hit times:");
        for (int i = 0; i <= PREFETCH_DEGREE_MAX; i++)
        {
            printf(" %d,", hit_times[i]);
        }
        printf("\n");
    }
    
    // Update degree
    prefetch_degree = next_prefetch_degree;
    
    // Reset stats
    stats_reset();
}
*/

/*
#define INC_THRESHOLD ((RATE_FACTOR*90)/100)
#define DEC_THRESHOLD ((RATE_FACTOR*70)/100)

int countdown = COUNTDOWN_LONG;

void prefetcher_calibrate()
{
    if (countdown > 0)
    {
        countdown--;
        return;
    }
    
    int hit_rate = stats_hit_rate();
    int issued_hit_rate = stats_issued_hit_rate();
    //int above_threshold = issued_hit_rate > INC_THRESHOLD;
    //int below_threshold = issued_hit_rate < DEC_THRESHOLD;
    int above_threshold = (issued_hit_rate > (hit_rate*100)/100)
        || (issued_hit_rate > ((RATE_FACTOR*95)/100));
    int below_threshold = issued_hit_rate < (hit_rate*95)/100;
    
    
    // Determine next PFD
    int next_prefetch_degree = prefetch_degree;
    if (prefetch_degree == 0)
    {
        next_prefetch_degree = prefetch_degree + 1;
    }
    else if (above_threshold && prefetch_degree < PREFETCH_DEGREE_MAX)
    {
        next_prefetch_degree = prefetch_degree + 1;
    }
    else if (below_threshold)
    {
        next_prefetch_degree = prefetch_degree - 1;
    }
    
    // Set countdown
    if (next_prefetch_degree == prefetch_degree || next_prefetch_degree == 0)
    {
        countdown = COUNTDOWN_LONG;
    }
    else
    {
        countdown = COUNTDOWN_SHORT;
    }
    
    if (VERBOSE >= 1)
    {
        printf("[] Calibrating...\n");
        printf(" - Hit rate: %d\n", hit_rate);
        printf(" - Issued hit rate: %d\n", issued_hit_rate);
        printf(" - Above threshold: %d\n", above_threshold);
        printf(" - Below threshold: %d\n", below_threshold);
        printf(" - Old PFD: %d\n", prefetch_degree);
        printf(" - New PFD: %d\n", next_prefetch_degree);
        printf(" - Countdown: %d\n", countdown);
    }
    
    // Update degree
    prefetch_degree = next_prefetch_degree;
    
    // Reset stats
    stats_reset();
}
*/

/*
#define CHECK_CURRENT 0
#define CHECK_LOWER 1
#define CHECK_HIGHER 2
#define CHOOSE 3
#define COUNTDOWN 4

int state = CHECK_CURRENT;
int score_lower = -1;
int score_current = -1;
int score_higher = -1;

int previous_choice = -1;
int countdown_left = 0;

void prefetcher_calibrate()
{
    int score = stats_hit_rate();
    int state_next = state;
    
    if (state == CHECK_CURRENT)
    {
        score_current = score;
        
        if (prefetch_degree - 1 < PREFETCH_DEGREE_MIN)
        {
            score_lower = -1;
            state_next = CHECK_HIGHER;
            prefetch_degree += 1;
        }
        else
        {
            state_next = CHECK_LOWER;
            prefetch_degree -= 1;
        }
    }
    if (state == CHECK_LOWER)
    {
        score_lower = score;
        
        if (prefetch_degree + 2 > PREFETCH_DEGREE_MAX)
        {
            score_higher = -1;
            prefetch_degree += 1;
            state_next = CHOOSE;
        }
        else
        {
            state_next = CHECK_HIGHER;
            prefetch_degree += 2;
        }
    }
    if (state == CHECK_HIGHER)
    {
        score_higher = score;
        prefetch_degree -= 1;
        state_next = CHOOSE;
    }
    if (state == CHOOSE)
    {
        // Set best PFD
        if (score_higher > score_current && score_higher > score_lower)
        {
           prefetch_degree += 1;
        } 
        if (score_lower > score_current && score_lower > score_higher)
        {
           prefetch_degree -= 1;
        } 
        
        // Set countdown
        if (prefetch_degree == previous_choice)
        {
            countdown_left = COUNTDOWN_LONG;
        }
        else
        {
            countdown_left = COUNTDOWN_SHORT;
        }
        
        state_next = COUNTDOWN;
        
        if (VERBOSE >= 1) printf("# New PFD: %d\n", prefetch_degree);
        if (VERBOSE >= 1) printf("- Scores: %d, %d, %d\n", score_lower, score_current, score_higher);
        if (VERBOSE >= 1) printf("- Countdown: %d\n", countdown_left);
        
        // Update choice
        previous_choice = prefetch_degree;
    }
    
    if (state == COUNTDOWN)
    {
        // Count down
        countdown_left--;
        
        // Recheck once countdown finishes
        if (countdown_left == 0)
        {
            state_next = CHECK_CURRENT;
        }
    }
    
    // Update state
    state = state_next;
    
    // Reset stats
    stats_reset();
}
*/
