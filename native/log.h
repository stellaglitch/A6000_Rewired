#ifndef REWIRED_LOG_H
#define REWIRED_LOG_H

//-------------------------------------------------------------------
// A log on the memory card, for the one question a camera never answers on
// its own: how far did it get before it died?
//
// Every line is opened, written and closed by itself, so a process killed
// mid-shot - by the kernel for memory, or by a fault - loses at most the
// line it was writing. A fault handler adds a last line naming the signal
// and the stage, then hands the signal back to whoever had it before, so
// the platform's own crash handling still runs.
//-------------------------------------------------------------------

// Where to write; null switches logging off. Installs the fault handler on
// the first call with a path.
void rw_log_open(const char *path);

void rw_log(const char *fmt, ...);

// One line of what memory looks like now: free and cached in the system, and
// this process's resident size. Off Linux it says so and moves on.
void rw_log_mem(const char *when);

// Milliseconds from an arbitrary start, for timing stages.
unsigned long rw_ms(void);

#endif
