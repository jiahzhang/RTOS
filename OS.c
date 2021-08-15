// *************os.c**************
// EE445M/EE380L.6 Labs 1, 2, 3, and 4 
// High-level OS functions
// Students will implement these functions as part of Lab
// Runs on LM4F120/TM4C123
// Jonathan W. Valvano 
// Jan 12, 2020, valvano@mail.utexas.edu


#include <stdint.h>
#include <stdio.h>
#include "../inc/tm4c123gh6pm.h"
#include "../inc/CortexM.h"
#include "../inc/PLL.h"
#include "../inc/LaunchPad.h"
#include "../inc/Timer4A.h"
#include "../inc/WTimer0A.h"
#include "../RTOS_Labs_common/OS.h"
#include "../RTOS_Labs_common/ST7735.h"
#include "../inc/ADCT0ATrigger.h"
#include "../RTOS_Labs_common/UART0int.h"
#include "../RTOS_Labs_common/eFile.h"
#include "../RTOS_Labs_common/eDisk.h"
#include "../RTOS_Labs_common/ADC.h"
#include "../RTOS_Labs_common/FIFO.h"
#include "../RTOS_Labs_common/heap.h"
#include "../inc/Timer0A.h"
#include "../inc/Timer2A.h"
#include "../inc/Timer3A.h"
#include "../inc/Timer4A.h"

extern void StartOS(uint32_t* sp);
extern void ContextSwitch(void);

// Performance Measurements 
int32_t MaxJitter;             // largest time jitter between interrupts in usec (first jitter)
#define JITTERSIZE 64
uint32_t const JitterSize1=JITTERSIZE;
uint32_t const JitterSize2=JITTERSIZE;
uint32_t JitterHistogram1[JITTERSIZE]={0,};
uint32_t JitterHistogram2[JITTERSIZE]={0,};

#define NUMTHREADS 10
#define NUMPROCESSES 10
#define STACKSIZE 128
#define OSFIFOSIZE 64
#define FIFOSUCCESS 1         // return on FIFO success
#define FIFOFAIL 0            // return on FIFO fail

#define PRI 1

// OS System Time only shared between TimerInit.c and OS.c
uint32_t msSystemTime;
uint32_t tensecSystemTime;

//THREADS
// Active thread linked list
TCB_t* RunPt = NULL;
// Next thread to be run in active list
TCB_t* NextRunPt = NULL;

// Linked list of sleeping threads
TCB_t* SleepPt = NULL;

// Allocate TCBs
static TCB_t TCBStack[NUMTHREADS];
static uint32_t ThreadCount;
static uint32_t ActiveThreads = 0;
// Allocate stacks
static uint32_t stack[NUMTHREADS][STACKSIZE];
// Currently allocated threads
static uint8_t CurrentThreads[NUMTHREADS];

//PROCESSES
static PCB_t PCBStack[NUMPROCESSES];
static uint8_t CurrentProcesses[NUMPROCESSES];
static uint32_t ProcessCount;

// Mailbox semaphores
Sema4Type DataValid;
Sema4Type BoxFree;
// Mailbox Data
uint32_t MailboxData;

// OS FIFO
AddIndexFifo(OS, OSFIFOSIZE, uint32_t, FIFOSUCCESS, FIFOFAIL);
// OS FIFO semaphore
Sema4Type OSFIFODataLeft;

// Indicates whether OS has started
static uint8_t OS_Active = 0;

// User defined time slice
uint32_t TimeSlice;

// will switch with equal priority
static TCB_t* FindNextRunLax(void) {
  TCB_t* next = RunPt->next;
#if PRI
  if(next == NULL) {
    return NextRunPt;
  }
  while(next != RunPt){
    if(RunPt->priority == next->priority) { //TODO: shouldn't need >, but seems to work
      return next;
    }
    next = next->next;
  }
  return RunPt;
#else
  if(next == NULL) {
    return NextRunPt;
  }
  else {
    return next;
  }
#endif
}

static TCB_t* GetPrevious(TCB_t* tcb) {
  TCB_t* temp = tcb->next;
  while(temp->next != tcb) {
    temp = temp->next;
  }
  
  return temp;
}

// will switch to thread with highest priority
static TCB_t* FindNextRunReq(void) {
  TCB_t* next = RunPt->next;
#if PRI
  if(next == NULL) {
    return NextRunPt;
  }
  while(next != RunPt){
    if(RunPt->priority == next->priority) {
      return next;
    }
    next = next->next;
  }

  return RunPt->next;
#else
  if(next == NULL) {
    return NextRunPt;
  }
  else {
    return next;
  }
#endif
}

