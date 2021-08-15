// *************Interpreter.c**************
// Students implement this as part of EE445M/EE380L.12 Lab 1,2,3,4 
// High-level OS user interface
// 
// Runs on LM4F120/TM4C123
// Jonathan W. Valvano 1/18/20, valvano@mail.utexas.edu
#include <stdint.h>
#include <string.h> 
#include <stdio.h>
#include <stdlib.h>
#include "../inc/tm4c123gh6pm.h"
#include "../inc/CortexM.h"
#include "../inc/PLL.h"
#include "../inc/LaunchPad.h"
#include "../RTOS_Labs_common/OS.h"
#include "../RTOS_Labs_common/ST7735.h"
#include "../inc/ADCT0ATrigger.h"
#include "../inc/ADCSWTrigger.h"
#include "../RTOS_Labs_common/UART0int.h"
#include "../RTOS_Labs_common/eDisk.h"
#include "../RTOS_Labs_common/eFile.h"
#include "../RTOS_Labs_common/ADC.h"
#include "../inc/tm4c123gh6pm.h"
#include "../inc/Launchpad.h"
#include "../RTOS_Lab5_ProcessLoader/loader.h"
#include "../RTOS_Labs_common/esp8266.h"
#include "../inc/Timer1A.h"
#include "../RTOS_Labs_common/heap.h"

#define CMD_NEXT_LINE() \
          UART_OutChar('\n'); \
          UART_OutChar(CR);

uint8_t LCD_screen = 0;
uint8_t LCD_line[2] = {0, 0};

static const ELFSymbol_t symtab[] = {
  {"ST7735_Message", ST7735_Message}
};

// Print jitter histogram
void Jitter(int32_t MaxJitter, uint32_t const JitterSize, uint32_t JitterHistogram[]){
  // write this for Lab 3 (the latest)
	UART_OutString("MaxJitter: ");
  UART_OutUDec(MaxJitter);
  CMD_NEXT_LINE();
  UART_OutString("JitterSize: ");
  UART_OutUDec(JitterSize);
  CMD_NEXT_LINE();
  for(int i = 0; i < JitterSize; i++) {
    UART_OutUDec(JitterHistogram[i]);
    UART_OutChar(' ');
  }
  CMD_NEXT_LINE();
}

void next_line(void) {
  LCD_line[LCD_screen]++;
  if(LCD_line[LCD_screen] == 8) {
    LCD_line[LCD_screen] = 0;
  }
}

void Interpreter_Error(int cause) {
  // Possible Errors (debugging purposes):
  //  Missing/Incorrect command
  //  Missing/Incorrect parameter 
  //  TODO: extra parameter?
  static uint32_t command;
  static uint32_t missing_parameter;
  static uint32_t wrong_parameter;
  
  switch(cause) {
    case 0:
      // Missing/Incorrect command
      command++;
      break;
    case 1:
      missing_parameter++;
    case 2:
      wrong_parameter++;
  }
}

void select_screen(char* screen) {
  if(!strcmp(screen, "top")) {
    LCD_screen = 0;
  }
  else if(!strcmp(screen, "bot")) {
    LCD_screen = 1;
  }
  else {
    // invalid parameter
    Interpreter_Error(2);
  }
}

void print_line(char* string, char* value) {
  ST7735_Message(LCD_screen, LCD_line[LCD_screen], string, atoi(value));
  next_line();
}

void get_adc_value(void) {
  char stop = 0;
  do{
    UART_OutString("ADC: ");
    UART_OutUDec(ADC_In());
    UART_OutChar(CR);
    stop = UART_InCharNonBlock();
  } while(stop == 0);
  CMD_NEXT_LINE();
}

void os_time_op(char* command) {
  if(!strcmp(command, "reset")) {
    OS_ClearMsTime();
  }
  else if(!strcmp(command, "read")) {
    char stop = 0;
    do{
      UART_OutString("OS time: ");
      UART_OutUDec(OS_MsTime());
      UART_OutChar(CR);
      stop = UART_InCharNonBlock();
    } while(stop == 0);
    CMD_NEXT_LINE();
  }
  else {
    // invalid parameter
    Interpreter_Error(2);
  }
}

