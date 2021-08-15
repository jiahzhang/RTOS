// filename ************** eFile.c *****************************
// High-level routines to implement a solid-state disk 
// Students implement these functions in Lab 4
// Jonathan W. Valvano 1/12/20
#include <stdint.h>
#include <string.h>
#include "../RTOS_Labs_common/OS.h"
#include "../RTOS_Labs_common/eDisk.h"
#include "../RTOS_Labs_common/eFile.h"
#include <stdio.h>
#include <string.h>

// FAT file system - need 2048 blocks to support 1 mebibyte (2048*512 = 1048576)
/*
struct FIL {
  char name[7];
  uint16_t start_block; // only need 11 bits but have to use 16
  uint16_t byte_count; // only need 9 bits but have to use 16, number of bytes in FINAL block (don't need count for others)
};
typedef struct FIL FIL_t;
*/
uint8_t file_state = 0; // 0 is closed, 1 is write-mode, 2 is read-mode
uint8_t openFileblock[512]; // just file data (most likely will be just last block or first block)
uint16_t byte_position; // current byte position (for reading/writing) - 0 to 511
uint16_t file_position; // current position in directory
uint16_t file_block; // block location open

// directory size is file (file name size + 2 + 2) x 10 + 1 + 1 + 2 = ((7 + 2 + 2) x 10) + 1 + 1 + 2 = 114
/*
  directory byte order will be:
    bitmask for files (2 byte)
    first empty block (2 bytes) (9 at start)
    file_name (7 bytes)       *
    start_block (2 bytes)     * Repeated 10 times
    byte_count (2 bytes)      *
*/
/*
struct DIR {
  FIL_t files[10];
  uint8_t file_count;
};
typedef struct DIR DIR_t;
*/
uint8_t openDIRblock[512]; // holds directory (pre-assign block 8)
int8_t dir_position;
uint8_t dir_state; // 0 - closed, 1 - open

// index table size is 512 bytes/2 (need 16 bits) x 8 = 256 (num of locations) x 8 = 2048 (need to represent 2048 blocks so need 8 blocks to support index table)
uint8_t openFATblock[512]; // pre-assign blocks 0-7 for index table
uint8_t partition;

uint8_t mount_state; // 0 - not yet mounted, 1 - mounted
Sema4Type sdc;

//---------- open_partition-------------
//Open corresponding partition if not opened yet
//Also updates location to reflect indexx
DRESULT open_partition(uint16_t* location) {
  uint8_t p = (*location)/256;
  *location = (*location)%256;
  if(partition != p) {
    // need to open this partition
    eDisk_WriteBlock(openFATblock, partition);
    DRESULT result = eDisk_ReadBlock(openFATblock, p);
    if(result != RES_OK) {
      return result;
    }
    partition = p;
  }
  return RES_OK;
}

// compare two buffers of size n
// 0 if same, 1 if different
uint8_t compare(char buf1[], const char buf2[]) {
  for(int i = 0; i < strlen(buf2); i++) {
    if(buf1[i] != buf2[i]) {
      return 0;
    }
  }
  return 1;
}

//---------- eFile_Init-----------------
// Activate the file system, without formating
// Input: none
// Output: 0 if successful and 1 on failure (already initialized)
int eFile_Init(void){ // initialize file system
  CS_Init();
  DSTATUS result = eDisk_Init(0);
  if(result == RES_OK) {
    OS_InitSemaphore(&sdc, 1);
    return 0;
  }
  return 1;   // replace
}