// returns 1 if higher priority than RunPt
// returns 0 if lower priority than RunPt
static uint8_t InsertIntoActive(TCB_t* tcb) {
  // insert semaPt->head to linked list (by priority)
  if(RunPt->next == NULL || RunPt->status == 1 || RunPt->sleep_state != 0 || RunPt != NextRunPt){
    // in the process of being blocked, sleeped, or killed
#if PRI
    if(tcb->priority < NextRunPt->priority) {
      GetPrevious(NextRunPt)->next = tcb;
      tcb->next = NextRunPt;
      NextRunPt = tcb;
      return 0;   // context switch helper already called, don't need to trigger again RunPt->status != 1
    }
    
    else {
      TCB_t* testThread = NextRunPt->next;
      while(testThread != NextRunPt) {
        if(tcb->priority < testThread->priority) {
          GetPrevious(testThread)->next = tcb;
          tcb->next = testThread;
          break;
        }
        else{
          testThread = testThread->next;
        }
      }
    
      // insert at end of lowest priority
      if(testThread == NextRunPt) {
        GetPrevious(NextRunPt)->next = tcb;
        tcb->next = NextRunPt;
      }
    
      return 0;
    }
#else
    GetPrevious(NextRunPt)->next = tcb;
    tcb->next = NextRunPt;
    return 0;
#endif
  }
  else {
#if PRI
    if(tcb->priority < RunPt->priority) {
      GetPrevious(RunPt)->next = tcb;
      tcb->next = RunPt;
      NextRunPt = tcb;
      return 1;
    }
    else {
      TCB_t* testThread = RunPt->next;
      while(testThread != RunPt) {
        if(tcb->priority < testThread->priority) {
          GetPrevious(testThread)->next = tcb;
          tcb->next = testThread;
          break;
        }
        else{
          testThread = testThread->next;
        }
      }
    
      // insert at end of lowest priority
      if(testThread == RunPt) {
        GetPrevious(RunPt)->next = tcb;
        tcb->next = RunPt;
      }
    
      return 0;
    }
#else
  GetPrevious(RunPt)->next = tcb;
  tcb->next = RunPt;
  return 0;
#endif
  }
}

static void ContextSwitchHelper(void) {
  // make sure next thread is valid
  if(ActiveThreads == 0) {
    return;
  }
  
  if(NextRunPt == RunPt) {
    RunPt->elapsedTime = 0;
    NVIC_ST_CURRENT_R = 0;
    return;
  }
  else{
    RunPt->elapsedTime = TimeSlice - NVIC_ST_CURRENT_R; // save elapsed time so far
    ContextSwitch();
  }
  return; // no available thread to switch to, do nothing
}

/*------------------------------------------------------------------------------
  Systick Interrupt Handler
  SysTick interrupt happens every 10 ms
  used for preemptive thread switch
 *------------------------------------------------------------------------------*/
void SysTick_Handler(void) {
  long sr = StartCritical();
  if(RunPt->next == NULL || RunPt->status == 1 || RunPt->sleep_state != 0) {
    EndCritical(sr);
    return;
  }  
  else {
    if(NextRunPt != RunPt) {
      EndCritical(sr);
      return;
    }
    NextRunPt = FindNextRunLax();
    if(NextRunPt == RunPt) {
      EndCritical(sr);
      return;
    }
    else{
      ContextSwitchHelper();
    }
  }
  
  EndCritical(sr);
} // end SysTick_Handler

unsigned long OS_LockScheduler(void){
  // lab 4 might need this for disk formating
  return 0;// replace with solution
}
void OS_UnLockScheduler(unsigned long previous){
  // lab 4 might need this for disk formating
}


void SysTick_Init(unsigned long period){
  // period in ms
  NVIC_ST_CTRL_R = 0;                   // disable SysTick during setup
  NVIC_ST_RELOAD_R = period;            // reload value
  NVIC_ST_CURRENT_R = 0;                // any write to current clears it
                                        // enable SysTick with core clock
  NVIC_ST_CTRL_R = NVIC_ST_CTRL_ENABLE+NVIC_ST_CTRL_CLK_SRC+NVIC_ST_CTRL_INTEN;
}

void Timer5A_Init() {
  SYSCTL_RCGCTIMER_R |= 0x20;      // 0) activate timer5
  TIMER5_CTL_R &= ~0x00000001;     // 1) disable timer5A during setup
  TIMER5_CFG_R = 0x00000000;       // 2) configure for 32-bit timer mode
  TIMER5_TAMR_R = 0x00000002;      // 3) configure for periodic mode, default down-count settings
  TIMER5_TAILR_R = 80000-1;        // 4) reload value
  TIMER5_TAPR_R = 0;               // 5) 12.5ns timer5A
  TIMER5_ICR_R = 0x00000001;       // 6) clear timer5A timeout flag
  TIMER5_IMR_R |= 0x00000001;      // 7) arm timeout interrupt
  NVIC_PRI23_R = (NVIC_PRI23_R&0xFFFFFF00)|(0<<5); // 92 = 23*4
  NVIC_EN2_R |= 1 << 28;         // 9) enable IRQ 92 in NVIC
  TIMER5_CTL_R |= 0x00000001;      // 10) enable timer5A
}

