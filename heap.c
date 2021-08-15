// filename *************************heap.c ************************
// Implements memory heap for dynamic memory allocation.
// Follows standard malloc/calloc/realloc/free interface
// for allocating/unallocating memory.

// Jacob Egner 2008-07-31
// modified 8/31/08 Jonathan Valvano for style
// modified 12/16/11 Jonathan Valvano for 32-bit machine
// modified August 10, 2014 for C99 syntax

/* This example accompanies the book
   "Embedded Systems: Real Time Operating Systems for ARM Cortex M Microcontrollers",
   ISBN: 978-1466468863, Jonathan Valvano, copyright (c) 2015

 Copyright 2015 by Jonathan W. Valvano, valvano@mail.utexas.edu
    You may use, edit, run or distribute this file
    as long as the above copyright notice remains

 THIS SOFTWARE IS PROVIDED "AS IS".  NO WARRANTIES, WHETHER EXPRESS, IMPLIED
 OR STATUTORY, INCLUDING, BUT NOT LIMITED TO, IMPLIED WARRANTIES OF
 MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE APPLY TO THIS SOFTWARE.
 VALVANO SHALL NOT, IN ANY CIRCUMSTANCES, BE LIABLE FOR SPECIAL, INCIDENTAL,
 OR CONSEQUENTIAL DAMAGES, FOR ANY REASON WHATSOEVER.
 For more information about my classes, my research, and my books, see
 http://users.ece.utexas.edu/~valvano/
 */


#include <stdint.h>
#include "../RTOS_Labs_common/heap.h"
#include "../RTOS_Labs_common/OS.h"

#define HEAP_SIZE 2048 //# of 32-bits

static int32_t HEAP[HEAP_SIZE];
Sema4Type heap;
/*
Heap allocation scheme
(+ int) ... (+ int) - indicates how much space is in between these two locations
(- int) ... (- int) - indicates space taken up by these two locations
example. 
1

1
 means one location available

*/

//******** Heap_Init *************** 
// Initialize the Heap
// input: none
// output: always 0
// notes: Initializes/resets the heap to a clean state where no memory
//  is allocated.
int32_t Heap_Init(void){
  HEAP[0] = HEAP_SIZE - 2;
  HEAP[HEAP_SIZE - 1] = HEAP_SIZE - 2;
  OS_InitSemaphore(&heap, 1);
  return 0;
}


//******** Heap_Malloc *************** 
// Allocate memory, data not initialized
// input: 
//   desiredBytes: desired number of bytes to allocate
// output: void* pointing to the allocated memory or will return NULL
//   if there isn't sufficient space to satisfy allocation request
void* Heap_Malloc(int32_t desiredBytes){
  // first fit
  // can only allocate by 32 bits
  OS_bWait(&heap);
  int32_t neededBlocks = (desiredBytes)/4; // words needed for bytes
  if(desiredBytes%4 != 0) {//extra block
    neededBlocks++;
  }
  uint32_t i = 0;
  while(i < HEAP_SIZE) {
    if(HEAP[i] >= neededBlocks) {
      //allocate here
      //some issues - what if 2 blocks available, but neededBlocks = 1? leftover block of 0
      if(i + HEAP[i] + 1 <= HEAP_SIZE - 1) {
        HEAP[i + HEAP[i] + 1] = HEAP[i] - (neededBlocks + 2);
      }
      if(i + neededBlocks + 2 <= HEAP_SIZE - 1) {
        HEAP[i + neededBlocks + 2] = HEAP[i] - (neededBlocks + 2);
      }
      HEAP[i] = -neededBlocks;
      HEAP[i + neededBlocks + 1] = -neededBlocks;
      OS_bSignal(&heap);
      return (HEAP + i + 1);
    }
    
    // might also be 0 (wasted space), but can't allocate
    if(HEAP[i] > 0) {
      i = i + HEAP[i] + 2; //go to next section
    }
    else if(HEAP[i] < 0) {
      i = i - HEAP[i] + 2; 
    }
    else{
      i++;
    }
  }
  OS_bSignal(&heap);
  return 0;   // no space
}


