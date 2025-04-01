/* updated as of april 1 9:30 am. integrates adc, circular buffer, hardware based
timer, isrs

GENERAL STRUCTURE IS:
- adc setup, interrupts setup
- update_timer function function basically is an interrupt based timer where
every SAMPLING_RATE time, it reads the value of the adc and adds to
raw_adc_samples[] which is of size ADC_BUFFER_SIZE. all of this added to the
circular buffer
- chunks being sent to different subfunctions not integrated yet
*/

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ADC_BASE 0xFF204000
#define LEDs_BASE 0xFF200000
#define GPIO_BASE 0xFF200070
#define BUTTON_BASE 0xFF200050
#define KEY_BASE 0xFF200050
#define TIMER_BASE 0xFF202000

#define ADC_BUFFER_SIZE 80
#define REF_VOLTAGE 5.0f  // 5V reference

volatile int write_index = 0;
volatile int read_index = 0;

volatile int* LEDS = (int*)LEDs_BASE;
volatile int* BUTTON = BUTTON_BASE;

volatile int* ADC_ptr = (int*)ADC_BASE;
volatile int* KEY_ptr = (int*)KEY_BASE;
volatile int* interval_timer_ptr =
    (int*)TIMER_BASE;  // interal timer base address

// MANUALLY SET SETTINGS
int SAMPLE_RATE = 5000000;
int TRIGGER = 3;

// TRIGGER VARIABLES
bool trigger_mode_active = true;
bool trigger_hold = false;

extern float raw_adc_samples[ADC_BUFFER_SIZE];
extern int adc_sample_count;
extern void buffer_to_measurements(void);

// ALL FUNCTION PROTOTYPES
unsigned int buffer_get(void);
void process_available_samples(void);

void interrupt_handler();
void get_samples();
void pushbutton_ISR();
void update_timer(int sample_rate);
void the_reset(void) __attribute__((section(".reset")));

void the_reset(void) {
  asm(".set noat");       /* Instruct the assembler NOT to use reg at (r1) as
                           * a temp register for performing optimizations */
  asm(".set nobreak");    /* Suppresses a warning message that says that
                           * some debuggers corrupt regs bt (r25) and ba
                           * (r30)
                           */
  asm("movia r2, main");  // Call the C language main program
  asm("jmp r2");
}

void the_exception(void) __attribute__((section(".exceptions")));
void the_exception(void) {
  asm("subi sp, sp, 128");
  asm("stw et, 96(sp)");
  asm("rdctl et, ctl4");
  asm("beq et, r0, SKIP_EA_DEC");  // Interrupt is not external
  asm("subi ea, ea, 4");           /* Must decrement ea by one instruction
                                    * for external interupts, so that the
                                    * interrupted instruction will be run */
  asm("SKIP_EA_DEC:");
  asm("stw r1, 4(sp)");  // Save all registers
  asm("stw r2, 8(sp)");
  asm("stw r3, 12(sp)");
  asm("stw r4, 16(sp)");
  asm("stw r5, 20(sp)");
  asm("stw r6, 24(sp)");
  asm("stw r7, 28(sp)");
  asm("stw r8, 32(sp)");
  asm("stw r9, 36(sp)");
  asm("stw r10, 40(sp)");
  asm("stw r11, 44(sp)");
  asm("stw r12, 48(sp)");
  asm("stw r13, 52(sp)");
  asm("stw r14, 56(sp)");
  asm("stw r15, 60(sp)");
  asm("stw r16, 64(sp)");
  asm("stw r17, 68(sp)");
  asm("stw r18, 72(sp)");
  asm("stw r19, 76(sp)");
  asm("stw r20, 80(sp)");
  asm("stw r21, 84(sp)");
  asm("stw r22, 88(sp)");
  asm("stw r23, 92(sp)");
  asm("stw r25, 100(sp)");  // r25 = bt (skip r24 = et, because it is saved
  // above)
  asm("stw r26, 104(sp)");  // r26 = gp
  // skip r27 because it is sp, and there is no point in saving this
  asm("stw r28, 112(sp)");  // r28 = fp
  asm("stw r29, 116(sp)");  // r29 = ea
  asm("stw r30, 120(sp)");  // r30 = ba
  asm("stw r31, 124(sp)");  // r31 = ra
  asm("addi fp, sp, 128");
  asm("call interrupt_handler");  // Call the C language interrupt handler
  asm("ldw r1, 4(sp)");           // Restore all registers
  asm("ldw r2, 8(sp)");
  asm("ldw r3, 12(sp)");
  asm("ldw r4, 16(sp)");
  asm("ldw r5, 20(sp)");
  asm("ldw r6, 24(sp)");
  asm("ldw r7, 28(sp)");
  asm("ldw r8, 32(sp)");
  asm("ldw r9, 36(sp)");
  asm("ldw r10, 40(sp)");
  asm("ldw r11, 44(sp)");
  asm("ldw r12, 48(sp)");
  asm("ldw r13, 52(sp)");
  asm("ldw r14, 56(sp)");
  asm("ldw r15, 60(sp)");
  asm("ldw r16, 64(sp)");
  asm("ldw r17, 68(sp)");
  asm("ldw r18, 72(sp)");
  asm("ldw r19, 76(sp)");
  asm("ldw r20, 80(sp)");
  asm("ldw r21, 84(sp)");
  asm("ldw r22, 88(sp)");
  asm("ldw r23, 92(sp)");
  asm("ldw r24, 96(sp)");
  asm("ldw r25, 100(sp)");  // r25 = bt
  asm("ldw r26, 104(sp)");  // r26 = gp
  // skip r27 because it is sp, and we did not save this on the stack
  asm("ldw r28, 112(sp)");  // r28 = fp
  asm("ldw r29, 116(sp)");  // r29 = ea
  asm("ldw r30, 120(sp)");  // r30 = ba
  asm("ldw r31, 124(sp)");  // r31 = ra
  asm("addi sp, sp, 128");
  asm("eret");
}