/**
 * @details  Initialize operating system, disable interrupts until OS_Launch.
 * Initialize OS controlled I/O: serial, ADC, systick, LaunchPad I/O and timers.
 * Interrupts not yet enabled.
 * @param  none
 * @return none
 * @brief  Initialize OS
 */
void OS_Init(void){
  // put Lab 2 (and beyond) solution here
  DisableInterrupts();
  PLL_Init(Bus80MHz);
  ST7735_InitR(INITR_REDTAB); // LCD initialization
  LaunchPad_Init();  // debugging profile on PF1
  //ADC_Init(3);
  UART_Init();
  Timer5A_Init();
  OS_ClearMsTime();
  // eFile_Init();
  Heap_Init();
}; 

// ******** OS_InitSemaphore ************
// initialize semaphore 
// input:  pointer to a semaphore
// output: none
void OS_InitSemaphore(Sema4Type *semaPt, int32_t value){
  // put Lab 2 (and beyond) solution here
  semaPt->Value = value;
  semaPt->head = NULL;
}; 

/*
 * @brief Insert thread into blocked linked list
 */
void OS_InsertIntoBlocked(Sema4Type *semaPt) {
  TCB_t* thread = semaPt->head;

#if PRI
  if(RunPt->priority < thread->priority) {
    GetPrevious(RunPt)->next = RunPt->next;
    RunPt->next = semaPt->head;
    semaPt->head = RunPt;
    return;
  }
  else {
    TCB_t* previous = thread;
    thread = thread->next;
    while(thread != NULL) {
      if(RunPt->priority < thread->priority) {
        GetPrevious(RunPt)->next = RunPt->next;
        RunPt->next = thread;
        previous->next = RunPt;
        break;
      }
      else{
        previous = thread;
        thread = thread->next;
      }
    }
        
    // test if null, then append at end
    if(thread == NULL) {
      GetPrevious(RunPt)->next = RunPt->next;
      RunPt->next = NULL;
      previous->next = RunPt;
    }
  }
#else
  GetPrevious(RunPt)->next = RunPt->next;
  while(thread->next != NULL) {
    thread = thread->next;
  }
  thread->next = RunPt;
  RunPt->next = NULL;
#endif
}

// ******** OS_Wait ************
// decrement semaphore 
// Lab2 spinlock
// Lab3 block if less than zero
// input:  pointer to a counting semaphore
// output: none
void OS_Wait(Sema4Type *semaPt){
  // put Lab 2 (and beyond) solution here
  DisableInterrupts();
  semaPt->Value--;
  
  if(semaPt->Value < 0) {
    RunPt->status = 1;
    NextRunPt = FindNextRunReq(); // need to find next AVAILABLE highest priority thread
    
    // insert into blocked linked list
    if(semaPt->head == NULL) {
      GetPrevious(RunPt)->next = RunPt->next;
      semaPt->head = RunPt;
      RunPt->next = NULL;
    }
    else {
      OS_InsertIntoBlocked(semaPt);
    }
    
    ActiveThreads--;
    ContextSwitchHelper();
  }
  
  EnableInterrupts();
}; 

// ******** OS_Signal ************
// increment semaphore 
// Lab2 spinlock
// Lab3 wakeup blocked thread if appropriate 
// input:  pointer to a counting semaphore
// output: none
void OS_Signal(Sema4Type *semaPt){
  // put Lab 2 (and beyond) solution here
  long sr = StartCritical();
  semaPt->Value++;
  
  if(semaPt->Value <= 0) {
    TCB_t* nextHead = semaPt->head->next;
    semaPt->head->status = 0;
    
    // insert semaPt->head to linked list (by priority)
    if(InsertIntoActive(semaPt->head)) {
      ContextSwitchHelper();
    }
    ActiveThreads++;
    semaPt->head = nextHead;
  }
  EndCritical(sr);
}; 

// ******** OS_bWait ************
// Lab2 spinlock, set to 0
// Lab3 block if less than zero
// input:  pointer to a binary semaphore
// output: none
void OS_bWait(Sema4Type *semaPt){
  // put Lab 2 (and beyond) solution here
  DisableInterrupts();
  
  if(semaPt->Value == 0) {
    RunPt->status = 1;
    NextRunPt = FindNextRunReq();
    
    // insert into blocked linked list
    if(semaPt->head == NULL) {
      GetPrevious(RunPt)->next = RunPt->next;
      semaPt->head = RunPt;
      RunPt->next = NULL;
    }
    else {
      OS_InsertIntoBlocked(semaPt);
    }
    
    ActiveThreads--;
    ContextSwitchHelper();
  }
  else{
    semaPt->Value = 0;
  }
  EnableInterrupts();
}; 