//---------- eFile_Format-----------------
// Erase all files, create blank directory, initialize free space manager
// Input: none
// Output: 0 if successful and 1 on failure (e.g., trouble writing to flash)
int eFile_Format(void){ // erase disk, add format
  long sr = OS_LockScheduler();
  OS_Wait(&sdc);
  // create new directory in RAM
  openDIRblock[0] = 0; openDIRblock[1] = 0; openDIRblock[2] = 0; openDIRblock[3] = 9;
  memset(&openDIRblock[4], 0, sizeof(openDIRblock) - 4);
  if(eDisk_WriteBlock(openDIRblock, 8) != RES_OK) {
    OS_Signal(&sdc);
    OS_UnLockScheduler(sr);
    return 1;
  }
  
  // create index tables in RAM and write onto disk, free space initialized
  // not available to be assigned (for index table and directory) partition 0
  partition = 0;
  openFATblock[0] = 0; // index table 0
  openFATblock[1] = 0;
  
  openFATblock[2] = 0; // index table 1
  openFATblock[3] = 0;
  
  openFATblock[4] = 0; // index table 2
  openFATblock[5] = 0;
  
  openFATblock[6] = 0; // index table 3
  openFATblock[7] = 0;
  
  openFATblock[8] = 0; // index table 4
  openFATblock[9] = 0;
  
  openFATblock[10] = 0; // index table 5
  openFATblock[11] = 0;
  
  openFATblock[12] = 0; // index table 6
  openFATblock[13] = 0;
  
  openFATblock[14] = 0; // index table 7
  openFATblock[15] = 0;
  
  openFATblock[16] = 0; // directory
  openFATblock[17] = 0;
  
  for(uint32_t j = 18; j < 256; j+=2) {
    uint16_t next_block = j/2 + 1; // example: index 18 will contain 10
    uint8_t upper_byte = next_block >> 8;
    uint8_t lower_byte = next_block & 0x00FF;
    openFATblock[j] = upper_byte;
    openFATblock[j+1] = lower_byte;
  }
  
  if(eDisk_WriteBlock(openFATblock, 0) != RES_OK) {
    OS_Signal(&sdc);
    OS_UnLockScheduler(sr);
    return 1;
  }
  
  for(uint8_t i = 1; i < 8; i++) { // partition 1 - 7
    for(uint32_t j = 0; j < 256; j+=2) { // position 512 to 2048
      uint16_t next_block = (256*i + j)/2;
      uint8_t upper_byte = next_block >> 8;
      uint8_t lower_byte = next_block & 0x00FF;
      openFATblock[j] = upper_byte;
      openFATblock[j+1] = lower_byte;
    }
    
    if(eDisk_WriteBlock(openFATblock, i) != RES_OK) {
      OS_Signal(&sdc);
      OS_UnLockScheduler(sr);
      return 1;
    }
    
    partition = i;
  }
  
  mount_state = 1;
  dir_position = -1;
  file_position = 0;
  file_state = 0;
  OS_Signal(&sdc);
  OS_UnLockScheduler(sr);
  return 0;   // replace
}

//---------- eFile_Mount-----------------
// Mount the file system, without formating
// Input: none
// Output: 0 if successful and 1 on failure
int eFile_Mount(void){ // initialize file system
  OS_Wait(&sdc);
  // bring in a directory from disk
  
  eDisk_ReadBlock(openDIRblock, 8);
  eDisk_ReadBlock(openFATblock, 0);
  partition = 0;
  dir_position = -1;
  file_position = 0;
  mount_state = 1;
  file_state = 0;
  OS_Signal(&sdc);
  return 0;   // replace
}


