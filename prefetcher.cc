#include "interface.hh"
#include <stdlib.h>
#include <stdio.h>

// Settings
#define VERBOSE 0
#define HISTORY_SIZE 8
#define PATTERNS_STORED_SIZE 256
#define PATTERNS_MATCH_SIZE 4
#define PATTERNS_PREDICT_SIZE 2
#define PATTERNS_AGING_FACTOR 2

//==================
// Helper Functions
//==================

void prefetch_if_not_cached(Addr addr)
{
    if (!in_cache(addr) && 0 <= addr && addr < MAX_PHYS_MEM_ADDR)
    {
        issue_prefetch(addr);
    }
}

//=========
// History 
//=========

AccessStat *history;
int history_index;

void history_init(void)
{
    history = (AccessStat*) calloc(sizeof(AccessStat), HISTORY_SIZE);
    history_index = HISTORY_SIZE-1;
}

void history_store(AccessStat stat)
{
    history_index = (history_index + 1) % HISTORY_SIZE;
    history[history_index] = stat;
}

AccessStat history_get(int i) // 0 = current, 1 = previous, etc
{
    int index = (history_index - i + HISTORY_SIZE) % HISTORY_SIZE;
    return history[index];
}

//==========
// Patterns
//==========

typedef struct {
    int32_t score;
    int32_t jumps[PATTERNS_MATCH_SIZE + PATTERNS_PREDICT_SIZE];
} Pattern;

Pattern *patterns_stored;

void patterns_init(void)
{
    if (VERBOSE) printf("patterns_init()\n");
    
    // Allocate memory
    patterns_stored = (Pattern*) calloc(sizeof(Pattern), PATTERNS_STORED_SIZE);
}

void pattern_match(Pattern *pat, int32_t *pat_id, int32_t *pat_score)
{
    if (VERBOSE) printf("pattern_match()\n");

    // Reset
    *pat_id = -1;
    *pat_score = 0;
    
    // Check for match
    for (int i = 0; i < PATTERNS_STORED_SIZE; i++)
    {
        int score = 0;
        for (int j = 0; j < PATTERNS_MATCH_SIZE; j++)
        {
            if ((*pat).jumps[j] == patterns_stored[i].jumps[j])
                score++;
            else
                break;
        }
        score *= (*pat).score;
        if (score > *pat_score)
        {
            *pat_id = i;
            *pat_score = score;
        }
    }
    
    if (VERBOSE) printf("best:\n");
    if (VERBOSE) printf("-id:%d\n", *pat_id);
    if (VERBOSE) printf("-score:%d\n", *pat_score);
}

void pattern_match_perfect(Pattern *pat, int32_t *pat_id)
{
    if (VERBOSE) printf("pattern_match_perfect()\n");
    
    // Reset
    *pat_id = -1;
    
    // Check for match
    for (int i = 0; i < PATTERNS_STORED_SIZE; i++)
    {
        int equal = 1;
        for (int j = 0; j < PATTERNS_MATCH_SIZE + PATTERNS_PREDICT_SIZE; j++)
        {
            if ((*pat).jumps[j] != patterns_stored[i].jumps[j])
                equal = 0;
        }
        if (equal == 1)
        {
            if (VERBOSE) printf("-found:%d\n", i);
            *pat_id = i;
            break;
        }
    }
}

void pattern_current(Pattern *pat)
{
    if (VERBOSE) printf("pattern_current()\n");
    
    (*pat).score = 1;
    
    // Calc jumps
    for (int i = 0; i < PATTERNS_MATCH_SIZE + PATTERNS_PREDICT_SIZE; i++)
    {
        int history_i = PATTERNS_MATCH_SIZE + PATTERNS_PREDICT_SIZE - i - 1;
        (*pat).jumps[i] = history_get(history_i).mem_addr - history_get(history_i+1).mem_addr;
    }
}

void pattern_pad(Pattern *pat, int padding)
{
    if (VERBOSE) printf("pattern_pad()\n");

    for (int i = 0; i < PATTERNS_MATCH_SIZE + PATTERNS_PREDICT_SIZE-padding; i++)
    {
        pat->jumps[i] = pat->jumps[i+padding];
    }
    for (int i = PATTERNS_MATCH_SIZE + PATTERNS_PREDICT_SIZE-padding; i < PATTERNS_MATCH_SIZE + PATTERNS_PREDICT_SIZE; i++)
    {
        pat->jumps[i] = 0;
    }
}

void pattern_worst(int32_t *pat_id)
{
    if (VERBOSE) printf("pattern_worst()\n");
    
    // Find pattern with lowest score
    int32_t lowest_i = 0;
    int32_t lowest_score = patterns_stored[0].score;
    for (int i = 1; i < PATTERNS_STORED_SIZE; i++)
    {
        if (patterns_stored[i].score < lowest_score)
        {
            lowest_score = patterns_stored[i].score;
            lowest_i = i;
        }
    }
    *pat_id = lowest_i;

    if (VERBOSE) printf("-id:%d\n", lowest_i);
    if (VERBOSE) printf("-score:%d\n", lowest_score);
}

void pattern_check(void)
{
    if (VERBOSE) printf("pattern_check()\n");
    
    // Get
    Pattern pat;
    pattern_current(&pat);
    pattern_pad(&pat, PATTERNS_PREDICT_SIZE);
    
    // Best match
    int32_t id;
    int32_t score;
    pattern_match(&pat, &id, &score);
    
    // Predict
    if (score > 1)
    {
        Pattern pat_match = patterns_stored[id];
        Addr addr = history_get(0).mem_addr;
        for (int i = PATTERNS_MATCH_SIZE; i < PATTERNS_MATCH_SIZE + PATTERNS_PREDICT_SIZE; i++)
        {
            addr += pat_match.jumps[i];  
            prefetch_if_not_cached(addr);
            if (VERBOSE) printf("-prefetching\n");
        }
    }
    
    // Check for perfect match
    int32_t id_perfect;
    pattern_match_perfect(&pat, &id_perfect);
    
    if (id_perfect < 0)
    {
        // Overwrite worst
        int32_t id_worst;
        pattern_worst(&id_worst);
        pattern_current(&patterns_stored[id_worst]);
        if (VERBOSE) printf("-replacing:%d\n", id_worst);
    }
    else
    {
        // Increase score
        patterns_stored[id_perfect].score++;
        if (VERBOSE) printf("-incrementing\n");
    }
}

void patterns_age(void)
{
    if (VERBOSE) printf("patterns_age()\n");
    
    // Check for match
    for (int i = 0; i < PATTERNS_STORED_SIZE; i++)
    {
        if (VERBOSE) printf("-(%d:%d)\n", i, patterns_stored[i].score);
        patterns_stored[i].score--;
    }
}

//===========
// Framework 
//===========

void prefetch_init(void)
{
    history_init();
    patterns_init();
}

int counter = 0;

void prefetch_access(AccessStat stat)
{
    // Store statistics
    history_store(stat);
    
    pattern_check();
    
    counter = (counter + 1) % (PATTERNS_AGING_FACTOR * PATTERNS_STORED_SIZE);
    if (counter == 0)
    {
        patterns_age();
    }
}

void prefetch_complete(Addr addr)
{
    // YOLO
}