// ******** OS_bSignal ************
// Lab2 spinlock, set to 1
// Lab3 wakeup blocked thread if appropriate 
// input:  pointer to a binary semaphore
// output: none
void OS_bSignal(Sema4Type *semaPt){
  // put Lab 2 (and beyond) solution here
  long sr = StartCritical();
  
  if(semaPt->head != NULL) {
    TCB_t* nextHead = semaPt->head->next;
    semaPt->head->status = 0;
    
    // insert semaPt->head to linked list (by priority)
    if(InsertIntoActive(semaPt->head)) {
      ContextSwitchHelper();
    }
    
    ActiveThreads++;
    semaPt->head = nextHead;
  }
  else {
    semaPt->Value = 1;
  }
  EndCritical(sr);
}; 

//**********OS_AddThread_Process*********
int OS_AddThread_Process(void(*task)(void), 
  uint32_t stackSize, uint32_t priority, PCB_t* parent) {
  long sr = StartCritical();
  TCB_t* TCB;
	if(ThreadCount > NUMTHREADS-1) {
    EndCritical(sr);
		return 0;
	}
  else{    
    int thread_location;
		// Find empty thread
    for(thread_location = 0; thread_location < NUMTHREADS; thread_location++) {
      if(CurrentThreads[thread_location] == 0) {
        
        // Claim this location
        TCB = &TCBStack[thread_location];
        break;
      }
    }
    
    TCB->id = thread_location;
    if(parent == NULL) {
      TCB->priority = priority;
    }
    else {
      
    }
    TCB->parent = parent;
    // insert into end of circular linked list
    if(RunPt == NULL) {
      RunPt = TCB;
      RunPt->next = RunPt;
    }
    else{
#if PRI
      // insert with respect to priority
      if(OS_Active && (RunPt->next == NULL || RunPt->status == 1 || RunPt->sleep_state != 0 || RunPt != NextRunPt)) {
        if(TCB->priority < NextRunPt->priority) {
          GetPrevious(NextRunPt)->next = TCB;
          TCB->next = NextRunPt;
          NextRunPt = TCB;
        }
        else {
          TCB_t* testThread = NextRunPt->next;
          TCB_t* previousThread = NextRunPt;
          while(testThread != NextRunPt){
            if(TCB->priority < testThread->priority) {
              previousThread->next = TCB;
              TCB->next = testThread;
              break;
            }
            else{
              previousThread = testThread;
              testThread = testThread->next;
            }
          };
        
          if(testThread == NextRunPt) {
            previousThread->next = TCB;
            TCB->next = NextRunPt;
          }
        }
      }
      else{
        if(TCB->priority < RunPt->priority) {
          GetPrevious(RunPt)->next = TCB;
          TCB->next = RunPt;
          //RunPt = TCB;
          if(OS_Active) {
            NextRunPt = TCB;
            ContextSwitchHelper();
          }
          else {
            RunPt = TCB;
          }
        }
        else {
          TCB_t* testThread = RunPt->next;
          TCB_t* previousThread = RunPt;
          while(testThread != RunPt){
            if(TCB->priority < testThread->priority) {
              previousThread->next = TCB;
              TCB->next = testThread;
              break;
            }
            else{
              previousThread = testThread;
              testThread = testThread->next;
            }
          };
        
          if(testThread == RunPt) {
            previousThread->next = TCB;
            TCB->next = RunPt;
          }
        }
      }
#else
      if(OS_Active && (RunPt->next == NULL || RunPt->status == 1 || RunPt->sleep_state != 0 || RunPt != NextRunPt)) {
        GetPrevious(NextRunPt)->next = TCB;
        TCB->next = NextRunPt;
      }
      else {
        GetPrevious(RunPt)->next = TCB;
        TCB->next = RunPt;
      }
#endif
    }
    
    TCB->sleep_state = 0;
    TCB->sp = &stack[thread_location][STACKSIZE];
    TCB->elapsedTime = 0;
    
    // simulate "pushing" registers onto stack
    *(--(TCB->sp)) = 0x01000000;               // PSR (Thumb bit)
    *(--(TCB->sp)) = (long) task;              // R15 (PC)
    *(--(TCB->sp)) = 0x14141414;               // R14 (LR)
    *(--(TCB->sp)) = 0x12121212;               // R12
    *(--(TCB->sp)) = 0x03030303;               // R3
    *(--(TCB->sp)) = 0x02020202;               // R2
    *(--(TCB->sp)) = 0x01010101;               // R1
    *(--(TCB->sp)) = 0x00000000;               // R0
    *(--(TCB->sp)) = 0x11111111;               // R11
    *(--(TCB->sp)) = 0x10101010;               // R10
    if(parent == NULL) {
      *(--(TCB->sp)) = 0x09090909;               // R9
    }
    else {
      *(--(TCB->sp)) = (uint32_t) parent->data;
    }
    *(--(TCB->sp)) = 0x08080808;               // R8
    *(--(TCB->sp)) = 0x07070707;               // R7
    *(--(TCB->sp)) = 0x06060606;               // R6
    *(--(TCB->sp)) = 0x05050505;               // R5
    *(--(TCB->sp)) = 0x04040404;               // R4
    
    ThreadCount++;
    ActiveThreads++;
    CurrentThreads[thread_location] = 1;
	} 
  
  // If running, trigger context switch
  EndCritical(sr);
  return 1;
}