void led_toggle(void) {
  PF2 ^= 0x04;
}

void help(void) {
  UART_OutString("lcd [bot/top]");
  CMD_NEXT_LINE();
  UART_OutString("print [string] [int]");
  CMD_NEXT_LINE();
  UART_OutString("adc_in");
  CMD_NEXT_LINE();
  UART_OutString("os_time [reset/read]");
  CMD_NEXT_LINE();
  UART_OutString("led");
  CMD_NEXT_LINE();
  UART_OutString("perf");
  CMD_NEXT_LINE();
  UART_OutString("x");
  CMD_NEXT_LINE();
  UART_OutString("y");
  CMD_NEXT_LINE();
  UART_OutString("format");
  CMD_NEXT_LINE();
  UART_OutString("dprint");
  CMD_NEXT_LINE();
  UART_OutString("wr");
  CMD_NEXT_LINE();
  UART_OutString("cr");
  CMD_NEXT_LINE();
  UART_OutString("fprint");
  CMD_NEXT_LINE();
  UART_OutString("fdel");
  CMD_NEXT_LINE();
}

void format(void) {
  eFile_Format();
};

void print_directory(void) {
  eFile_DOpen("");
  for(int i = 0; i < 10; i++) {
    char* name;
    unsigned long size;
    if(eFile_DirNext(&name, &size) == 0){
      UART_OutString(name);
      UART_OutChar(' ');
      UART_OutUDec(size);
      CMD_NEXT_LINE();
    }
    else{
      break;
    }
  }
  eFile_DClose();
};

void create_file(char* name) {
  eFile_Create(name);
};

void write_file(char* name, char* data) {
  eFile_WOpen(name);
  for(int i = 0; i < strlen(data); i++) {
    eFile_Write(data[i]);
  }
  eFile_WClose();
};

void print_file(char* name) {
  eFile_ROpen(name);
  char buf;
  while(!eFile_ReadNext(&buf)) {
    UART_OutChar(buf);
  }
  CMD_NEXT_LINE();
  eFile_RClose();
};

void delete_file(char* name) {
  eFile_Delete(name);
};

// returns 0 if token exists, 1 if no more tokens
int Grab_Token(char* buf) {
  char* token = strtok(NULL, " ");
  if(token != NULL) {
    strcpy(buf, token);
    return 0;
  }
  else {
    // no more tokens
    return 1;
  }
}

