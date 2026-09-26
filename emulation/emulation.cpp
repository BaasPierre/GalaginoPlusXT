#include "Arduino.h"
#include "emulation.h"
#include "../machines/machineBase.h"
#include <atomic>

extern machineBase *currentMachine;
TaskHandle_t emulationTaskHandle;
volatile static char doDeleteEmulationTask;

// vblanks given by the video loop but not yet emulated. A task notification
// alone would merge several gives into one, losing frames while the video
// loop catches up (main.cpp frame clock). Capped so a game that emulates
// slower than real time slows down instead of building an ever growing
// backlog.
#define MAX_PENDING_FRAMES 2
static std::atomic<int> pendingFrames(0);

void emulation_start() {
#ifdef DEBUG_TIMING
  timeTotal = millis();
#endif
  currentMachine->reset();
  currentMachine->start();
  pendingFrames = 0;
  xTaskCreatePinnedToCore(emulation_task, "emulation task", 4096, NULL, 2, &emulationTaskHandle, ARDUINO_RUNNING_CORE == 0 ? 1 : 0);
}

void emulation_stop() {
  if (!emulationTaskHandle)
    return;
  
  doDeleteEmulationTask = 1;  
  while (doDeleteEmulationTask) {
    xTaskNotifyGive(emulationTaskHandle);
    vTaskDelay(1);
  }

  emulationTaskHandle = NULL;
  currentMachine->reset();  // clear sound output
  currentMachine->stop();   // free any lazily-allocated scratch buffers
}

void emulation_notifyGive() {
  if (!emulationTaskHandle)
    return;

#ifdef DEBUG_TIMING
  giveCount++;
#endif
  if (pendingFrames < MAX_PENDING_FRAMES)
    pendingFrames++;
  xTaskNotifyGive(emulationTaskHandle);
}

// Use this, not the task's notification value: the value is only taken
// when no frame is pending, so it can stay set while the emulation catches
// up. IRAM: polled from machines' IRAM code on the emulation core.
bool IRAM_ATTR emulation_framePending(void) {
  return pendingFrames > 0;
}

void emulation_videoRendered(void) {
#ifdef DEBUG_TIMING
  videoSum += millis() - cpuStart;
#endif
}

void emulation_task(void *p) {
  for(;;) {
    // Investigated (2026-09-16): theorized that useVideoHalfRate()'s two
    // emulation_notifyGive() calls per updateAudioVideo() could coalesce
    // into a single ulTaskNotifyTake() wake-up if both landed before this
    // task got back to the wait, silently dropping every 2nd frame advance.
    // DISPROVED on real hardware: last_pending (the raw count
    // ulTaskNotifyTake returns before clearing) was measured as exactly 1
    // on every single logged iteration during real gameplay - the two
    // gives are never actually coalescing. The slow-motion symptom is real
    // (run_frame Hz measured ~28-29Hz dropping to ~18Hz, not the expected
    // ~59-60Hz) but this was not its cause. Kept the extra run_frame Hz
    // diagnostic (vs. the old loop-iteration-only Hz) since it's a more
    // direct measurement of actual game-logic rate; removed the dead
    // catch-up-frame logic since pending is never >1 in practice.
#ifdef DEBUG_TIMING
    cpuStart = millis();
#endif

    currentMachine->run_frame();
#ifdef DEBUG_TIMING
    runFrameCount++;
#endif

    if (doDeleteEmulationTask) {
      doDeleteEmulationTask = 0;
      vTaskDelete(emulationTaskHandle);
    }

#ifdef DEBUG_TIMING
    cpuSum += millis() - cpuStart;
#endif

    // Wait for signal from video task to emulate a 60Hz frame rate. Don't do
    // this unless the game has actually started to speed up the boot process
    // a little bit.
    if(currentMachine->game_started) {
      // one run_frame per vblank given (see pendingFrames); emulation_stop()
      // wakes the task with a plain notify to delete it
      while (pendingFrames <= 0 && !doDeleteEmulationTask)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      if (pendingFrames > 0)
        pendingFrames--;
    } else
      vTaskDelay(1); // give a millisecond delay to make the watchdog happy

#ifdef DEBUG_TIMING
    // The 60hz vblank rate is in turn 16.6 ms.
    if (counter % 10 == 0) {
      // good time total: 160...170ms. run_frame Hz = actual game-logic
      // advances per second - this is what should read ~59-60 for correct
      // speed. give Hz = how often the VIDEO/main core actually calls
      // emulation_notifyGive() - compare against run_frame Hz: if give Hz
      // is also ~29 (not ~59), the bug is on the video/give side (main.cpp
      // not reaching its 2nd emulation_notifyGive() per updateAudioVideo()
      // call at the intended rate), not in the emulation task's wait/take
      // logic (already proven innocent - see the comment above this loop).
      unsigned long now = millis();
      printf("10-loops: %3d Hz | run_frame: %3lu Hz | give: %3lu Hz | Total: %3d ms | Cpu: %3d ms | Video: %3d ms\n",
             10000 / (now - timeTotal), runFrameCount * 1000 / (now - timeTotal),
             giveCount * 1000 / (now - timeTotal),
             now - timeTotal, cpuSum, videoSum);
      timeTotal = now;
      cpuSum = 0;
      videoSum = 0;
      giveCount = 0;
      runFrameCount = 0;
    }
    counter++;
#endif
  }
}

unsigned char IRAM_ATTR OpZ80_INL(unsigned short Addr) {
  return currentMachine->opZ80(Addr);
}

void IRAM_ATTR OutZ80(unsigned short Port, unsigned char Value) {
  currentMachine->outZ80(Port, Value);
}
  
unsigned char IRAM_ATTR InZ80(unsigned short Port) {
  return currentMachine->inZ80(Port);
}

void IRAM_ATTR WrZ80(unsigned short Addr, unsigned char Value) {
  currentMachine->wrZ80(Addr, Value);
}

unsigned char IRAM_ATTR RdZ80(unsigned short Addr) {
  return currentMachine->rdZ80(Addr);
}

void PatchZ80(Z80 *R) {
}

void i8048_port_write(i8048_state_S *state, unsigned char port, unsigned char pos) {
  currentMachine->wrI8048_port(state, port, pos);
}

unsigned char i8048_port_read(i8048_state_S *state, unsigned char port) {
  return currentMachine->rdI8048_port(state, port);
}

unsigned char i8048_rom_read(i8048_state_S *state, unsigned short addr) {
  return currentMachine->rdI8048_rom(state, addr);
}

unsigned char i8048_xdm_read(i8048_state_S *state, unsigned char addr) {
  return currentMachine->rdI8048_xdm(state, addr);
}

void i8048_xdm_write(i8048_state_S *state, unsigned char addr, unsigned char data) {
}

unsigned char IRAM_ATTR m6809_read(m6809_state *s, uint16_t addr) {
  return currentMachine->m6809_read(s, addr);
}

void IRAM_ATTR m6809_write(m6809_state *s, uint16_t addr, uint8_t val) {
  currentMachine->m6809_write(s, addr, val);
}

unsigned char IRAM_ATTR m6809_read_opcode(m6809_state *s, uint16_t addr) {
  return currentMachine->m6809_read_opcode(s, addr);
}