//******** OS_AddThread *************** 
// add a foregound thread to the scheduler
// Inputs: pointer to a void/void foreground task
//         number of bytes allocated for its stack
//         priority, 0 is highest, 5 is the lowest
// Outputs: 1 if successful, 0 if this thread can not be added
// stack size must be divisable by 8 (aligned to double word boundary)
// In Lab 2, you can ignore both the stackSize and priority fields
// In Lab 3, you can ignore the stackSize fields
int OS_AddThread(void(*task)(void), 
   uint32_t stackSize, uint32_t priority){
  if(RunPt != NULL) {
   return OS_AddThread_Process(task, stackSize, priority, RunPt->parent);
  }
  else {
   return OS_AddThread_Process(task, stackSize, priority, NULL); //OS thread, does not have a parent
  }
};

//******** OS_AddProcess *************** 
// add a process with foregound thread to the scheduler
// Inputs: pointer to a void/void entry point
//         pointer to process text (code) segment
//         pointer to process data segment
//         number of bytes allocated for its stack
//         priority (0 is highest)
// Outputs: 1 if successful, 0 if this process can not be added
// This function will be needed for Lab 5
// In Labs 2-4, this function can be ignored
int OS_AddProcess(void(*entry)(void), void *text, void *data, 
  unsigned long stackSize, unsigned long priority){
  // put Lab 5 solution here
  long sr = StartCritical();
  PCB_t* pcb;
  // look for open process spot
  if(ProcessCount == NUMPROCESSES) {
    EndCritical(sr);
    return 0;
  }
  for(int i = 0; i < NUMPROCESSES; i++){
    if(CurrentProcesses[i] == 0) {
      // claim this location
      pcb = &PCBStack[i];
      CurrentProcesses[i] = 1;
      break;
    }
  }
  pcb->text = text; //not sure what the point of text is
  pcb->data = data;
  int x = OS_AddThread_Process(entry, stackSize, priority, pcb);
  EndCritical(sr);
  return x; // replace this line with Lab 5 solution
}


//******** OS_Id *************** 
// returns the thread ID for the currently running thread
// Inputs: none
// Outputs: Thread ID, number greater than zero 
uint32_t OS_Id(void){
  // put Lab 2 (and beyond) solution here
  return RunPt->id;
};

//******** OS_AddPeriodicThread *************** 
// add a background periodic task
// typically this function receives the highest priority
// Inputs: pointer to a void/void background function
//         period given in system time units (12.5ns)
//         priority 0 is the highest, 5 is the lowest
// Outputs: 1 if successful, 0 if this thread can not be added
// You are free to select the time resolution for this function
// It is assumed that the user task will run to completion and return
// This task can not spin, block, loop, sleep, or kill
// This task can call OS_Signal  OS_bSignal   OS_AddThread
// This task does not have a Thread ID
// In lab 1, this command will be called 1 time
// In lab 2, this command will be called 0 or 1 times
// In lab 2, the priority field can be ignored
// In lab 3, this command will be called 0 1 or 2 times
// In lab 3, there will be up to four background threads, and this priority field 
//           determines the relative priority of these four threads
int OS_AddPeriodicThread(void(*task)(void), 
   uint32_t period, uint32_t priority){
  // put Lab 2 (and beyond) solution here
  static uint8_t num_periodicthreads = 0;
  // Init timer
  if(num_periodicthreads == 0) {
    Timer4A_Init(task, period, priority);
    num_periodicthreads = 1;
  }
  else if (num_periodicthreads == 1) {
    Timer3A_Init(task, period, priority);
  }
  else {
    return 0;
  }
  return 1;
};

void (*APeriodicTaskSW1)(void); 
void (*APeriodicTaskSW2)(void);

/*----------------------------------------------------------------------------
  PF1 Interrupt Handler
 *----------------------------------------------------------------------------*/
void GPIOPortF_Handler(void){
  // Heartbeat for PF4 = PC4
  if(GPIO_PORTF_RIS_R&0x10) { //PF4
    GPIO_PORTF_ICR_R = 0x10;
    APeriodicTaskSW1();
  }
  else if(GPIO_PORTF_RIS_R&0x01) { //PF0
    GPIO_PORTF_ICR_R = 0x01;
    APeriodicTaskSW2();
  }
}