// *********** Command line interpreter (shell) ************
void Interpreter(void){ 
  // Available functions:
  //  Select top/bottom LCD screen
  //  Print a string + value on next available line
  //  ADC_In
  //  Current OS time/Reset OS time
  // File System:
  //  file format, printdir, cr, wr, file_print, delete
    
  // Grab next serial input
  char next_input[16];
  char next_command[16];
  char* token;
  // eFile_Init(); // Timer2A needs to trigger 10ms before this
  
  while(1) {
    UART_InString(next_input, 16);
    
    token = strtok(next_input, " ");
    strcpy(next_command, token);
        
    // Go to next function
    if(!strcmp(next_command, "lcd")) {
      char next_parameter[16];
      if(Grab_Token(next_parameter)) {
        // missing parameter
        Interpreter_Error(1);
      }
      
      select_screen(next_parameter);
    }
    else if(!strcmp(next_command, "print")) {
      // Grab string and signed integer
      char next_parameter_1[16];
      if(Grab_Token(next_parameter_1)) {
        Interpreter_Error(1);
      }
      
      char next_parameter_2[16];
      if(Grab_Token(next_parameter_2)) {
        Interpreter_Error(1);
      }
      
      print_line(next_parameter_1, next_parameter_2);
    }
    else if(!strcmp(next_command, "adc_in")) {
      // Don't care about anything extra
      get_adc_value();
    }
    else if(!strcmp(next_command, "os_time")) {
      char next_parameter[16];
      if(Grab_Token(next_parameter)) {
        Interpreter_Error(1);
      }
    
      os_time_op(next_parameter);
    }
    else if(!strcmp(next_command, "led")) {
      char next_parameter[16];
      if(Grab_Token(next_parameter)) {
        Interpreter_Error(1);
      }
      
      led_toggle();
    }
    else if(!strcmp(next_command, "help")) {
      help();
    }
    else if(!strcmp(next_command, "format")) {
      format();
    }
    else if(!strcmp(next_command, "dprint")) {
      print_directory();
    }
    else if(!strcmp(next_command, "fcr")) {
      char next_parameter[16];
      if(Grab_Token(next_parameter)) {
        Interpreter_Error(1);
      }
      create_file(next_parameter);
    }
    else if(!strcmp(next_command, "fwr")) {
      char next_parameter[16];
      if(Grab_Token(next_parameter)) {
        Interpreter_Error(1);
      }
      char next_parameter_2[16];
      if(Grab_Token(next_parameter_2)) {
        Interpreter_Error(1);
      }
      
      write_file(next_parameter, next_parameter_2);
    }
    else if(!strcmp(next_command, "fr")) {
      char next_parameter[16];
      if(Grab_Token(next_parameter)) {
        Interpreter_Error(1);
      }
      print_file(next_parameter);
    }
    else if(!strcmp(next_command, "fdel")) {
      char next_parameter[16];
      if(Grab_Token(next_parameter)) {
        Interpreter_Error(1);
      }
      delete_file(next_parameter);
    }
    else if(!strcmp(next_command, "mount")) {
      eFile_Mount();
    }
    else if(!strcmp(next_command, "unmount")) {
      eFile_Unmount();
    }
    else if(!strcmp(next_command, "loadp")) {
      ELFEnv_t env = {symtab, 1};
      // call elf loader
      if(!exec_elf("User.axf", &env)){ //success
        UART_OutString("loaded user program");
        CMD_NEXT_LINE();
      }
    }
    else if(!strcmp(next_command, "wifi")) {
      //OS_AddThread(&WebServer, 128, 0);
    }
    else {
      // Missing/incorrect command, just log error and ignore
      Interpreter_Error(0);
    }
  }
}

char cr[2] = {CR, '\0'};
char newline[2] = {'\n', '\0'};
int test = 0;
extern Sema4Type TM4CWebServerSema;

void TypingTest(void);