//---------- eFile_Create-----------------
// Create a new, empty file with one allocated block
// Input: file name is an ASCII string up to seven characters 
// Output: 0 if successful and 1 on failure (e.g., trouble writing to flash)
int eFile_Create( const char name[]){  // create new file, make it empty 
  OS_Wait(&sdc);
  if(mount_state == 0) {
    OS_Signal(&sdc);
    return 1;
  }
  // find if available space
  uint16_t bitmask = (openDIRblock[0] << 8) + openDIRblock[1];
  uint16_t i;
  for(i = 0; i < 10; i++) {
    if((bitmask & 0x0001) == 0) {
      // claim this location
      break;
    }
    bitmask = bitmask >> 1;
  }
  
  if(i == 10) { //all spaces full
    OS_Signal(&sdc);
    return 1;
  }
  
  // get first empty block
  uint16_t location = (openDIRblock[2] << 8) + openDIRblock[3];
  // claim space (update pointer to first free block)
  uint16_t temp = location;
  if(open_partition(&location) != RES_OK) {
    OS_Signal(&sdc);
    return 1;
  }
  openDIRblock[2] = openFATblock[2*location];
  openDIRblock[3] = openFATblock[2*location + 1];
  // update FAT block to not point to next free block
  openFATblock[2*location] = 0;
  openFATblock[2*location + 1] = 0;
  
  // add new file to directory - update name, start block, num_bytes in last block
  // name
  for(int j = 0; j < strlen(name); j++){
    openDIRblock[4 + i*11 + j] = name[j]; // could probably use strcpy here
  }
  if(strlen(name) != 7) {
    openDIRblock[4 + i*11 + strlen(name)] = 0;
  }
  // start_block
  openDIRblock[11 + i*11] = temp >> 8;
  openDIRblock[12 + i*11] = temp & 0x00FF;
  // byte_count
  openDIRblock[13 + i*11] = 0;
  openDIRblock[14 + i*11] = 0;
  
  // update bitmask in directory
  bitmask = (openDIRblock[0] << 8) + openDIRblock[1]; 
  bitmask |= (1 << i);
  openDIRblock[0] = bitmask >> 8;
  openDIRblock[1] = bitmask & 0x00FF;
  OS_Signal(&sdc);
  return 0;
}


//---------- eFile_WOpen-----------------
// Open the file, read into RAM last block
// Input: file name is an ASCII string up to seven characters
// Output: 0 if successful and 1 on failure (e.g., trouble writing to flash)
int eFile_WOpen( const char name[]){      // open a file for writing 
  OS_Wait(&sdc);
  if(file_state != 0 || mount_state == 0) { // a file is already open for writing
    OS_Signal(&sdc);
    return 1;
  }
  // find this file in directory
  uint16_t bitmask = (openDIRblock[0] << 8) + openDIRblock[1];
  int i;
  for(i = 0; i < 10; i++) {
    if(bitmask & 0x0001) {
      char buffer[7];
      buffer[0] = openDIRblock[4 + i*11];
      buffer[1] = openDIRblock[5 + i*11];
      buffer[2] = openDIRblock[6 + i*11];
      buffer[3] = openDIRblock[7 + i*11];
      buffer[4] = openDIRblock[8 + i*11];
      buffer[5] = openDIRblock[9 + i*11];
      buffer[6] = openDIRblock[10 + i*11];
      
      if(compare(buffer, name)) { // found file
        // find last block location
        uint16_t location = 0;
        uint16_t next_location = (openDIRblock[11 + i*11] << 8) + openDIRblock[12 + i*11];
        do {
          location = next_location;
          if(open_partition(&location) != RES_OK) {
            OS_Signal(&sdc);
            return 1;
          }
          // get next block
          next_location = (openFATblock[2*location] << 8) + openFATblock[2*location + 1];
        } while(next_location != 0);
        
        // read into RAM
        if(eDisk_ReadBlock(openFileblock, partition*256 + location) != RES_OK) {
          OS_Signal(&sdc);
          return 1;
        }
        file_block = partition*256 + location;
        file_position = i;
        break;
      }
    }
    bitmask = bitmask >> 1;
  }
  if(i == 10) {
    OS_Signal(&sdc);
    return 1;
  }
  file_state = 1;
  OS_Signal(&sdc);
  return 0;   // replace  
}