//******** OS_AddSW1Task *************** 
// add a background task to run whenever the SW1 (PF4) button is pushed
// Inputs: pointer to a void/void background function
//         priority 0 is the highest, 5 is the lowest
// Outputs: 1 if successful, 0 if this thread can not be added
// It is assumed that the user task will run to completion and return
// This task can not spin, block, loop, sleep, or kill
// This task can call OS_Signal  OS_bSignal   OS_AddThread
// This task does not have a Thread ID
// In labs 2 and 3, this command will be called 0 or 1 times
// In lab 2, the priority field can be ignored
// In lab 3, there will be up to four background threads, and this priority field 
//           determines the relative priority of these four threads
int OS_AddSW1Task(void(*task)(void), uint32_t priority){
  // put Lab 2 (and beyond) solution here
  APeriodicTaskSW1 = task;
  SYSCTL_RCGCGPIO_R |= 0x00000020; // (a) activate clock for port F
  GPIO_PORTF_DIR_R &= ~0x10;    // (c) make PF4 in (built-in button)
  GPIO_PORTF_AFSEL_R &= ~0x10;  //     disable alt funct on PF4
  GPIO_PORTF_DEN_R |= 0x10;     //     enable digital I/O on PF4   
  GPIO_PORTF_PCTL_R &= ~0x000F0000; // configure PF4 as GPIO
  GPIO_PORTF_AMSEL_R = 0;       //     disable analog functionality on PF
  GPIO_PORTF_PUR_R |= 0x10;     //     enable weak pull-up on PF4
  GPIO_PORTF_IS_R &= ~0x10;     // (d) PF4 is edge-sensitive
  GPIO_PORTF_IBE_R &= ~0x10;    //     PF4 is not both edges
  GPIO_PORTF_IEV_R &= ~0x10;    //     PF4 falling edge event
  GPIO_PORTF_ICR_R = 0x10;      // (e) clear flag4
  GPIO_PORTF_IM_R |= 0x10;      // (f) arm interrupt on PF4 *** No IME bit as mentioned in Book ***
  NVIC_PRI7_R = (NVIC_PRI7_R&0xFF00FFFF)|(priority << 15);
  NVIC_EN0_R = 0x40000000;      // (h) enable interrupt 30 in NVIC
  return 1; // replace this line with solution
};

//******** OS_AddSW2Task *************** 
// add a background task to run whenever the SW2 (PF0) button is pushed
// Inputs: pointer to a void/void background function
//         priority 0 is highest, 5 is lowest
// Outputs: 1 if successful, 0 if this thread can not be added
// It is assumed user task will run to completion and return
// This task can not spin block loop sleep or kill
// This task can call issue OS_Signal, it can call OS_AddThread
// This task does not have a Thread ID
// In lab 2, this function can be ignored
// In lab 3, this command will be called will be called 0 or 1 times
// In lab 3, there will be up to four background threads, and this priority field 
//           determines the relative priority of these four threads
int OS_AddSW2Task(void(*task)(void), uint32_t priority){
  // put Lab 2 (and beyond) solution here
  APeriodicTaskSW2 = task;
  SYSCTL_RCGCGPIO_R |= 0x00000020; // (a) activate clock for port F
  GPIO_PORTF_DIR_R &= ~0x01;    // (c) make PF0 in (built-in button)
  GPIO_PORTF_AFSEL_R &= ~0x01;  //     disable alt funct on PF0
  GPIO_PORTF_DEN_R |= 0x01;     //     enable digital I/O on PF0   
  GPIO_PORTF_PCTL_R &= ~0x0000000F; // configure PF0 as GPIO
  GPIO_PORTF_AMSEL_R = 0;       //     disable analog functionality on PF
  GPIO_PORTF_PUR_R |= 0x01;     //     enable weak pull-up on PF0
  GPIO_PORTF_IS_R &= ~0x01;     // (d) PF0 is edge-sensitive
  GPIO_PORTF_IBE_R &= ~0x01;    //     PF0 is not both edges
  GPIO_PORTF_IEV_R &= ~0x01;    //     PF0 falling edge event
  GPIO_PORTF_ICR_R = 0x01;      // (e) clear flag4
  GPIO_PORTF_IM_R |= 0x01;      // (f) arm interrupt on PF0 *** No IME bit as mentioned in Book ***
  NVIC_PRI7_R = (NVIC_PRI7_R&0xFF00FFFF)|(priority << 15);
  NVIC_EN0_R = 0x40000000;      // (h) enable interrupt 30 in NVIC
  return 1; // replace this line with solution
};


// ******** OS_Sleep ************
// place this thread into a dormant state
// input:  number of msec to sleep
// output: none
// You are free to select the time resolution for this function
// OS_Sleep(0) implements cooperative multitasking
void OS_Sleep(uint32_t sleepTime){
  // put Lab 2 (and beyond) solution here
  long sr = StartCritical();
  RunPt->sleep_state = sleepTime;
  NextRunPt = FindNextRunReq();
  
  // insert into sleep linked list (order does not matter here)
  if(sleepTime != 0){
    if(SleepPt == NULL) {
      SleepPt = RunPt;
      GetPrevious(RunPt)->next = RunPt->next;
      RunPt->next = NULL;
    }
    else {
      GetPrevious(RunPt)->next = RunPt->next;
      RunPt->next = SleepPt;
      SleepPt = RunPt;
    }
  }
  ActiveThreads--;
  ContextSwitchHelper();
  EndCritical(sr);
};  