// *********** Command line interpreter (shell) ************
void InterpreterWeb(void){ 
  // Available functions:
  //  Select top/bottom LCD screen 1
  //  Print a string + value on next available line 2
  //  ADC_In 3
  //  Current OS time/Reset OS time 4 (reset 1, read 0)
  // led 5
  // help 6
  // File System:
  //  file format 7, printdir 8, cr 9, wr 10, file_print 11, delete 12, mount 13, unmount 14, loadp 15, exit 16, rgb 17
    
  // Grab next serial input
  char next_input[16];
  char next_command[16];
  char* token;
  // eFile_Init(); // Timer2A needs to trigger 10ms before this
  
  while(1) {
    if(!test) {
    strcpy(next_input, "");
    ESP8266_Receive(next_input, 16);
    
    token = strtok(next_input, " ");
    strcpy(next_command, token);
        
    // Go to next function
    if(!strcmp(next_command, "1")) {
      char next_parameter[16];
      if(Grab_Token(next_parameter)) {
        // missing parameter
        Interpreter_Error(1);
      }
      
      select_screen(next_parameter);
    }
    else if(!strcmp(next_command, "2")) {
      // Grab string and signed integer
      char next_parameter_1[16];
      if(Grab_Token(next_parameter_1)) {
        Interpreter_Error(1);
      }
      
      char next_parameter_2[16];
      if(Grab_Token(next_parameter_2)) {
        Interpreter_Error(1);
      }
      
      print_line(next_parameter_1, next_parameter_2);
    }
    else if(!strcmp(next_command, "3")) {
      // Don't care about anything extra
      char buffer[10];
      ESP8266_Send("ADC: ");
      sprintf(buffer, "%d", ADC_In());
      ESP8266_Send(buffer);
      ESP8266_Send(cr);
      ESP8266_Send(newline);
    }
    else if(!strcmp(next_command, "4")) {
      char next_parameter[16];
      if(Grab_Token(next_parameter)) {
        Interpreter_Error(1);
      }
    
      if(!strcmp(next_parameter, "1")) {
        OS_ClearMsTime();
      }
      else if(!strcmp(next_parameter, "0")) {
        char buffer[10];
        ESP8266_Send("OS time: ");
        sprintf(buffer, "%d", OS_MsTime());
        ESP8266_Send(buffer);
        ESP8266_Send(cr);
        ESP8266_Send(newline);
      }
      else {
        // invalid parameter
        Interpreter_Error(2);
      }
    }
    else if(!strcmp(next_command, "5")) {
      char next_parameter[16];
      if(Grab_Token(next_parameter)) {
        Interpreter_Error(1);
      }
      
      led_toggle();
    }
    else if(!strcmp(next_command, "6")) {
      ESP8266_Send("lcd [bot/top] 1");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("print [str][int] 2");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("adc_in 3");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("os_time 4 1/0");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("led 5");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("help 6");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("format 7");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("dprint 8");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("cr 9");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("wr 10");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("fprint 11");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("fdel 12");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("mount 13");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("unmount 14");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("loadp 15");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("exit 16");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("rgb 17");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      ESP8266_Send("End");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
    }
    else if(!strcmp(next_command, "7")) {
      format();
    }
    else if(!strcmp(next_command, "8")) {
      eFile_DOpen("");
      for(int i = 0; i < 10; i++) {
        char* name;
        unsigned long size;
        if(eFile_DirNext(&name, &size) == 0){
          ESP8266_Send(name);
          char buffer[2] = {' ', '\0'};
          ESP8266_Send(buffer);
          char buf[10];
          sprintf(buf, "%u", size);
          ESP8266_Send(buf);
          ESP8266_Send(cr);
          ESP8266_Send(newline);
        }
        else{
          break;
        }
      }
      ESP8266_Send("End");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      eFile_DClose();
    }
    else if(!strcmp(next_command, "9")) {
      char next_parameter[16];
      if(Grab_Token(next_parameter)) {
        Interpreter_Error(1);
      }
      create_file(next_parameter);
    }
    else if(!strcmp(next_command, "10")) {
      char next_parameter[16];
      if(Grab_Token(next_parameter)) {
        Interpreter_Error(1);
      }
      char next_parameter_2[16];
      if(Grab_Token(next_parameter_2)) {
        Interpreter_Error(1);
      }
      
      write_file(next_parameter, next_parameter_2);
    }
    else if(!strcmp(next_command, "11")) {
      char next_parameter[16];
      if(Grab_Token(next_parameter)) {
        Interpreter_Error(1);
      }
      eFile_ROpen(next_parameter);
      char buf;
      while(!eFile_ReadNext(&buf)) {
        char buffer[10];
        sprintf(buffer, "%c", buf);
        ESP8266_Send(buffer);
        ESP8266_Send(cr);
        ESP8266_Send(newline);
      }
      ESP8266_Send("End");
      ESP8266_Send(cr);
      ESP8266_Send(newline);
      eFile_RClose();
    }
    else if(!strcmp(next_command, "12")) {
      char next_parameter[16];
      if(Grab_Token(next_parameter)) {
        Interpreter_Error(1);
      }
      delete_file(next_parameter);
    }
    else if(!strcmp(next_command, "13")) {
      eFile_Mount();
    }
    else if(!strcmp(next_command, "14")) {
      eFile_Unmount();
    }
    else if(!strcmp(next_command, "15")) {
      ELFEnv_t env = {symtab, 1};
      // call elf loader
      if(!exec_elf("User.axf", &env)){ //success
        //ESP8266_Send("loaded");
        //ESP8266_Send(cr);
        //ESP8266_Send(newline);
      }
    }
    else if(!strcmp(next_command, "16")) {
      break;
    }
    else if(!strcmp(next_command, "17")) {
      PF3 ^= 0x08;
      OS_Sleep(500); // 500 ms
      PF3 ^= 0x08;
      PF1 ^= 0x02;
      OS_Sleep(500);
      PF1 ^= 0x02;
      PF2 ^= 0x04;
      OS_Sleep(500);
      PF2 ^= 0x04;
    }
    else if(!strcmp(next_command, "18")) {
      long sr = StartCritical();
      test = 1;
      EndCritical(sr);
      OS_AddThread(TypingTest, 128, 1);
    }
    else {
      // Missing/incorrect command, just log error and ignore
      Interpreter_Error(0);
    }
  }
  }
  ESP8266_CloseTCPConnection();
  OS_Signal(&TM4CWebServerSema);
  OS_Kill();
}

