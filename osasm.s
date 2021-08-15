;/*****************************************************************************/
;/* OSasm.s: low-level OS commands, written in assembly                       */
;/* derived from uCOS-II                                                      */
;/*****************************************************************************/
;Jonathan Valvano, OS Lab2/3/4/5, 1/12/20
;Students will implement these functions as part of EE445M/EE380L.12 Lab

        AREA |.text|, CODE, READONLY, ALIGN=2
        THUMB
        REQUIRE8
        PRESERVE8

        EXTERN  RunPt            ; currently running thread
        EXTERN  NextRunPt        ; next thread to run
        EXTERN  TimeSlice        ; user defined time slice
        EXPORT  StartOS
        EXPORT  ContextSwitch
        EXPORT  PendSV_Handler
        EXPORT  SVC_Handler


NVIC_INT_CTRL   EQU     0xE000ED04                              ; Interrupt control state register.
NVIC_SYSPRI14   EQU     0xE000ED22                              ; PendSV priority register (position 14).
NVIC_SYSPRI15   EQU     0xE000ED23                              ; Systick priority register (position 15).
NVIC_LEVEL14    EQU           0xEF                              ; Systick priority value (second lowest).
NVIC_LEVEL15    EQU           0xFF                              ; PendSV priority value (lowest).
NVIC_PENDSVSET  EQU     0x10000000                              ; Value to trigger PendSV exception.
NVIC_ST_CURRENT_R     EQU 0xE000E018
NVIC_ST_RELOAD_R      EQU 0xE000E014

StartOS
; put your code here
    ; set systick and pendsv priority
    LDR R1, =NVIC_SYSPRI15
    LDR R2, =NVIC_LEVEL14
    STR R2, [R1]
    LDR R1, =NVIC_SYSPRI14
    LDR R2, =NVIC_LEVEL15
    STR R2, [R1]
    ; pop regs for first thread
    MOV SP, R0
    POP {R4-R11}
    POP {R0-R3}
    POP {R12}
    POP {LR}    ; discard LR
    POP {LR}
    POP {R1}    ; discard PSR
    CPSIE I
    BX      LR                 ; start first thread

OSStartHang
    B       OSStartHang        ; Should never get here


;********************************************************************************************************
;                               PERFORM A CONTEXT SWITCH (From task level)
;                                           void ContextSwitch(void)
;
; Note(s) : 1) ContextSwitch() is called when OS wants to perform a task context switch.  This function
;              triggers the PendSV exception which is where the real work is done.
;********************************************************************************************************

ContextSwitch
; edit this code
    ;CPSID I
    LDR R0, =NVIC_INT_CTRL
    LDR R1, =NVIC_PENDSVSET
    STR R1, [R0]
    ;CPSIE I
    BX      LR
    

;********************************************************************************************************
;                                         HANDLE PendSV EXCEPTION
;                                     void OS_CPU_PendSVHandler(void)
;
; Note(s) : 1) PendSV is used to cause a context switch.  This is a recommended method for performing
;              context switches with Cortex-M.  This is because the Cortex-M3 auto-saves half of the
;              processor context on any exception, and restores same on return from exception.  So only
;              saving of R4-R11 is required and fixing up the stack pointers.  Using the PendSV exception
;              this way means that context saving and restoring is identical whether it is initiated from
;              a thread or occurs due to an interrupt or exception.
;
;           2) Pseudo-code is:
;              a) Get the process SP, if 0 then skip (goto d) the saving part (first context switch);
;              b) Save remaining regs r4-r11 on process stack;
;              c) Save the process SP in its TCB, OSTCBCur->OSTCBStkPtr = SP;
;              d) Call OSTaskSwHook();
;              e) Get current high priority, OSPrioCur = OSPrioHighRdy;
;              f) Get current ready thread TCB, OSTCBCur = OSTCBHighRdy;
;              g) Get new process SP from TCB, SP = OSTCBHighRdy->OSTCBStkPtr;
;              h) Restore R4-R11 from new process stack;
;              i) Perform exception return which will restore remaining context.
;
;           3) On entry into PendSV handler:
;              a) The following have been saved on the process stack (by processor):
;                 xPSR, PC, LR, R12, R0-R3
;              b) Processor mode is switched to Handler mode (from Thread mode)
;              c) Stack is Main stack (switched from Process stack)
;              d) OSTCBCur      points to the OS_TCB of the task to suspend
;                 OSTCBHighRdy  points to the OS_TCB of the task to resume
;
;           4) Since PendSV is set to lowest priority in the system (by OSStartHighRdy() above), we
;              know that it will only be run when no other exception or interrupt is active, and
;              therefore safe to assume that context being switched out was using the process stack (PSP).
;********************************************************************************************************