//******** Heap_Calloc *************** 
// Allocate memory, data are initialized to 0
// input:
//   desiredBytes: desired number of bytes to allocate
// output: void* pointing to the allocated memory block or will return NULL
//   if there isn't sufficient space to satisfy allocation request
//notes: the allocated memory block will be zeroed out
void* Heap_Calloc(int32_t desiredBytes){  
  // malloc then init to 0
  int32_t* block = Heap_Malloc(desiredBytes);
  OS_bWait(&heap);
  if(block == 0) {
    OS_bSignal(&heap);
    return 0;
  }
  for(int i = 0; i < -*(block - 1); i++) { //num bytes
    block[i] = 0;
  }
  OS_bSignal(&heap);
  return block;   // NULL
}


//******** Heap_Realloc *************** 
// Reallocate buffer to a new size
//input: 
//  oldBlock: pointer to a block
//  desiredBytes: a desired number of bytes for a new block
// output: void* pointing to the new block or will return NULL
//   if there is any reason the reallocation can't be completed
// notes: the given block may be unallocated and its contents
//   are copied to a new block if growing/shrinking not possible
void* Heap_Realloc(void* oldBlock, int32_t desiredBytes){
  int32_t* newblock = Heap_Malloc(desiredBytes);
  OS_bWait(&heap);
  if(newblock == 0) {
    OS_bSignal(&heap);
    return 0;
  }
  // get smaller size
  int32_t* blockptr = (int32_t*) oldBlock;
  
  int tmp1 = -*((int32_t*) oldBlock - 1);
  int tmp2 = -*((int32_t*) newblock - 1);
  if(tmp1 <= tmp2){
    for(int i = 0; i < -*((int32_t*) oldBlock - 1); i++) {
      newblock[i] = blockptr[i];
    }
  }
  else {
    for(int i = 0; i < -*((int32_t*) newblock - 1); i++) {
      newblock[i] = blockptr[i];
    }
  }
  OS_bSignal(&heap);
  if(Heap_Free(oldBlock)) {
    return 0;
  }
  return newblock;   // NULL
}


//******** Heap_Free *************** 
// return a block to the heap
// input: pointer to memory to unallocate
// output: 0 if everything is ok, non-zero in case of error (e.g. invalid pointer
//     or trying to unallocate memory that has already been unallocated
int32_t Heap_Free(void* pointer){
  // set boundaries to positive, test if above and below are positive, if yes merge
  OS_bWait(&heap);
  int32_t* blockptr = (int32_t*) pointer;
  *(blockptr - 1) = - *(blockptr - 1);
  *(blockptr + *(blockptr -1)) = *(blockptr - 1);
  // merge above
  int32_t* top = blockptr - 1;
  if(blockptr - 1 != HEAP){
    if(*(blockptr - 2) >= 0) {
      uint32_t total = *(blockptr - 1) + *(blockptr - 2) + 2;
      *(blockptr - 2 - *(blockptr - 2) - 1) = total;
      top = blockptr - 2 - *(blockptr - 2) - 1;
      *(blockptr + *(blockptr -1)) = total;
    }
  }
  //merge below
  if(blockptr + *(blockptr -1) != &HEAP[HEAP_SIZE - 1]) {
    if(*(blockptr + *(blockptr -1) + 1) >= 0) {
      uint32_t total =  *(blockptr + *(blockptr -1)) + *(blockptr + *(blockptr -1) + 1) + 2;
      *(blockptr + *(blockptr - 1) + 2 + *(blockptr + *(blockptr - 1) + 1)) = total;
      *(top) = total;
    }
  }
  OS_bSignal(&heap);
  return 0;   // replace
}


//******** Heap_Stats *************** 
// return the current status of the heap
// input: reference to a heap_stats_t that returns the current usage of the heap
// output: 0 in case of success, non-zeror in case of error (e.g. corrupted heap)
int32_t Heap_Stats(heap_stats_t *stats){
  // just go through heap
  uint32_t i = 0;
  stats->free = 0;
  stats->used = 0;
  while(i < HEAP_SIZE) {
    if(HEAP[i] > 0) {
      stats->free += HEAP[i]*sizeof(int32_t);
      i = i + HEAP[i] + 2;
    }
    else if(HEAP[i] < 0) {
      stats->used -= HEAP[i]*sizeof(int32_t);
      i = i - HEAP[i] + 2;
    }
    else {
      i++;
    }
  }
  stats->size = HEAP_SIZE*sizeof(int32_t);
  return 0;   // replace
}
