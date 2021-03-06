/**
 * @file   voter.cpp
 * @brief  For the replicated version of DieHard: votes on output.
 * @author Emery Berger <http://www.cs.umass.edu/~emery>
 * @note   Copyright (C) 2005 by Emery Berger, University of Massachusetts Amherst.
 */

#include <signal.h>
#include <stdio.h>
#include <string.h> // for memset

#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>

#include "pipetype.h"
#include "selector.h"

#include "fingerprinter.h"
#include "mostcommonelement.h"
#include "randomnumbergenerator.h"
#include "realrandomvalue.h"

bool dumpPending = false;
bool dumpRunning = false;

/// @brief Picks an agreed-upon buffer value and outputs it.
static int handleInput (const char * space, const int index, FILE ** out);

void requestDump(int signum) {
  fprintf(stderr,"Voter got request to start isolation\n");
  dumpPending = true;
}

void dumpCompleted(int signum) {
  fprintf(stderr,"Voter got notification of new patch\n");
  dumpRunning = false;
  dumpPending = false;
}

void voter (char * space, pipeType * fullpipe, pipeType * continuepipe)
{
  signal(SIGUSR1,requestDump);
  signal(SIGUSR2,dumpCompleted);

  FILE * continueSignal[NUMBER_OF_REPLICAS];
  FILE * bufferFull[NUMBER_OF_REPLICAS];
  int deadCount = 0;
  bool deadFd[NUMBER_OF_REPLICAS];

  for (int i = 0; i < NUMBER_OF_REPLICAS; i++) {
    deadFd[i] = false;
    bufferFull[i] = fdopen (fullpipe[i].getInput(), "rb");
    setbuf (bufferFull[i], 0);
    continueSignal[i] = fdopen (continuepipe[i].getOutput(), "wb");
    setbuf (continueSignal[i], 0);
  }

  while (ReplicasAliveNow > 0) {

    int maxfd = 0;
    fd_set readfds;
    FD_ZERO (&readfds);
    for (int i = 0; i < NUMBER_OF_REPLICAS; i++) {
      if (!deadFd[i]) {
        int fd = fileno(bufferFull[i]);
        if (fd > maxfd) {
          maxfd = fd;
        }
        FD_SET (fd, &readfds);
      }
    }

    struct timeval timeoutValue;
    timeoutValue.tv_sec = 0;
    timeoutValue.tv_usec = 10000;
    int readyFds = select (maxfd + 1, &readfds, NULL, NULL, &timeoutValue); // NULL

    if (readyFds) {

      // We got some input.
      // Handle the ready file descriptors.
      
      for (int i = 0; i < NUMBER_OF_REPLICAS; i++) {
        
        // If this fd was ready, process it.
        
        if ((FD_ISSET (fileno(bufferFull[i]), &readfds))) {
          
          handleInput (space, i, continueSignal);

          if (fgetc (bufferFull[i]) == EOF) {
            //assert(!deadFd[i]);
            if (!deadFd[i]) {
              // If it wasn't already dead, kill it.
              fprintf(stderr,"dead fd %d\n",i);
              deadFd[i] = true;
              deadCount++;
              ReplicasAliveNow--;
            }      
          }
        }
      }
    }
    
    fflush (stdout);
  }

  fprintf(stderr,"voter exiting (%d)...\n",getpid());

  exit(23);
}

static RandomNumberGenerator rng (RealRandomValue::value(), RealRandomValue::value());
static int barrierArrivals = 0;
static Fingerprinter f[MAX_REPLICAS];

static int handleInput (const char * space, const int index, FILE ** out)
{
  barrierArrivals++;
  // Got a signal.

  //fprintf(stderr,"barrier arrivals: %d\n",barrierArrivals);
  
  if (barrierArrivals >= ReplicasAliveNow) {
    barrierArrivals = 0;

    if(dumpPending && !dumpRunning) {
      dumpRunning = true;
      dumpPending = false;
      fprintf(stderr,"Running isolator from barrier entry\n");
      if(!fork()) {
        isolator();
      }
    }
    
    // Now we do fingerprinting and "vote".

    // EDB: FIX ME - this needs to take into account the fact that
    // some replicas may have died.

    for (int i = 0; i < NUMBER_OF_REPLICAS; i++) {
      f[i]((char *) space + i * BUFFER_LENGTH, BUFFER_LENGTH);
    }
    
    Fingerprinter result;
    int count;
    mostCommonElement<Fingerprinter>(f, NUMBER_OF_REPLICAS, result, count);
    
    //fprintf (stderr, "(agreement = %d)\n", count);
    
    if (count < 2) {
      // Now we have no way to check for agreement.
      // So off we go.
      fprintf (stderr, "No replicas agreed - time to die.\n");

      // Dump the buffers for debugging.
      // Print out one of each of the values.
#if 0
      for (int i = 0; i < NUMBER_OF_REPLICAS; i++) {
        printf ("---- replica %d ----\n", i);
        char * ch = (char *) space + i * BUFFER_LENGTH;
        for (int j = 0; j < BUFFER_LENGTH; j++) {
          if (*ch != '\0') {
            fputc (*ch, stdout);
          }
          ch++;
        }
      }
#endif

      exit (-2);
    }

    if (count >= 2) {

      //fprintf(stderr,"foo\n");

      // Print out one of the agreed-upon values.
#if 1 
      char * ch = (char *) result.getStart();
      for (int j = 0; j < BUFFER_LENGTH; j++) {
        if (*ch != '\0') {
          fputc (*ch, stdout);
        }
        ch++;
      }
#endif

      fflush (stdout);

      // Clean up the buffer.
      long * l = (long *) result.getStart();
      for (int j = 0; j < BUFFER_LENGTH / sizeof(long); j++) {
        *l = rng.next();
        l++;
      }
    }

    // if not all agree, then run the isolator
    if(count < ReplicasAliveNow) {
      fprintf(stderr,"got %d agreed vs %d\n",count,ReplicasAliveNow);
      if(!fork()) isolator();
    }

    // Here we could kill those who don't agree,
    // or replace them with snapshots of ones that do (replacing
    // their RNG seed). For now, we do nothing.

    for (int i = 0; i < NUMBER_OF_REPLICAS; i++) {
      fputc ('X', out[i]);
      fflush (out[i]);
    }
    return 0;
  }
}