//---------- eFile_Write-----------------
// save at end of the open file
// Input: data to be saved
// Output: 0 if successful and 1 on failure (e.g., trouble writing to flash)
int eFile_Write( const char data){
  OS_Wait(&sdc);
  if(file_state != 1 || mount_state == 0) {
    OS_Signal(&sdc);
    return 1;
  }
  
  uint16_t write_position = (openDIRblock[13 + 11*file_position] << 8) + openDIRblock[14 + 11*file_position];
  openFileblock[write_position] = data;
  write_position++;
  if(write_position == 512) {
    write_position = 0;
    openDIRblock[13 + 11*file_position] = write_position >> 8;
    openDIRblock[14 + 11*file_position] = write_position & 0x00FF;
  }
  else {
    openDIRblock[13 + 11*file_position] = write_position >> 8;
    openDIRblock[14 + 11*file_position] = write_position & 0x00FF;
    OS_Signal(&sdc);
    return 0;
  }
  
  // update index table new block added, wrap end_byte_position 
  // store back file into location
  eDisk_WriteBlock(openFileblock, file_block);
  
  //get first available block in free space manager
  uint16_t new_block_location = (openDIRblock[2] << 8) + openDIRblock[3];
  //store block in FAT
  open_partition(&file_block);
  openFATblock[2*file_block] = openDIRblock[2];
  openFATblock[2*file_block + 1] = openDIRblock[3];
  if(open_partition(&new_block_location)) {
    OS_Signal(&sdc);
    return 1;
  }
  //update free space manager
  openDIRblock[2] = openFATblock[2*new_block_location];
  openDIRblock[3] = openFATblock[2*new_block_location + 1];
  //clear FAT block
  openFATblock[2*new_block_location] = 0;
  openFATblock[2*new_block_location + 1] = 0;
  
  // read in new block
  if(eDisk_ReadBlock(openFileblock, 256*partition + new_block_location) != RES_OK) {
    OS_Signal(&sdc);
    return 1;
  }
  file_block = 256*partition + new_block_location;
  OS_Signal(&sdc);
  return 0;   // replace
}

//---------- eFile_WClose-----------------
// close the file, left disk in a state power can be removed
// Input: none
// Output: 0 if successful and 1 on failure (e.g., trouble writing to flash)
int eFile_WClose(void){ // close the file for writing
  OS_Wait(&sdc);
  if(file_state != 1 || mount_state == 0) {
    OS_Signal(&sdc);
    return 1;
  }
  
  eDisk_WriteBlock(openFileblock, file_block);
  file_block = 0;
  file_state = 0;
  // make sure position is at end
  OS_Signal(&sdc);
  return 0;   // replace
}


//---------- eFile_ROpen-----------------
// Open the file, read first block into RAM 
// Input: file name is an ASCII string up to seven characters
// Output: 0 if successful and 1 on failure (e.g., trouble read to flash)
int eFile_ROpen( const char name[]){      // open a file for reading 
  OS_Wait(&sdc);
  if(file_state != 0 || mount_state == 0) {
    OS_Signal(&sdc);
    return 1;
  }
  // find this file in directory
  uint16_t bitmask = (openDIRblock[0] << 8) + openDIRblock[1];
  int i;
  for(i = 0; i < 10; i++) {
    if(bitmask & 0x0001) {
      char buffer[7];
      buffer[0] = openDIRblock[4 + i*11];
      buffer[1] = openDIRblock[5 + i*11];
      buffer[2] = openDIRblock[6 + i*11];
      buffer[3] = openDIRblock[7 + i*11];
      buffer[4] = openDIRblock[8 + i*11];
      buffer[5] = openDIRblock[9 + i*11];
      buffer[6] = openDIRblock[10 + i*11];
      
      if(compare(buffer, name)) { // found file
        // find last block location
        uint16_t location = (openDIRblock[11 + i*11] << 8) + openDIRblock[12 + i*11];
        
        // read into RAM
        if(eDisk_ReadBlock(openFileblock, partition*256 + location) != RES_OK) {
          OS_Signal(&sdc);
          return 1;
        }
        file_block = partition*256 + location;
        file_position = i;
        byte_position = 0;
        break;
      }
    }
    bitmask = bitmask >> 1;
  }  
  if(i == 10) {
    OS_Signal(&sdc);
    return 1;
  }
  file_state = 2;
  OS_Signal(&sdc);
  return 0;   // replace   
}
 