PendSV_Handler
    CPSID I
    PUSH {R4-R11}   ; all registers saved
    LDR R0, =RunPt
    LDR R1, [R0]    ; R1 = RunPt
    STR SP, [R1]    ; save (updated) SP into RunPt->sp
    LDR R1, =NextRunPt ; R1 = &NextRunPt
    LDR R1, [R1]       ; R1 = NextRunPt
    STR R1, [R0]       ; RunPt = NextRunPt
    
    LDR R2, [R1,#4]    ; R2 = RunPt->elapsedTime
    LDR R3, =TimeSlice ; R3 = TimeSlice
    LDR R3, [R3]
    SUB R2, R3, R2     ; R2 = TimeSlice - elapsedTime = remainingTime
    LDR R3, =NVIC_ST_RELOAD_R
    STR R2, [R3]               ; Reload = remainingTime
    LDR R3, =NVIC_ST_CURRENT_R ; Current = 0, resets to reload
    STR R2, [R3]
    LDR R3, =TimeSlice  ; restore Reload to TimeSlice
    LDR R3, [R3]
    LDR R2, =NVIC_ST_RELOAD_R
    STR R3, [R2]
    
    ;LDR R1, [R1,#4] ; R1 = RunPt->next
    ;STR R1, [R0]    ; RunPT = RunPt->next (R1)
    LDR SP, [R1]    ; SP = RunPt->sp (the next thread)
    POP {R4-R11}    ; recover new thread's R4-R11
    CPSIE I
    BX      LR                 ; Exception return will restore remaining context (R0-R3, R12, LR, PC, PSR)  
    

;********************************************************************************************************
;                                         HANDLE SVC EXCEPTION
;                                     void OS_CPU_SVCHandler(void)
;
; Note(s) : SVC is a software-triggered exception to make OS kernel calls from user land. 
;           The function ID to call is encoded in the instruction itself, the location of which can be
;           found relative to the return address saved on the stack on exception entry.
;           Function-call paramters in R0..R3 are also auto-saved on stack on exception entry.
;********************************************************************************************************

        IMPORT    OS_Id
        IMPORT    OS_Kill
        IMPORT    OS_Sleep
        IMPORT    OS_Time
        IMPORT    OS_AddThread

SVC_Handler
; put your Lab 5 code here
    LDR R12, [SP,#24] ; return address
    LDRH R12, [R12,#-2] ;svc instruction is 2 bytes
    BIC R12, #0xFF00 ; extract ID from instruction
    LDM SP,{R0-R3} ;get parameters
    CMP R12, #0 ; OS_Id
    BEQ id
    CMP R12, #1 ;OS_Kill
    BEQ kill
    CMP R12, #2
    BEQ sleep
    CMP R12, #3
    BEQ time
    CMP R12, #4
    BEQ addthread
    B end
id
    ; save LR
    PUSH {LR}
    BL OS_Id
    POP {LR}
    B end
kill
    PUSH {LR}
    BL OS_Kill
    POP {LR}
    B end
sleep
    PUSH {LR}
    BL OS_Sleep
    POP {LR}
    B end
time
    PUSH {LR}
    BL OS_Time
    POP {LR}
    B end
addthread
    PUSH {LR}
    BL OS_AddThread
    POP {LR}
end
    STR R0,[SP] ;store R0 at top of stack (return value)
    BX      LR                   ; Return from exception



    ALIGN
    END