// ******** OS_Kill ************
// kill the currently running thread, release its TCB and stack
// input:  none
// output: none
void OS_Kill(void){
  // put Lab 2 (and beyond) solution here
  DisableInterrupts();

  // free this location
  CurrentThreads[RunPt->id] = 0;
  
  ThreadCount--;
  ActiveThreads--;
  
  NextRunPt = FindNextRunReq();
  GetPrevious(RunPt)->next = RunPt->next;
  RunPt->next = NULL;
  // free text and data from heap if last thread in process
  if(RunPt->parent != NULL) {
    // see if other threads are using this process - need to look in active, slept, and blocked
    uint16_t i = 0;
    for(i = 0; i < NUMTHREADS; i++) {
      if(CurrentThreads[i] == 1) {
        if(TCBStack[i].parent == RunPt->parent) {
          break; //still in use
        }
      }
    }
    
    if(i == 10) {
      //no other threads are part of this process, free heap
      Heap_Free(RunPt->parent->data);
      Heap_Free(RunPt->parent->text);
    }
  }
  ContextSwitchHelper();
  EnableInterrupts();   // end of atomic section  
  //while(1){};
}; 

// ******** OS_Suspend ************
// suspend execution of currently running thread
// scheduler will choose another thread to execute
// Can be used to implement cooperative multitasking 
// Same function as OS_Sleep(0)
// input:  none
// output: none
void OS_Suspend(void){
  // put Lab 2 (and beyond) solution here
  // 1 -> 2 => 2 -> 1
  OS_Sleep(0);
};
  
// ******** OS_Fifo_Init ************
// Initialize the Fifo to be empty
// Inputs: size
// Outputs: none 
// In Lab 2, you can ignore the size field
// In Lab 3, you should implement the user-defined fifo size
// In Lab 3, you can put whatever restrictions you want on size
//    e.g., 4 to 64 elements
//    e.g., must be a power of 2,4,8,16,32,64,128
void OS_Fifo_Init(uint32_t size){
  // put Lab 2 (and beyond) solution here
  OSFifo_Init();
  OS_InitSemaphore(&OSFIFODataLeft, 0);
};

// ******** OS_Fifo_Put ************
// Enter one data sample into the Fifo
// Called from the background, so no waiting 
// Inputs:  data
// Outputs: true if data is properly saved,
//          false if data not saved, because it was full
// Since this is called by interrupt handlers 
//  this function can not disable or enable interrupts
int OS_Fifo_Put(uint32_t data){
  // put Lab 2 (and beyond) solution here
  if(OSFifo_Put(data) == FIFOFAIL) {  
    return FIFOFAIL;
  } 
  OS_Signal(&OSFIFODataLeft);
  return FIFOSUCCESS;
};  

// ******** OS_Fifo_Get ************
// Remove one data sample from the Fifo
// Called in foreground, will spin/block if empty
// Inputs:  none
// Outputs: data 
uint32_t OS_Fifo_Get(void){
  // put Lab 2 (and beyond) solution here
  OS_Wait(&OSFIFODataLeft);
  uint32_t data;
  OSFifo_Get(&data);
  return data;
};

// ******** OS_Fifo_Size ************
// Check the status of the Fifo
// Inputs: none
// Outputs: returns the number of elements in the Fifo
//          greater than zero if a call to OS_Fifo_Get will return right away
//          zero or less than zero if the Fifo is empty 
//          zero or less than zero if a call to OS_Fifo_Get will spin or block
int32_t OS_Fifo_Size(void){
  // put Lab 2 (and beyond) solution here
  return OSFifo_Size();
};


// ******** OS_MailBox_Init ************
// Initialize communication channel
// Inputs:  none
// Outputs: none
void OS_MailBox_Init(void){
  // put Lab 2 (and beyond) solution here
  OS_InitSemaphore(&DataValid, 0);
  OS_InitSemaphore(&BoxFree, 1);
};

// ******** OS_MailBox_Send ************
// enter mail into the MailBox
// Inputs:  data to be sent
// Outputs: none
// This function will be called from a foreground thread
// It will spin/block if the MailBox contains data not yet received 
void OS_MailBox_Send(uint32_t data){
  // put Lab 2 (and beyond) solution here
  OS_bWait(&BoxFree);
  MailboxData = data;
  OS_bSignal(&DataValid);
};

// ******** OS_MailBox_Recv ************
// remove mail from the MailBox
// Inputs:  none
// Outputs: data received
// This function will be called from a foreground thread
// It will spin/block if the MailBox is empty 
uint32_t OS_MailBox_Recv(void){
  // put Lab 2 (and beyond) solution here
  OS_bWait(&DataValid);
  uint32_t data = MailboxData;
  OS_bSignal(&BoxFree);
  return data;
};