//---------- eFile_ReadNext-----------------
// retreive data from open file
// Input: none
// Output: return by reference data
//         0 if successful and 1 on failure (e.g., end of file)
int eFile_ReadNext( char *pt){       // get next byte 
  OS_Wait(&sdc);
  if(file_state != 2 || mount_state == 0) {
    OS_Signal(&sdc);
    return 1;
  }
  // check if EOF
  uint16_t num_bytes = (openDIRblock[13 + 11*file_position] << 8) + openDIRblock[14 + 11*file_position];
  if(open_partition(&file_block)) {
    OS_Signal(&sdc);
    return 1;
  }
  uint16_t next_block = (openFATblock[2*file_block] << 8) + openFATblock[2*file_block + 1];
  if(byte_position == num_bytes && next_block == 0) {
    OS_Signal(&sdc);
    return 1;
  }
  
  // not EOF, get byte
  *pt = openFileblock[byte_position];
  byte_position++;
  if(byte_position == 512) {
    // open next block
    byte_position = 0;
  }
  else {
    OS_Signal(&sdc);
    return 0;
  }
  
  // switch out file block if EOF
  if(eDisk_ReadBlock(openFileblock, next_block) != RES_OK) {
    OS_Signal(&sdc);
    return 1;
  }
  file_block = next_block;
  OS_Signal(&sdc);
  return 0;   // replace
}
    
//---------- eFile_RClose-----------------
// close the reading file
// Input: none
// Output: 0 if successful and 1 on failure (e.g., wasn't open)
int eFile_RClose(void){ // close the file for writing
  OS_Wait(&sdc);
  if(file_state != 2 || mount_state == 0) {
    OS_Signal(&sdc);
    return 1;
  }
  
  file_block = 0; 
  file_state = 0;
  byte_position = 0;
  OS_Signal(&sdc);
  return 0;   // replace
}


//---------- eFile_Delete-----------------
// delete this file
// Input: file name is seven ASCII letters
// Output: 0 if successful and 1 on failure (e.g., trouble writing to flash)
int eFile_Delete( const char name[]){  // remove this file 
  OS_Wait(&sdc);
  if(file_state == 1) {
    OS_Signal(&sdc);
    eFile_WClose();
    OS_Wait(&sdc);
  }
  else if(file_state == 2) {
    OS_Signal(&sdc);
    eFile_RClose();
    OS_Wait(&sdc);
  }
  else if (mount_state == 0) {
    OS_Signal(&sdc);
    return 1;
  }
  
  // find this file in directory
  uint16_t bitmask = (openDIRblock[0] << 8) + openDIRblock[1];
  int i;
  for(i = 0; i < 10; i++) {
    if(bitmask & 0x0001) {
      char buffer[7];
      buffer[0] = openDIRblock[4 + i*11];
      buffer[1] = openDIRblock[5 + i*11];
      buffer[2] = openDIRblock[6 + i*11];
      buffer[3] = openDIRblock[7 + i*11];
      buffer[4] = openDIRblock[8 + i*11];
      buffer[5] = openDIRblock[9 + i*11];
      buffer[6] = openDIRblock[10 + i*11];
      
      if(compare(buffer, name)) { // found file
        uint16_t location = 0;
        uint16_t next_location = (openDIRblock[11 + i*11] << 8) + openDIRblock[12 + i*11];
        do {
          location = next_location;
          if(open_partition(&location) != RES_OK) {
            OS_Signal(&sdc);
            return 1;
          }
          // get next block
          next_location = (openFATblock[2*location] << 8) + openFATblock[2*location + 1];
          // update FAT so that location now points to head of free space and then becomes head of free space
          openFATblock[2*location] = openDIRblock[2];
          openFATblock[2*location + 1] = openDIRblock[3];
          openDIRblock[2] = (partition*256 + location) >> 8;
          openDIRblock[3] = (partition*256 + location) & 0x00FF;
        } while(next_location != 0);
        
        // remove entry from directory
        bitmask = (openDIRblock[0] << 8) + openDIRblock[1];
        bitmask = bitmask & ~(1 << i);
        openDIRblock[0] = bitmask >> 8;
        openDIRblock[1] = bitmask & 0x00FF;
        break;
      }
    }
    bitmask = bitmask >> 1;
  }  
  if(i == 10) {
    OS_Signal(&sdc);
    return 1;
  }
  OS_Signal(&sdc);
  return 0;   // replace
}                             