int main(void) {
  update_timer(SAMPLE_RATE);
  //*(KEY_ptr + 2) = 0x3; // enable interrupts for all pushbuttons
  *(KEY_ptr + 2) = 0xF;  // write to the pushbutton interrupt mask register
  __builtin_wrctl(3, 0b11);
  __builtin_wrctl(0, 1);  // enable Nios II interrupts

  // ADC SETUP
  *(ADC_ptr + 1) = 0xFFFFFFFF;
  *LEDS = 0;

  while (1) {
    // *LEDS = index; WRITE OR READ?

    // process_available_samples();  // chunk samples and process
  }

  return 0;
}

void update_timer(int sample_rate) {
  *(interval_timer_ptr + 0x2) = (sample_rate & 0xFFFF);
  *(interval_timer_ptr + 0x3) = (sample_rate >> 16) & 0xFFFF;
  *(interval_timer_ptr + 1) = 0x7;  // STOP = 0, START = 1, CONT = 1, ITO = 1
}

void interrupt_handler(void) {
  int ipending;
  ipending = __builtin_rdctl(4);

  if (ipending & 0x1) {
    get_samples();
  } else {
    pushbutton_ISR();
  }
  return;
}

void get_samples() {
  interval_timer_ptr = (int*)0xFF202000;
  *(interval_timer_ptr) = 0;  // clear the interrupt

  raw_adc_samples[write_index] = (*ADC_ptr) & 0xFFF;
  write_index = (write_index + 1) % ADC_BUFFER_SIZE;
}

void pushbutton_ISR() {
  trigger_hold = false;
  volatile int* KEY_ptr = (int*)0xFF200050;
  int press;
  press = *(KEY_ptr + 3);  // read the pushbutton interrupt register
  *(KEY_ptr + 3) = press;  // clear the interrupt
  if (press & 0x1) {
    SAMPLE_RATE *= 2;
    // SAMPLE_RATE = (SAMPLE_RATE&(1<<31)) ? SAMPLE_RATE * 2 : SAMPLE_RATE;
    update_timer(SAMPLE_RATE);
  } else if (press & 0x2) {
    SAMPLE_RATE = (SAMPLE_RATE > 1) ? SAMPLE_RATE / 2 : SAMPLE_RATE;
    update_timer(SAMPLE_RATE);
  } else if (press & 0x4) {
    TRIGGER++;
  } else {
    TRIGGER = (TRIGGER > 0) ? TRIGGER - 1 : 0;
  }
  return;
}

//////////////////////// ANYTHING BELOW THIS IS NOT UPDATED
/////////////////////////////////////

unsigned int buffer_get(void) {  // READ SAMPLE
  if (write_index == read_index) {
    return 0;  // empty buffer
  }

  unsigned int sample = raw_adc_samples[read_index];
  read_index++;
  if (read_index >= ADC_BUFFER_SIZE) read_index = 0;
  return sample;
}

// PROCESS SAMPLES IN CHUNKS (rather than processing individually)
#define CHUNK_SIZE 80

void process_available_samples(void) {
  int available_raw_adc_samples;

  while (1) {
    // how many samples are there?
    if (write_index >= read_index) {
      available_raw_adc_samples = write_index - read_index;
    } else {
      available_raw_adc_samples = ADC_BUFFER_SIZE - read_index + write_index;
    }

    if (available_raw_adc_samples <= 0)  // if none break
      break;

    // GET MAX SIZE TO READ
    int samples_to_read;
    if (available_raw_adc_samples > CHUNK_SIZE) {
      samples_to_read = CHUNK_SIZE;
    } else {
      samples_to_read = available_raw_adc_samples;
    }

    // CONVERT VALUES TO VOTLAGE INSTEAD OF RAW ADC VALUES
    for (int i = 0; i < samples_to_read; i++) {
      unsigned int raw_sample = buffer_get();

      float voltage = (raw_sample / 4095.0f) * REF_VOLTAGE;
      raw_adc_samples[adc_sample_count++] = voltage;
    }

    // DIVIDE AND SEND IN CHUNKS
    while (adc_sample_count >= CHUNK_SIZE) {
      // first chunk
      buffer_to_measurements();

      // shift and send
      int remaining = adc_sample_count - CHUNK_SIZE;
      for (int j = 0; j < remaining; j++) {
        raw_adc_samples[j] = raw_adc_samples[j + CHUNK_SIZE];
      }
      adc_sample_count = remaining;
    }
  }

  // IF ANY MORE FULL CHUNKS SEND
  while (adc_sample_count >= CHUNK_SIZE) {
    buffer_to_measurements();

    int remaining = adc_sample_count - CHUNK_SIZE;
    for (int j = 0; j < remaining; j++) {
      raw_adc_samples[j] = raw_adc_samples[j + CHUNK_SIZE];
    }
    adc_sample_count = remaining;
  }
}