int end = 0;

void signal(void) {
  static int i = 0;
  if(i < 9) {
    i++;
  }
  else{
    end = 1;
    i = 0;
  }
}

int num_incorrect(char* file, char* user) {
  uint32_t incorrect = 0;
  uint32_t count = 0;
  while(user[count] != 0) {
    if(file[count] != user[count]) {
      incorrect++;
    }
    count++;
  }
  return incorrect;
}

void TypingTest(void) {
  // put this in a loop
  // load a random file
  // send entire file to client (client should include enter chars for formatting)
  // wait for client to indicate start or exit (OS_Kill)
  // count down start 3.. 2.. 1.. use OS_Sleep
  // begin a 1 minute timer
  // continuously accept single characters (client should ignore enter chars)
  // once timer is triggered, signal end (also to user), calculate WPM, and send to client
  eFile_Mount();
  int random = rand()%10;
  char file[2];
  sprintf(file, "%d", random);
  char* file_buffer = (char*) Heap_Calloc(1500);
  char* file_bufferpt = file_buffer;
  char* user_buffer = (char*) Heap_Calloc(1500);
  
  eFile_ROpen(file);
  char buf;
  while(!eFile_ReadNext(&buf)) {
    *file_bufferpt = buf;
    file_bufferpt++;
  }
  eFile_RClose();
  ESP8266_Send(file_buffer);
  ESP8266_Send(cr);
  ESP8266_Send(newline);
  
  // wait for user response
  char response[10];
  ESP8266_Receive(response, 10);
  if(!strcmp(response, "0")) {
    // start test, should be echoed to user
    end = 0;
    
    eFile_ROpen(file);
    ESP8266_Send("3");
    ESP8266_Send(cr);
    ESP8266_Send(newline);
    OS_Sleep(1000);
    ESP8266_Send("2");
    ESP8266_Send(cr);
    ESP8266_Send(newline);
    OS_Sleep(1000);
    ESP8266_Send("1");
    ESP8266_Send(cr);
    ESP8266_Send(newline);
    OS_Sleep(1000);
    ESP8266_Send("START");
    ESP8266_Send(cr);
    ESP8266_Send(newline);
    
    // start timer
    Timer1A_Init(signal, (uint32_t) 480000000, 1); // can't do one minute, need to repeat 10 times
    while(end == 0) {}
    Timer1A_Stop();
    ESP8266_Send("0");
    ESP8266_Send(cr);
    ESP8266_Send(newline);
    ESP8266_Receive(user_buffer, 1500);
      
    // timer done
    // calculate score
    uint32_t incorrect = num_incorrect(file_buffer, user_buffer);
    uint32_t wpm = ((strlen(user_buffer))*(strlen(user_buffer) - incorrect))/(5*strlen(user_buffer));
    char wpm_send[10];
    sprintf(wpm_send, "%d", wpm);
    ESP8266_Send(wpm_send);
    ESP8266_Send(cr);
    ESP8266_Send(newline);
  }
  Heap_Free(file_buffer);
  Heap_Free(user_buffer);
  eFile_Unmount();
  test = 0;
  OS_Kill();
}