//---------- eFile_DOpen-----------------
// Open a (sub)directory, read into RAM
// Input: directory name is an ASCII string up to seven characters
//        (empty/NULL for root directory)
// Output: 0 if successful and 1 on failure (e.g., trouble reading from flash)
int eFile_DOpen( const char name[]){ // open directory
  OS_Wait(&sdc);
  if(mount_state == 0) {
    OS_Signal(&sdc);
    return 1;
  }
  dir_state = 1;
  dir_position = -1;
  OS_Signal(&sdc);
  return 0;   // replace
}
  
//---------- eFile_DirNext-----------------
// Retreive directory entry from open directory
// Input: none
// Output: return file name and size by reference
//         0 if successful and 1 on failure (e.g., end of directory)
int eFile_DirNext( char name[], unsigned long *size){  // get next entry 
  OS_Wait(&sdc);
  // get name
  if(mount_state == 0 || dir_state == 0) {
    OS_Signal(&sdc);
    return 1;
  }
  
  //get next position
  dir_position++;
  if(dir_position == 10) {
    dir_position = -1;
    OS_Signal(&sdc);
    return 1;
  }
  uint16_t bitmask = (openDIRblock[0] << 8) + openDIRblock[1];
  while((bitmask & (1 << dir_position)) == 0) {
    dir_position++;
    if(dir_position == 10) {
      dir_position = -1;
      OS_Signal(&sdc);
      return 1;
    }
  }
  name[0] = openDIRblock[4 + 11*dir_position];
  name[1] = openDIRblock[5 + 11*dir_position];
  name[2] = openDIRblock[6 + 11*dir_position];
  name[3] = openDIRblock[7 + 11*dir_position];
  name[4] = openDIRblock[8 + 11*dir_position];
  name[5] = openDIRblock[9 + 11*dir_position];
  name[6] = openDIRblock[10 + 11*dir_position];
  
  // get size
  uint16_t i = 0;
  uint16_t location = 0;
  uint16_t next_location = (openDIRblock[11 + dir_position*11] << 8) + openDIRblock[12 + dir_position*11];
  do {
    i++;
    location = next_location;
    if(open_partition(&location) != RES_OK) {
      OS_Signal(&sdc);
      return 1;
    }
    // get next block
    next_location = (openFATblock[2*location] << 8) + openFATblock[2*location + 1];
  } while(next_location != 0);
  
  *size = (i-1)*512 + (openDIRblock[13 + dir_position*11] << 8) + openDIRblock[14 + dir_position*11];
  OS_Signal(&sdc);
  return 0;   // replace
}

//---------- eFile_DClose-----------------
// Close the directory
// Input: none
// Output: 0 if successful and 1 on failure (e.g., wasn't open)
int eFile_DClose(void){ // close the directory
  OS_Wait(&sdc);
  if(eDisk_WriteBlock(openDIRblock, 8) != RES_OK || mount_state == 0) {
    OS_Signal(&sdc);
    return 1;
  }
  dir_position = -1;
  dir_state = 0;
  OS_Signal(&sdc);
  return 0;   // replace
}


//---------- eFile_Unmount-----------------
// Unmount and deactivate the file system
// Input: none
// Output: 0 if successful and 1 on failure (not currently mounted)
int eFile_Unmount(void){ 
  OS_Wait(&sdc);
  if(mount_state == 0) {
    OS_Signal(&sdc);
    return 1;
  }
  eDisk_WriteBlock(openDIRblock, 8);
  eDisk_WriteBlock(openFATblock, partition);
  if(file_block != 0){
    eDisk_WriteBlock(openFileblock, file_block);
  }  
  mount_state = 0;
  OS_Signal(&sdc);
  return 0;   // replace
}