// ******** OS_Time ************
// return the system time 
// Inputs:  none
// Outputs: time in 12.5ns units, 0 to 4294967295
// The time resolution should be less than or equal to 1us, and the precision 32 bits
// It is ok to change the resolution and precision of this function as long as 
//   this function and OS_TimeDifference have the same resolution and precision 
uint32_t OS_Time(void){
  return (msSystemTime*80000)+(79999-TIMER5_TAR_R);
};

// ******** OS_TimeDifference ************
// Calculates difference between two times
// Inputs:  two times measured with OS_Time
// Outputs: time difference in 12.5ns units 
// The time resolution should be less than or equal to 1us, and the precision at least 12 bits
// It is ok to change the resolution and precision of this function as long as 
//   this function and OS_Time have the same resolution and precision 
uint32_t OS_TimeDifference(uint32_t start, uint32_t stop){
  // only accounts for one loop
  if(start > stop){
		return (80000*10000) - (start-stop);
	}else{
		return stop - start;
	}
};


// ******** OS_ClearMsTime ************
// sets the system time to zero (solve for Lab 1), and start a periodic interrupt
// Inputs:  none
// Outputs: none
// You are free to change how this works
void OS_ClearMsTime(void){
  // put Lab 1 solution here
	msSystemTime = 0;
	tensecSystemTime = 0;
  return; // replace this line with solution
};

void Timer5A_Handler(void){
  long sr = StartCritical();
  TIMER5_ICR_R = 0x01;         // acknowledge timer0A timeout
  if(msSystemTime == 9999){
		msSystemTime = 0;
		tensecSystemTime++;
	}else{
		msSystemTime++;                // execute user task
	}
  
  if(SleepPt == NULL) {
    EndCritical(sr);
    return;
  }
  
  TCB_t* previous = NULL;
  TCB_t* next = SleepPt;
  int result = 0;
  if(SleepPt == RunPt) { // hasn't been moved yet
    EndCritical(sr);
    return;
  }
  do{
    // Decrement sleep_state if > 0
    next->sleep_state--;
    next = next->next;
  }while(next != NULL);
  
  // wake up threads if needed
  next = SleepPt;
  do{
    if(next->sleep_state == 0) {
      TCB_t* temp_next = next->next;
      
      if(SleepPt == next) {
        SleepPt = temp_next;
      }
      else {
        previous->next = temp_next;
      }
      if(InsertIntoActive(next)) {
        result = 1;
      }
      next = temp_next;
      ActiveThreads++;
    }
    else {
      previous = next;
      next = next->next;
    }
  }while(next != NULL);
#if PRI
  if(result == 1){
    ContextSwitchHelper();
  }
#endif
  EndCritical(sr);
}

// ******** OS_MsTime ************
// reads the current time in msec (solve for Lab 1)
// Inputs:  none
// Outputs: time in ms units
// You are free to select the time resolution for this function
// For Labs 2 and beyond, it is ok to make the resolution to match the first call to OS_AddPeriodicThread
uint32_t OS_MsTime(void){
  // put Lab 1 solution here
	return msSystemTime;
};


//******** OS_Launch *************** 
// start the scheduler, enable interrupts
// Inputs: number of 12.5ns clock cycles for each time slice
//         you may select the units of this parameter
// Outputs: none (does not return)
// In Lab 2, you can ignore the theTimeSlice field
// In Lab 3, you should implement the user-defined TimeSlice field
// It is ok to limit the range of theTimeSlice to match the 24-bit SysTick
void OS_Launch(uint32_t theTimeSlice){
  // put Lab 2 (and beyond) solution here
  SysTick_Init(theTimeSlice);
  TimeSlice = theTimeSlice;
  OS_Active = 1;
  NextRunPt = RunPt;
  StartOS(RunPt->sp);
};

//************** I/O Redirection *************** 
// redirect terminal I/O to UART or file (Lab 4)

int StreamToDevice=0;                // 0=UART, 1=stream to file (Lab 4)

int fputc (int ch, FILE *f) { 
  if(StreamToDevice==1){  // Lab 4
    if(eFile_Write(ch)){          // close file on error
       OS_EndRedirectToFile(); // cannot write to file
       return 1;                  // failure
    }
    return 0; // success writing
  }
  
  // default UART output
  UART_OutChar(ch);
  return ch; 
}

int fgetc (FILE *f){
  char ch = UART_InChar();  // receive from keyboard
  UART_OutChar(ch);         // echo
  return ch;
}

int OS_RedirectToFile(const char *name){  // Lab 4
  eFile_Create(name);              // ignore error if file already exists
  if(eFile_WOpen(name)) return 1;  // cannot open file
  StreamToDevice = 1;
  return 0;
}

int OS_EndRedirectToFile(void){  // Lab 4
  StreamToDevice = 0;
  if(eFile_WClose()) return 1;    // cannot close file
  return 0;
}

int OS_RedirectToUART(void){
  StreamToDevice = 0;
  return 0;
}

int OS_RedirectToST7735(void){
  
  return 1;
}

