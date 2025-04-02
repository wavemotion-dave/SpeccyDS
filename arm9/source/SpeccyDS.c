// =====================================================================================
// Copyright (c) 2025 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave and Marat 
// Fayzullin (Z80 core) are thanked profusely.
//
// The SpeccyDS emulator is offered as-is, without any warranty. Please see readme.md
// =====================================================================================
#include <nds.h>
#include <nds/fifomessages.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <fat.h>
#include <maxmod9.h>

#include "SpeccyDS.h"
#include "highscore.h"
#include "SpeccyUtils.h"
#include "speccy_kbd.h"
#include "debug_ovl.h"
#include "topscreen.h"
#include "mainmenu.h"
#include "soundbank.h"
#include "soundbank_bin.h"
#include "screenshot.h"
#include "cpu/z80/Z80_interface.h"

#include "printf.h"

u32 debug[0x10]={0};

// --------------------------------------------------------------------------
// This is the full 64K Spectrum memory map:
//    0x0000-0x3FFF  Spectrum BIOS. Either 48.rom or 128.rom (bank 0 or 1)
//    0x4000-0xFFFF  Spectrum 48K of RAM / Memory
// --------------------------------------------------------------------------
u8 RAM_Memory[0x10000]    ALIGN(32) = {0};  // The Z80 Memory is 64K 
u8 RAM_Memory128[0x20000] ALIGN(32) = {0};  // The Z80 Memory is 64K but we expand this for a 128K model
u8 SpectrumBios[0x4000]             = {0};  // We keep the 16k ZX Spectrum 48K BIOS around
u8 SpectrumBios128[0x8000]          = {0};  // We keep the 32k ZX Spectrum 128K BIOS around

u8 ROM_Memory[MAX_CART_SIZE];            // ROM Carts up to 1MB

static char cmd_line_file[256];
char initial_file[MAX_ROM_NAME] = "";
char initial_path[MAX_ROM_NAME] = "";

u8 last_speccy_mode  = 99;
u8 bFirstTime        = 3;
u8 bStartTapeIn      = 2;

// ---------------------------------------------------------------------------
// Some timing and frame rate comutations to keep the emulation on pace...
// ---------------------------------------------------------------------------
u16 emuFps          __attribute__((section(".dtcm"))) = 0;
u16 emuActFrames    __attribute__((section(".dtcm"))) = 0;
u16 timingFrames    __attribute__((section(".dtcm"))) = 0;

// ----------------------------------------------------------------------------------
// For the various BIOS files ... only the 48.rom spectrum BIOS is truly required...
// ----------------------------------------------------------------------------------
u8 bSpeccyBiosFound   = false;

u8 soundEmuPause     __attribute__((section(".dtcm"))) = 1;       // Set to 1 to pause (mute) sound, 0 is sound unmuted (sound channels active)

// -----------------------------------------------------------------------------------------------
// This set of critical vars is what determines the machine type - 
// -----------------------------------------------------------------------------------------------
u8 speccy_mode       __attribute__((section(".dtcm"))) = 0;       // Set to 1 when a .Z80 ROM is loaded (ZX Spectrum 48K)
u8 kbd_key           __attribute__((section(".dtcm"))) = 0;       // 0 if no key pressed, othewise the ASCII key (e.g. 'A', 'B', '3', etc)
u16 nds_key          __attribute__((section(".dtcm"))) = 0;       // 0 if no key pressed, othewise the NDS keys from keysCurrent() or similar
u8 last_mapped_key   __attribute__((section(".dtcm"))) = 0;       // The last mapped key which has been pressed - used for key click feedback
u8 kbd_keys_pressed  __attribute__((section(".dtcm"))) = 0;       // Each frame we check for keys pressed - since we can map keyboard keys to the NDS, there may be several pressed at once
u8 kbd_keys[12]      __attribute__((section(".dtcm")));           // Up to 12 possible keys pressed at the same time (we have 12 NDS physical buttons though it's unlikely that more than 2 or maybe 3 would be pressed)

u8 bStartSoundEngine = 0;      // Set to true to unmute sound after 1 frame of rendering...
int bg0, bg1, bg0b, bg1b;      // Some vars for NDS background screen handling
u16 vusCptVBL = 0;             // We use this as a basic timer for the Mario sprite... could be removed if another timer can be utilized
u8 touch_debounce = 0;         // A bit of touch-screen debounce
u8 key_debounce = 0;           // A bit of key debounce

// The DS/DSi has 12 keys that can be mapped
u16 NDS_keyMap[12] __attribute__((section(".dtcm"))) = {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_A, KEY_B, KEY_X, KEY_Y, KEY_R, KEY_L, KEY_START, KEY_SELECT};

// --------------------------------------------------------------------
// The key map for the ZX Spectrum... mapped into the NDS controller
// --------------------------------------------------------------------
u16 keyCoresp[MAX_KEY_OPTIONS] __attribute__((section(".dtcm"))) = {
    JST_UP,     //0
    JST_DOWN,
    JST_LEFT,
    JST_RIGHT,
    JST_FIRE,
    
    META_KBD_A, //5
    META_KBD_B,
    META_KBD_C,
    META_KBD_D,
    META_KBD_E,
    META_KBD_F, //10
    META_KBD_G,
    META_KBD_H,
    META_KBD_I,
    META_KBD_J,
    META_KBD_K, //15
    META_KBD_L,
    META_KBD_M, 
    META_KBD_N,
    META_KBD_O,
    META_KBD_P, //20
    META_KBD_Q,
    META_KBD_R,
    META_KBD_S,
    META_KBD_T,
    META_KBD_U, //25
    META_KBD_V,
    META_KBD_W,
    META_KBD_X,
    META_KBD_Y,
    META_KBD_Z, //30

    META_KBD_1, //31
    META_KBD_2,
    META_KBD_3,
    META_KBD_4,
    META_KBD_5,
    META_KBD_6,
    META_KBD_7,
    META_KBD_8,
    META_KBD_9,
    META_KBD_0, //40
    
    META_KBD_SHIFT,
    META_KBD_SYMBOL,
    META_KBD_SPACE,
    META_KBD_RETURN
};

static char tmp[64];    // For various sprintf() calls

// ------------------------------------------------------------
// Utility function to pause the sound...
// ------------------------------------------------------------
void SoundPause(void)
{
    soundEmuPause = 1;
}

// ------------------------------------------------------------
// Utility function to un pause the sound...
// ------------------------------------------------------------
void SoundUnPause(void)
{
    soundEmuPause = 0;
}

// --------------------------------------------------------------------------------------------
// MAXMOD streaming setup and handling...
// We were using the normal ARM7 sound core but it sounded "scratchy" and so with the help
// of FluBBa, we've swiched over to the maxmod sound core which performs much better.
// --------------------------------------------------------------------------------------------
#define sample_rate         (23100)    // To match how many samples (3x per scanline x 312 scanlines x 50 frames)
#define buffer_size         (512+16)   // Enough buffer that we don't have to fill it too often. Must be multiple of 16.

mm_ds_system sys   __attribute__((section(".dtcm")));
mm_stream myStream __attribute__((section(".dtcm")));

#define WAVE_DIRECT_BUF_SIZE 4095
u16 mixer_read      __attribute__((section(".dtcm"))) = 0;
u16 mixer_write     __attribute__((section(".dtcm"))) = 0;
s16 mixer[WAVE_DIRECT_BUF_SIZE+1];

// -------------------------------------------------------------------------------------------
// maxmod will call this routine when the buffer is half-empty and requests that
// we fill the sound buffer with more samples. They will request 'len' samples and
// we will fill exactly that many. If the sound is paused, we fill with 'mute' samples.
// -------------------------------------------------------------------------------------------
s16 last_sample __attribute__((section(".dtcm"))) = 0;
int breather    __attribute__((section(".dtcm"))) = 0;
extern void processDirectBeeperPlusAY(void);
ITCM_CODE mm_word OurSoundMixer(mm_word len, mm_addr dest, mm_stream_formats format)
{
    if (soundEmuPause)  // If paused, just "mix" in mute sound chip... all channels are OFF
    {
        s16 *p = (s16*)dest;
        for (int i=0; i<len; i++)
        {
           *p++ = last_sample;      // To prevent pops and clicks... just keep outputting the last sample
           *p++ = last_sample;      // To prevent pops and clicks... just keep outputting the last sample
        }
    }
    else
    {
        s16 *p = (s16*)dest;
        for (int i=0; i<len*2; i++)
        {
            if (mixer_read == mixer_write) {processDirectBeeperPlusAY();}
            *p++ = mixer[mixer_read];
            mixer_read = (mixer_read + 1) & WAVE_DIRECT_BUF_SIZE;
        }
        last_sample = *(p-1);
        if (breather) {breather -= (len*2); if (breather < 0) breather = 0;}
    }
    
    return  len;
}

// --------------------------------------------------------------------------------------------
// This is called when we are configured for 'Beeper' sound - it's really only useful for the
// ZX Spectrum handling. We mix in AY samples as well if we are a ZX 128K... however, the AY 
// doesn't need to be sampled quite as often so we grab 4 fresh samples per scanline and then
// use those to mix into the beeper processing which is happening at 4x per scanline.
// --------------------------------------------------------------------------------------------
s16 mixbufAY[4]  __attribute__((section(".dtcm")));
u8  mixbufAY_idx __attribute__((section(".dtcm"))) = 0;
ITCM_CODE void processDirectBeeperAY3(void)
{
    if (zx_AY_enabled) 
    {
        ay38910Mixer(3, mixbufAY, &myAY);
    }
    
    mixbufAY_idx = 0;
}


ITCM_CODE void processDirectBeeper(void)
{
    if (breather) {return;}
    s16 sample = mixbufAY[mixbufAY_idx++];
    if (portFE & 0x10) sample += 0xA00;
    mixer[mixer_write] = sample;
    mixer_write++; mixer_write &= WAVE_DIRECT_BUF_SIZE;
    if (((mixer_write+1)&WAVE_DIRECT_BUF_SIZE) == mixer_read) {breather = 2048;}
}


ITCM_CODE void processDirectBeeperPlusAY(void)
{
    s16 tmpAYbuf[2] = {0};

    s16 sample = (portFE & 0x10) ? 0xA00 : 0x000;
    
    if (zx_AY_enabled)
    {
        ay38910Mixer(1, tmpAYbuf, &myAY);        
        sample += tmpAYbuf[0];
    }
    mixer[mixer_write] = sample;
    mixer_write++; mixer_write &= WAVE_DIRECT_BUF_SIZE;
}


// -------------------------------------------------------------------------------------------
// Setup the maxmod audio stream - this will be a 16-bit Stereo PCM output at 55KHz which
// sounds about right for the Spectrum.
// -------------------------------------------------------------------------------------------
void setupStream(void)
{
  //----------------------------------------------------------------
  //  initialize maxmod with our small 3-effect soundbank
  //----------------------------------------------------------------
  mmInitDefaultMem((mm_addr)soundbank_bin);

  mmLoadEffect(SFX_CLICKNOQUIT);
  mmLoadEffect(SFX_KEYCLICK);
  mmLoadEffect(SFX_MUS_INTRO);
  
  //----------------------------------------------------------------
  //  open stream
  //----------------------------------------------------------------
  myStream.sampling_rate  = sample_rate;            // sample_rate for the CV to match the SN/AY drivers
  myStream.buffer_length  = buffer_size;            // buffer length = (512+16)
  myStream.callback       = OurSoundMixer;          // set callback function
  myStream.format         = MM_STREAM_16BIT_STEREO; // format = stereo 16-bit
  myStream.timer          = MM_TIMER0;              // use hardware timer 0
  myStream.manual         = false;                  // use automatic filling
  mmStreamOpen(&myStream);

  //----------------------------------------------------------------
  //  when using 'automatic' filling, your callback will be triggered
  //  every time half of the wave buffer is processed.
  //
  //  so:
  //  25000 (rate)
  //  ----- = ~21 Hz for a full pass, and ~42hz for half pass
  //  1200  (length)
  //----------------------------------------------------------------
  //  with 'manual' filling, you must call mmStreamUpdate
  //  periodically (and often enough to avoid buffer underruns)
  //----------------------------------------------------------------
}

void sound_chip_reset()
{
  memset(mixer,   0x00, sizeof(mixer));
  mixer_read=0;
  mixer_write=0;
  
  //  --------------------------------------------------------------------
  //  The AY sound chip is for the ZX Spectrum 128K
  //  --------------------------------------------------------------------
  ay38910Reset(&myAY);             // Reset the "AY" sound chip
  ay38910IndexW(0x07, &myAY);      // Register 7 is ENABLE
  ay38910DataW(0x3F, &myAY);       // All OFF (negative logic)
  ay38910Mixer(4, mixbufAY, &myAY);// Do an initial mix conversion to clear the output
  
  memset(mixbufAY, 0x00, sizeof(mixbufAY));
}

// -----------------------------------------------------------------------
// We setup the sound chips - disabling all volumes to start.
// -----------------------------------------------------------------------
void dsInstallSoundEmuFIFO(void)
{
  SoundPause();             // Pause any sound output  
  sound_chip_reset();       // Reset the SN, AY and SCC chips 
  setupStream();            // Setup maxmod stream...
  bStartSoundEngine = 5;    // Volume will 'unpause' after 5 frames in the main loop.
}

//*****************************************************************************
// Reset the Spectrum - mostly CPU, Super Game Module and memory...
//*****************************************************************************

// --------------------------------------------------------------
// When we first load a ROM/CASSETTE or when the user presses
// the RESET button on the touch-screen...
// --------------------------------------------------------------
void ResetSpectrum(void)
{    
  JoyState = 0x00000000;                // Nothing pressed to start

  sound_chip_reset();                   // Reset the AY chip
  ResetZ80(&CPU);                       // Reset the Z80 CPU core
  speccy_reset();                       // Reset the ZX Spectrum memory - decompress .z80 and restore BIOS

  // -----------------------------------------------------------
  // Timer 1 is used to time frame-to-frame of actual emulation
  // -----------------------------------------------------------
  TIMER1_CR = 0;
  TIMER1_DATA=0;
  TIMER1_CR=TIMER_ENABLE  | TIMER_DIV_1024;

  // -----------------------------------------------------------
  // Timer 2 is used to time once per second events
  // -----------------------------------------------------------
  TIMER2_CR=0;
  TIMER2_DATA=0;
  TIMER2_CR=TIMER_ENABLE  | TIMER_DIV_1024;
  timingFrames  = 0;
  emuFps=0;
  
  bFirstTime = 2;
  bStartTapeIn = 0;

  last_speccy_mode = 99;
}

//*********************************************************************************
// A mini Z80 debugger of sorts. Put out some Z80 and PORT information along
// with our ever-handy debug[] registers. This is enabled via global configuration.
//*********************************************************************************
extern u8 *fake_heap_end;     // current heap start
extern u8 *fake_heap_start;   // current heap end

u8* getHeapStart() {return fake_heap_start;}
u8* getHeapEnd()   {return (u8*)sbrk(0);}
u8* getHeapLimit() {return fake_heap_end;}

int getMemUsed() { // returns the amount of used memory in bytes
   struct mallinfo mi = mallinfo();
   return mi.uordblks;
}

int getMemFree() { // returns the amount of free memory in bytes
   struct mallinfo mi = mallinfo();
   return mi.fordblks + (getHeapLimit() - getHeapEnd());
}

void ShowDebugZ80(void)
{
    u8 idx=2;
    
    if (myGlobalConfig.debugger == 3)
    {
        sprintf(tmp, "PC %04X  SP %04X", CPU.PC.W, CPU.SP.W);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "AF %04X  AF'%04X", CPU.AF.W, CPU.AF1.W);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "BC %04X  BC'%04X", CPU.BC.W, CPU.BC1.W);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "DE %04X  DC'%04X", CPU.DE.W, CPU.DE1.W);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "HL %04X  HL'%04X", CPU.HL.W, CPU.HL1.W);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "IX %04X  IY %04X", CPU.IX.W, CPU.IY.W);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "Ts %-9d", CPU.TStates);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "I  %02X  R %02X", CPU.I, (CPU.R & 0x7F) | CPU.R_HighBit);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "FE %02X  FD %02X", portFE&0x7, portFD);
        DSPrint(0,idx++,7, tmp);

        idx++;
        
        sprintf(tmp, "AY %02X %02X %02X %02X", myAY.ayRegs[0], myAY.ayRegs[1], myAY.ayRegs[2], myAY.ayRegs[3]);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "AY %02X %02X %02X %02X", myAY.ayRegs[4], myAY.ayRegs[5], myAY.ayRegs[6], myAY.ayRegs[7]);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "AY %02X %02X %02X %02X", myAY.ayRegs[8], myAY.ayRegs[9], myAY.ayRegs[10], myAY.ayRegs[11]);
        DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "AY %02X %02X %02X %02X", myAY.ayRegs[12], myAY.ayRegs[13], myAY.ayRegs[14], myAY.ayRegs[15]);
        DSPrint(0,idx++,7, tmp);
        
        idx++;            

        sprintf(tmp, "LOAD: %-9s", loader_type); DSPrint(0,idx++, 7, tmp);
        sprintf(tmp, "MEM Used %dK", getMemUsed()/1024); DSPrint(0,idx++,7, tmp);
        sprintf(tmp, "MEM Free %dK", getMemFree()/1024); DSPrint(0,idx++,7, tmp);
        
        // CPU Disassembly!

        // Put out the debug registers...
        idx = 2;
        for (u8 i=0; i<16; i++)
        {
            sprintf(tmp, "D%X %-7lu %04X", i, debug[i], (u16)debug[i]); DSPrint(17,idx++, 7, tmp);
        }
    }
    else
    {
        idx = 1;
        for (u8 i=0; i<4; i++)
        {
            sprintf(tmp, "D%d %-7ld %04lX  D%d %-7ld %04lX", i, (s32)debug[i], (debug[i] < 0xFFFF ? debug[i]:0xFFFF), 4+i, (s32)debug[4+i], (debug[4+i] < 0xFFFF ? debug[4+i]:0xFFFF)); 
            DSPrint(0,idx++,7, tmp);
        }
    }
    idx++;
}


// ------------------------------------------------------------
// The status line shows the status of the Spectrum System
// on the top line of the bottom DS display.
// ------------------------------------------------------------
void DisplayStatusLine(bool bForce)
{
    if (myGlobalConfig.emuText == 0) return;
    
    if ((speccy_mode != last_speccy_mode) || bForce)
    {
        last_speccy_mode = speccy_mode;
        DSPrint(18,0,6, zx_128k_mode ? "SPECCY 128K" : "SPECCY 48K ");
    }

    if (zx_special_key || (kbd_key == KBD_KEY_SYMBOL) || (kbd_key == KBD_KEY_SHIFT))
    {
        if ((zx_special_key == 1) || (kbd_key == KBD_KEY_SHIFT))  DSPrint(3,0,6, "@");
        if ((zx_special_key == 2) || (kbd_key == KBD_KEY_SYMBOL)) DSPrint(3,0,2, "@");
    } else DSPrint(3,0,6, " ");
    
    if (tape_is_playing())
    {
        sprintf(tmp, "TAPE %-6d", tape_bytes_processed);
        DSPrint(5,0,6, tmp);
    }
    else 
    {
        DSPrint(5,0,6, "            ");
    }
}

// ------------------------------------------------------------------------
// Swap in a new .cas Cassette/Tape - reset position counter to zero.
// ------------------------------------------------------------------------
void CassetteInsert(char *filename)
{
    if (strstr(filename, ".tap") != 0) speccy_mode = MODE_TAP;
    if (strstr(filename, ".TAP") != 0) speccy_mode = MODE_TAP;
    if (strstr(filename, ".tzx") != 0) speccy_mode = MODE_TZX;
    if (strstr(filename, ".TZX") != 0) speccy_mode = MODE_TZX;
    FILE *inFile = fopen(filename, "rb");
    if (inFile)
    {
        last_file_size = fread(ROM_Memory, 1, MAX_CART_SIZE, inFile);
        fclose(inFile);
        tape_parse_blocks(last_file_size);
        tape_reset();
    }
}

#define MENU_ACTION_END             255 // Always the last sentinal value
#define MENU_ACTION_EXIT            0   // Exit the menu
#define MENU_ACTION_PLAY            1   // Play Cassette
#define MENU_ACTION_STOP            2   // Stop Cassette
#define MENU_ACTION_SWAP            3   // Swap Cassette
#define MENU_ACTION_REWIND          4   // Rewind Cassette
#define MENU_ACTION_POSITION        5   // Position Cassette

#define MENU_ACTION_RESET           98  // Reset the machine
#define MENU_ACTION_SKIP            99  // Skip this MENU choice

typedef struct 
{
    char *menu_string;
    u8    menu_action;
} MenuItem_t;

typedef struct 
{
    char *title;
    u8   start_row;
    MenuItem_t menulist[15];
} CassetteDiskMenu_t;

CassetteDiskMenu_t generic_cassette_menu =
{
    "CASSETTE MENU",
    5,
    {
        {" PLAY CASSETTE    ",      MENU_ACTION_PLAY},
        {" STOP CASSETTE    ",      MENU_ACTION_STOP},        
        {" SWAP CASSETTE    ",      MENU_ACTION_SWAP},
        {" REWIND CASSETTE  ",      MENU_ACTION_REWIND},
        {" POSITION CASSETTE",      MENU_ACTION_POSITION},
        {" EXIT MENU        ",      MENU_ACTION_EXIT},
        {" NULL             ",      MENU_ACTION_END},
    },
};


CassetteDiskMenu_t *menu = &generic_cassette_menu;

// ------------------------------------------------------------------------
// Show the Cassette/Disk Menu text - highlight the selected row.
// ------------------------------------------------------------------------
u8 cassette_menu_items = 0;
void CassetteMenuShow(bool bClearScreen, u8 sel)
{
    cassette_menu_items = 0;
    
    if (bClearScreen)
    {
      // ---------------------------------------------------
      // Put up a generic background for this mini-menu...
      // ---------------------------------------------------
      BottomScreenOptions();
    }
    
    // ---------------------------------------------------
    // Pick the right context menu based on the machine
    // ---------------------------------------------------
    menu = &generic_cassette_menu;
    
    // Display the menu title
    DSPrint(16-(strlen(menu->title)/2), menu->start_row, 6, menu->title);
    
    // And display all of the menu items
    while (menu->menulist[cassette_menu_items].menu_action != MENU_ACTION_END)
    {
        DSPrint(16-(strlen(menu->menulist[cassette_menu_items].menu_string)/2), menu->start_row+2+cassette_menu_items, (cassette_menu_items == sel) ? 7:6, menu->menulist[cassette_menu_items].menu_string);
        cassette_menu_items++;   
    }
    
    // ----------------------------------------------------------------------------------------------
    // And near the bottom, display the file/rom/disk/cassette that is currently loaded into memory.
    // ----------------------------------------------------------------------------------------------
    DisplayFileName();
}

// ------------------------------------------------------------------------
// Handle Cassette/Disk mini-menu interface...
// ------------------------------------------------------------------------
void CassetteMenu(void)
{
  u8 menuSelection = 0;

  SoundPause();
  while ((keysCurrent() & (KEY_TOUCH | KEY_LEFT | KEY_RIGHT | KEY_A ))!=0);

  // --------------------------------------------------------------------------------------------
  // Otherwise we are showing the cassette menu based on the current machine being emulated...
  // --------------------------------------------------------------------------------------------
  CassetteMenuShow(true, menuSelection);

  u8 bExitMenu = false;
  while (true)
  {
    nds_key = keysCurrent();
    if (nds_key)
    {
        if (nds_key & KEY_UP)
        {
            menuSelection = (menuSelection > 0) ? (menuSelection-1):(cassette_menu_items-1);
            while (menu->menulist[menuSelection].menu_action == MENU_ACTION_SKIP)
            {
                menuSelection = (menuSelection > 0) ? (menuSelection-1):(cassette_menu_items-1);
            }
            CassetteMenuShow(false, menuSelection);
        }
        if (nds_key & KEY_DOWN)
        {
            menuSelection = (menuSelection+1) % cassette_menu_items;
            while (menu->menulist[menuSelection].menu_action == MENU_ACTION_SKIP)
            {
                menuSelection = (menuSelection+1) % cassette_menu_items;
            }
            CassetteMenuShow(false, menuSelection);
        }
        if (nds_key & KEY_A)    // User has picked a menu item... let's see what it is!
        {
            switch(menu->menulist[menuSelection].menu_action)
            {
                case MENU_ACTION_EXIT:
                    bExitMenu = true;
                    break;
                    
                case MENU_ACTION_PLAY:
                    tape_play();
                    bExitMenu = true;
                    break;
                    
                case MENU_ACTION_STOP:
                    tape_stop();
                    bExitMenu = true;
                    break;

                case MENU_ACTION_SWAP:
                    speccyDSLoadFile(1);
                    if (ucGameChoice >= 0)
                    {
                        CassetteInsert(gpFic[ucGameChoice].szName);
                        bExitMenu = true;
                    }
                    else
                    {
                        CassetteMenuShow(true, menuSelection);
                    }
                    break;

                case MENU_ACTION_REWIND:
                    tape_reset();
                    bExitMenu = true;
                    break;

                case MENU_ACTION_POSITION:
                    u8 newPos = speccyTapePosition();
                    if (newPos != 255)
                    {
                        tape_position(newPos);
                        bExitMenu = true;
                    }
                    else
                    {
                        CassetteMenuShow(true, menuSelection);
                    }
                    break;
            }
        }
        if (nds_key & KEY_B)
        {
            bExitMenu = true;
        }

        if (bExitMenu) break;
        while ((keysCurrent() & (KEY_UP | KEY_DOWN | KEY_A ))!=0);
        WAITVBL;WAITVBL;
    }
  }

  while ((keysCurrent() & (KEY_UP | KEY_DOWN | KEY_A ))!=0);
  WAITVBL;WAITVBL;

  BottomScreenKeypad();  // Could be generic or overlay...

  SoundUnPause();
}



// ------------------------------------------------------------------------
// Show the Mini Menu - highlight the selected row.
// ------------------------------------------------------------------------
u8 mini_menu_items = 0;
void MiniMenuShow(bool bClearScreen, u8 sel)
{
    mini_menu_items = 0;
    if (bClearScreen)
    {
      // ---------------------------------------------------
      // Put up a generic background for this mini-menu...
      // ---------------------------------------------------
      BottomScreenOptions();
    }

    DSPrint(8,7,6,                                           " DS MINI MENU  ");
    DSPrint(8,9+mini_menu_items,(sel==mini_menu_items)?2:0,  " RESET  GAME   ");  mini_menu_items++;
    DSPrint(8,9+mini_menu_items,(sel==mini_menu_items)?2:0,  " QUIT   GAME   ");  mini_menu_items++;
    DSPrint(8,9+mini_menu_items,(sel==mini_menu_items)?2:0,  " HIGH   SCORE  ");  mini_menu_items++;
    DSPrint(8,9+mini_menu_items,(sel==mini_menu_items)?2:0,  " SAVE   STATE  ");  mini_menu_items++;
    DSPrint(8,9+mini_menu_items,(sel==mini_menu_items)?2:0,  " LOAD   STATE  ");  mini_menu_items++;
    DSPrint(8,9+mini_menu_items,(sel==mini_menu_items)?2:0,  " EXIT   MENU   ");  mini_menu_items++;
}

// ------------------------------------------------------------------------
// Handle mini-menu interface...
// ------------------------------------------------------------------------
u8 MiniMenu(void)
{
  u8 retVal = MENU_CHOICE_NONE;
  u8 menuSelection = 0;

  SoundPause();
  while ((keysCurrent() & (KEY_TOUCH | KEY_LEFT | KEY_RIGHT | KEY_A ))!=0);

  MiniMenuShow(true, menuSelection);

  while (true)
  {
    nds_key = keysCurrent();
    if (nds_key)
    {
        if (nds_key & KEY_UP)
        {
            menuSelection = (menuSelection > 0) ? (menuSelection-1):(mini_menu_items-1);
            MiniMenuShow(false, menuSelection);
        }
        if (nds_key & KEY_DOWN)
        {
            menuSelection = (menuSelection+1) % mini_menu_items;
            MiniMenuShow(false, menuSelection);
        }
        if (nds_key & KEY_A)
        {
            if      (menuSelection == 0) retVal = MENU_CHOICE_RESET_GAME;
            else if (menuSelection == 1) retVal = MENU_CHOICE_END_GAME;
            else if (menuSelection == 2) retVal = MENU_CHOICE_HI_SCORE;
            else if (menuSelection == 3) retVal = MENU_CHOICE_SAVE_GAME;
            else if (menuSelection == 4) retVal = MENU_CHOICE_LOAD_GAME;
            else if (menuSelection == 5) retVal = MENU_CHOICE_NONE;
            else retVal = MENU_CHOICE_NONE;
            break;
        }
        if (nds_key & KEY_B)
        {
            retVal = MENU_CHOICE_NONE;
            break;
        }

        while ((keysCurrent() & (KEY_UP | KEY_DOWN | KEY_A ))!=0);
        WAITVBL;WAITVBL;
    }
  }

  while ((keysCurrent() & (KEY_UP | KEY_DOWN | KEY_A ))!=0);
  WAITVBL;WAITVBL;

  BottomScreenKeypad();  // Could be generic or overlay...

  SoundUnPause();

  return retVal;
}


u8 last_special_key = 0;
u8 last_special_key_dampen = 0;
u8 last_kbd_key = 0;

u8 handle_spectrum_keyboard_press(u16 iTx, u16 iTy)  // ZX Spectrum keyboard
{
    if ((iTy >= 14) && (iTy < 48))   // Row 1 (number row)
    {
        if      ((iTx >= 0)   && (iTx < 28))   kbd_key = '1';
        else if ((iTx >= 28)  && (iTx < 54))   kbd_key = '2';
        else if ((iTx >= 54)  && (iTx < 80))   kbd_key = '3';
        else if ((iTx >= 80)  && (iTx < 106))  kbd_key = '4';
        else if ((iTx >= 106) && (iTx < 132))  kbd_key = '5';
        else if ((iTx >= 132) && (iTx < 148))  kbd_key = '6';
        else if ((iTx >= 148) && (iTx < 174))  kbd_key = '7';
        else if ((iTx >= 174) && (iTx < 200))  kbd_key = '8';
        else if ((iTx >= 200) && (iTx < 226))  kbd_key = '9';
        else if ((iTx >= 226) && (iTx < 255))  kbd_key = '0';
    }
    else if ((iTy >= 48) && (iTy < 85))  // Row 2 (QWERTY row)
    {
        if      ((iTx >= 0)   && (iTx < 28))   kbd_key = 'Q';
        else if ((iTx >= 28)  && (iTx < 54))   kbd_key = 'W';
        else if ((iTx >= 54)  && (iTx < 80))   kbd_key = 'E';
        else if ((iTx >= 80)  && (iTx < 106))  kbd_key = 'R';
        else if ((iTx >= 106) && (iTx < 132))  kbd_key = 'T';
        else if ((iTx >= 132) && (iTx < 148))  kbd_key = 'Y';
        else if ((iTx >= 148) && (iTx < 174))  kbd_key = 'U';
        else if ((iTx >= 174) && (iTx < 200))  kbd_key = 'I';
        else if ((iTx >= 200) && (iTx < 226))  kbd_key = 'O';
        else if ((iTx >= 226) && (iTx < 255))  kbd_key = 'P';
    }
    else if ((iTy >= 85) && (iTy < 122)) // Row 3 (ASDF row)
    {
        if      ((iTx >= 0)   && (iTx < 28))   kbd_key = 'A';
        else if ((iTx >= 28)  && (iTx < 54))   kbd_key = 'S';
        else if ((iTx >= 54)  && (iTx < 80))   kbd_key = 'D';
        else if ((iTx >= 80)  && (iTx < 106))  kbd_key = 'F';
        else if ((iTx >= 106) && (iTx < 132))  kbd_key = 'G';
        else if ((iTx >= 132) && (iTx < 148))  kbd_key = 'H';
        else if ((iTx >= 148) && (iTx < 174))  kbd_key = 'J';
        else if ((iTx >= 174) && (iTx < 200))  kbd_key = 'K';
        else if ((iTx >= 200) && (iTx < 226))  kbd_key = 'L';
        else if ((iTx >= 226) && (iTx < 255))  kbd_key = KBD_KEY_SYMBOL;
    }
    else if ((iTy >= 122) && (iTy < 159)) // Row 4 (ZXCV row)
    {
        if      ((iTx >= 0)   && (iTx < 28))   kbd_key = 'Z';
        else if ((iTx >= 28)  && (iTx < 54))   kbd_key = 'X';
        else if ((iTx >= 54)  && (iTx < 80))   kbd_key = 'C';
        else if ((iTx >= 80)  && (iTx < 106))  kbd_key = 'V';
        else if ((iTx >= 106) && (iTx < 132))  kbd_key = 'B';
        else if ((iTx >= 132) && (iTx < 148))  kbd_key = 'N';
        else if ((iTx >= 148) && (iTx < 174))  kbd_key = 'M';
        else if ((iTx >= 174) && (iTx < 200))  kbd_key = KBD_KEY_SHIFT;
        else if ((iTx >= 200) && (iTx < 226))  kbd_key = KBD_KEY_RET;
        else if ((iTx >= 226) && (iTx < 255))  kbd_key = KBD_KEY_RET;
    }
    else if ((iTy >= 159) && (iTy < 192)) // Row 5 (SPACE BAR and icons row)
    {
        if      ((iTx >= 1)   && (iTx < 54))   return MENU_CHOICE_CASSETTE;
        else if ((iTx >= 54)  && (iTx < 202))  kbd_key = ' ';
        else if ((iTx >= 202) && (iTx < 255))  return MENU_CHOICE_MENU;
    }
    
    DisplayStatusLine(false);
    
    return MENU_CHOICE_NONE;
}

u8 handle_debugger_overlay(u16 iTx, u16 iTy)
{
    if ((iTy >= 165) && (iTy < 192)) // Bottom row is where the debugger keys are...
    {
        if      ((iTx >= 0)   && (iTx <  26))  kbd_key = '0';
        if      ((iTx >= 26)  && (iTx <  47))  kbd_key = 'J';
        if      ((iTx >= 47)  && (iTx <  73))  kbd_key = 'P';
        if      ((iTx >= 73)  && (iTx <  97))  kbd_key = KBD_KEY_SYMBOL;
        if      ((iTx >= 97)  && (iTx < 125))  kbd_key = KBD_KEY_RET;
        
        if      ((iTx >= 125) && (iTx < 158))  return MENU_CHOICE_MENU;
        else if ((iTx >= 158) && (iTx < 192))  return MENU_CHOICE_MENU;
        else if ((iTx >= 192) && (iTx < 255))  return MENU_CHOICE_CASSETTE;
        
        DisplayStatusLine(false);
    }
    else {kbd_key = 0; last_kbd_key = 0;}

    return MENU_CHOICE_NONE;
}

u8 __attribute__((noinline)) handle_meta_key(u8 meta_key)
{
    switch (meta_key)
    {
        case MENU_CHOICE_RESET_GAME:
            SoundPause();
            // Ask for verification
            if (showMessage("DO YOU REALLY WANT TO", "RESET THE CURRENT GAME ?") == ID_SHM_YES)
            {
                ResetSpectrum();                            
            }
            BottomScreenKeypad();
            SoundUnPause();
            break;

        case MENU_CHOICE_END_GAME:
              SoundPause();
              //  Ask for verification
              if  (showMessage("DO YOU REALLY WANT TO","QUIT THE CURRENT GAME ?") == ID_SHM_YES)
              {
                  memset((u8*)0x06000000, 0x00, 0x20000);    // Reset VRAM to 0x00 to clear any potential display garbage on way out
                  return 1;
              }
              BottomScreenKeypad();
              DisplayStatusLine(true);
              SoundUnPause();
            break;

        case MENU_CHOICE_HI_SCORE:
            SoundPause();
            highscore_display(file_crc);
            DisplayStatusLine(true);
            SoundUnPause();
            break;

        case MENU_CHOICE_SAVE_GAME:
            SoundPause();
            if  (showMessage("DO YOU REALLY WANT TO","SAVE GAME STATE ?") == ID_SHM_YES)
            {
              spectrumSaveState();
            }
            BottomScreenKeypad();
            SoundUnPause();
            break;

        case MENU_CHOICE_LOAD_GAME:
            SoundPause();
            if (showMessage("DO YOU REALLY WANT TO","LOAD GAME STATE ?") == ID_SHM_YES)
            {
              spectrumLoadState();
            }
            BottomScreenKeypad();
            SoundUnPause();
            break;

        case MENU_CHOICE_CASSETTE:
            CassetteMenu();
            break;
    }

    return 0;
}

// Show tape blocks with filenames/descriptions... 
u8 speccyTapePosition(void)
{
    u8 sel = 0;

    while ((keysCurrent() & (KEY_UP | KEY_DOWN | KEY_A ))!=0);

    BottomScreenOptions();
    
    u8 max = tape_find_positions();
    DSPrint(1,3,0,"         TAPE POSITIONS         ");
    
    for (u8 i=0; i < (max < 18 ? max:18); i++)
    {
        sprintf(tmp, "%03d %-26s", TapePositionTable[i].block_id, TapePositionTable[i].description);
        DSPrint(1,4+i,(i==sel) ? 2:0,tmp);
    }
    
    while (1)
    {
        u16 keys = keysCurrent();
        if (keys & KEY_A) break;
        if (keys & KEY_DOWN)
        {
            if (sel < (max-1))
            {
                sprintf(tmp, "%03d %-26s", TapePositionTable[sel].block_id, TapePositionTable[sel].description);
                DSPrint(1,4+sel,0,tmp);
                sel++;
                sprintf(tmp, "%03d %-26s", TapePositionTable[sel].block_id, TapePositionTable[sel].description);
                DSPrint(1,4+sel,2,tmp);
                WAITVBL;WAITVBL;
            }
        }
        if (keys & KEY_UP)
        {
            if (sel > 0)
            {
                sprintf(tmp, "%03d %-26s", TapePositionTable[sel].block_id, TapePositionTable[sel].description);
                DSPrint(1,4+sel,0,tmp);
                sel--;
                sprintf(tmp, "%03d %-26s", TapePositionTable[sel].block_id, TapePositionTable[sel].description);
                DSPrint(1,4+sel,2,tmp);
                WAITVBL;WAITVBL;
            }
        }
    }
    
    while ((keysCurrent() & (KEY_UP | KEY_DOWN | KEY_A ))!=0);
    WAITVBL;WAITVBL;
    
    return sel;
}

// ------------------------------------------------------------------------
// The main emulation loop is here... call into the Z80, VDP and PSG
// ------------------------------------------------------------------------
void SpeccyDS_main(void)
{
  u16 iTx,  iTy;
  u32 ucDEUX;
  static u32 lastUN = 0;
  static u8 dampenClick = 0;
  u8 meta_key = 0;

  // Setup the debug buffer for DSi use
  debug_init();
  
  // Returns when  user has asked for a game to run...
  BottomScreenOptions();

  // Get the ZX Spectrum Emulator ready
  spectrumInit(gpFic[ucGameAct].szName);

  spectrumSetPalette();
  spectrumRun();
  
  // Frame-to-frame timing...
  TIMER1_CR = 0;
  TIMER1_DATA=0;
  TIMER1_CR=TIMER_ENABLE  | TIMER_DIV_1024;

  // Once/second timing...
  TIMER2_CR=0;
  TIMER2_DATA=0;
  TIMER2_CR=TIMER_ENABLE  | TIMER_DIV_1024;
  timingFrames  = 0;
  emuFps=0;

  // Force the sound engine to turn on when we start emulation
  bStartSoundEngine = 10;
  
  bFirstTime = 2;
  bStartTapeIn = 0;
    
  // -------------------------------------------------------------------
  // Stay in this loop running the Spectrum game until the user exits...
  // -------------------------------------------------------------------
  while(1)
  {
    // Take a tour of the Z80 counter and display the screen if necessary
    if (!speccy_run())
    {
        // If we've been asked to start the sound engine, rock-and-roll!
        if (bStartSoundEngine)
        {
              if (--bStartSoundEngine == 0) SoundUnPause();
        }

        // -------------------------------------------------------------
        // Stuff to do once/second such as FPS display and Debug Data
        // -------------------------------------------------------------
        if (TIMER1_DATA >= 32728)   //  1000MS (1 sec)
        {
            char szChai[4];

            TIMER1_CR = 0;
            TIMER1_DATA = 0;
            TIMER1_CR=TIMER_ENABLE | TIMER_DIV_1024;
            emuFps = emuActFrames;
            if (myGlobalConfig.showFPS)
            {
                // Due to minor sampling of the frame rate, 49,50 and 51
                // pretty much all represent full-speed so just show 50fps.
                if (emuFps == 51) emuFps=50;
                else if (emuFps == 49) emuFps=50;
                if (emuFps/100) szChai[0] = '0' + emuFps/100;
                else szChai[0] = ' ';
                szChai[1] = '0' + (emuFps%100) / 10;
                szChai[2] = '0' + (emuFps%100) % 10;
                szChai[3] = 0;
                DSPrint(0,0,6,szChai);
            }
            DisplayStatusLine(false);
            emuActFrames = 0;
            
            if (bStartTapeIn)
            {
                if (--bStartTapeIn == 0)
                {
                    if (!tape_is_playing()) tape_play();
                }
            }

            if (bFirstTime)
            {
                if (--bFirstTime == 0)
                {
                    // Tape Loader - Put the LOAD "" into the keyboard buffer 
                    if (speccy_mode < MODE_SNA)
                    {
                        if (myConfig.autoLoad)
                        {
                            BufferKey('J'); BufferKey(KBD_KEY_SYMBOL); BufferKey('P'); BufferKey(KBD_KEY_SYMBOL); BufferKey('P'); BufferKey(KBD_KEY_RET);
                            bStartTapeIn = 2;
                        }
                    }
                }
            }
        }
        emuActFrames++;

        // -------------------------------------------------------------------
        // We only support PAL 50 frames as this is a ZED-X Speccy!
        // -------------------------------------------------------------------
        if (++timingFrames == 50)
        {
            TIMER2_CR=0;
            TIMER2_DATA=0;
            TIMER2_CR=TIMER_ENABLE | TIMER_DIV_1024;
            timingFrames = 0;
        }

        // ----------------------------------------------------------------------
        // 32,728.5 ticks of TIMER2 = 1 second
        // 1 frame = 1/50 or 655 ticks of TIMER2
        //
        // This is how we time frame-to frame to keep the game running at 50FPS
        // ----------------------------------------------------------------------
        while (TIMER2_DATA < 655*(timingFrames+1))
        {
            if (myGlobalConfig.showFPS == 2) break;   // If Full Speed, break out...
            if (tape_is_playing()) 
            {
                mixer_read = mixer_write = 0;
                bStartSoundEngine = 5;  // Unpause sound after 5 frames
                SoundPause();           // But for now, keep muted while we load
                break;                  // With tape playing, speedup to allow faster load
            }
        }

       // We've run one frame of timing... let the tape player know
       tape_frame();

      // If the Z80 Debugger is enabled, call it
      if (myGlobalConfig.debugger >= 2)
      {
          ShowDebugZ80();
      }

      // --------------------------------------------------------------
      // Hold the key press for a brief instant... To allow the 
      // emulated ZX Spectrum to 'see' the key briefly... Good enough.
      // --------------------------------------------------------------
      if (key_debounce > 0) key_debounce--;
      else
      {
          // -----------------------------------------------------------
          // This is where we accumualte the keys pressed... up to 12!
          // -----------------------------------------------------------
          kbd_keys_pressed = 0;
          memset(kbd_keys, 0x00, sizeof(kbd_keys));
          kbd_key = 0;

          // ------------------------------------------
          // Handle any screen touch events
          // ------------------------------------------
          if  (keysCurrent() & KEY_TOUCH)
          {
              // ------------------------------------------------------------------------------------------------
              // Just a tiny bit of touch debounce so ensure touch screen is pressed for a fraction of a second.
              // ------------------------------------------------------------------------------------------------
              if (++touch_debounce > 1)
              {
                touchPosition touch;
                touchRead(&touch);
                iTx = touch.px;
                iTy = touch.py;

                if (myGlobalConfig.debugger == 3)
                {
                    meta_key = handle_debugger_overlay(iTx, iTy);
                }
                // ------------------------------------------------------------
                // Test the touchscreen for various full keyboard handlers... 
                // ------------------------------------------------------------
                else
                {
                    meta_key = handle_spectrum_keyboard_press(iTx, iTy);
                }                  

                if (kbd_key != 0)
                {
                    kbd_keys[kbd_keys_pressed++] = kbd_key;
                    key_debounce = 2;
                }

                // If the special menu key indicates we should show the choice menu, do so here...
                if (meta_key == MENU_CHOICE_MENU)
                {
                    meta_key = MiniMenu();
                }

                // -------------------------------------------------------------------
                // If one of the special meta keys was picked, we handle that here...
                // -------------------------------------------------------------------
                if (handle_meta_key(meta_key)) return;

                if (++dampenClick > 0)  // Make sure the key is pressed for an appreciable amount of time...
                {
                    if ((kbd_key != 0) && (lastUN == 0))
                    {
                         mmEffect(SFX_KEYCLICK);  // Play short key click for feedback...
                    }
                    lastUN = kbd_key;
                }
              }
          } //  SCR_TOUCH
          else
          {
            touch_debounce = 0;
            lastUN = 0;  dampenClick = 0;
            last_kbd_key = 0;
          }
      }


      // ------------------------------------------------------------------------
      //  Test DS keypresses (ABXY, L/R) and map to corresponding Spectrum keys
      // ------------------------------------------------------------------------
      ucDEUX  = 0;
      nds_key  = keysCurrent();     // Get any current keys pressed on the NDS
         
      {
          if ((nds_key & KEY_L) && (nds_key & KEY_R) && (nds_key & KEY_X))
          {
                lcdSwap();
                WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;
          }
          else if ((nds_key & KEY_L) && (nds_key & KEY_R))
          {
                DSPrint(5,0,0,"SNAPSHOT");
                screenshot();
                debug_save();
                WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;WAITVBL;
                DSPrint(5,0,0,"        ");
          }
          else if  (nds_key & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_START | KEY_SELECT | KEY_R | KEY_L | KEY_X | KEY_Y))
          {
              // --------------------------------------------------------------------------------------------------
              // There are 12 NDS buttons (D-Pad, XYAB, L/R and Start+Select) - we allow mapping of any of these.
              // --------------------------------------------------------------------------------------------------
              for (u8 i=0; i<12; i++)
              {
                  if (nds_key & NDS_keyMap[i])
                  {
                      if (keyCoresp[myConfig.keymap[i]] < 0xF000)   // Normal key map
                      {
                          ucDEUX  |= keyCoresp[myConfig.keymap[i]];
                      }
                      else // This is a keyboard maping... handle that here... just set the appopriate kbd_key
                      {
                          if      ((keyCoresp[myConfig.keymap[i]] >= META_KBD_A) && (keyCoresp[myConfig.keymap[i]] <= META_KBD_Z))  kbd_key = ('A' + (keyCoresp[myConfig.keymap[i]] - META_KBD_A));
                          else if ((keyCoresp[myConfig.keymap[i]] >= META_KBD_0) && (keyCoresp[myConfig.keymap[i]] <= META_KBD_9))  kbd_key = ('0' + (keyCoresp[myConfig.keymap[i]] - META_KBD_0));
                          else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SPACE)     kbd_key     = ' ';
                          else if (keyCoresp[myConfig.keymap[i]] == META_KBD_RETURN)    kbd_key     = KBD_KEY_RET;
                          else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SHIFT)     {kbd_key     = KBD_KEY_SHIFT;DisplayStatusLine(false);}
                          else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SYMBOL)    {kbd_key     = KBD_KEY_SYMBOL;DisplayStatusLine(false);}

                          if (kbd_key != 0)
                          {
                              kbd_keys[kbd_keys_pressed++] = kbd_key;
                          }
                      }
                  }
              }
          }
          else
          {
              last_mapped_key = 0;
          }
      }
      
      // ------------------------------------------------------------------------------------------
      // Finally, check if there are any buffered keys that need to go into the keyboard handling.
      // ------------------------------------------------------------------------------------------
      ProcessBufferedKeys();

      // ---------------------------------------------------------
      // Accumulate all bits above into the Joystick State var...
      // ---------------------------------------------------------
      JoyState = ucDEUX;
      
      // --------------------------------------------------
      // Handle Auto-Fire if enabled in configuration...
      // --------------------------------------------------
      static u8 autoFireTimer=0;
      if (myConfig.autoFire && (JoyState & JST_FIRE))  // Fire Button
      {
         if ((++autoFireTimer & 7) > 4)  JoyState &= ~JST_FIRE;
      }
    }
  }
}


// ----------------------------------------------------------------------------------------
// We steal 256K of the VRAM to hold a shadow copy of the ROM cart for fast swap...
// ----------------------------------------------------------------------------------------
void useVRAM(void)
{
  vramSetBankD(VRAM_D_LCD );        // Not using this for video but 128K of faster RAM always useful!  Mapped at 0x06860000 -   Used for tape patch look-up
  vramSetBankE(VRAM_E_LCD );        // Not using this for video but 64K of faster RAM always useful!   Mapped at 0x06880000 -   ..
  vramSetBankF(VRAM_F_LCD );        // Not using this for video but 16K of faster RAM always useful!   Mapped at 0x06890000 -   ..
  vramSetBankG(VRAM_G_LCD );        // Not using this for video but 16K of faster RAM always useful!   Mapped at 0x06894000 -   ..
  vramSetBankH(VRAM_H_LCD );        // Not using this for video but 32K of faster RAM always useful!   Mapped at 0x06898000 -   ..
  vramSetBankI(VRAM_I_LCD );        // Not using this for video but 16K of faster RAM always useful!   Mapped at 0x068A0000 -   ..
}

/*********************************************************************************
 * Init DS Emulator - setup VRAM banks and background screen rendering banks
 ********************************************************************************/
void speccyDSInit(void)
{
  //  Init graphic mode (bitmap mode)
  videoSetMode(MODE_0_2D  | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_SPR_1D_LAYOUT | DISPLAY_SPR_ACTIVE);
  videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE  | DISPLAY_BG1_ACTIVE | DISPLAY_SPR_1D_LAYOUT | DISPLAY_SPR_ACTIVE);
  vramSetBankA(VRAM_A_MAIN_BG);
  vramSetBankB(VRAM_B_MAIN_SPRITE);          // Once emulation of game starts, we steal this back for an additional 128K of VRAM at 0x6820000 which we will use as a snapshot buffer for taking screen pics
  vramSetBankC(VRAM_C_SUB_BG);

  //  Stop blending effect of intro
  REG_BLDCNT=0; REG_BLDCNT_SUB=0; REG_BLDY=0; REG_BLDY_SUB=0;

  //  Render the top screen
  bg0 = bgInit(0, BgType_Text8bpp,  BgSize_T_256x512, 31,0);
  bg1 = bgInit(1, BgType_Text8bpp,  BgSize_T_256x512, 29,0);
  bgSetPriority(bg0,1);bgSetPriority(bg1,0);
  decompress(topscreenTiles,  bgGetGfxPtr(bg0), LZ77Vram);
  decompress(topscreenMap,  (void*) bgGetMapPtr(bg0), LZ77Vram);
  dmaCopy((void*) topscreenPal,(void*)  BG_PALETTE,256*2);
  unsigned  short dmaVal =*(bgGetMapPtr(bg0)+51*32);
  dmaFillWords(dmaVal | (dmaVal<<16),(void*)  bgGetMapPtr(bg1),32*24*2);

  // Put up the options screen 
  BottomScreenOptions();

  //  Find the files
  speccyDSFindFiles(0);
}


void BottomScreenOptions(void)
{
    swiWaitForVBlank();
    
    // ---------------------------------------------------
    // Put up the options select screen background...
    // ---------------------------------------------------
    bg0b = bgInitSub(0, BgType_Text8bpp, BgSize_T_256x256, 31,0);
    bg1b = bgInitSub(1, BgType_Text8bpp, BgSize_T_256x256, 29,0);
    bgSetPriority(bg0b,1);bgSetPriority(bg1b,0);
    
    decompress(mainmenuTiles, bgGetGfxPtr(bg0b), LZ77Vram);
    decompress(mainmenuMap, (void*) bgGetMapPtr(bg0b), LZ77Vram);
    dmaCopy((void*) mainmenuPal,(void*) BG_PALETTE_SUB,256*2);
    
    unsigned short dmaVal = *(bgGetMapPtr(bg1b)+24*32);
    dmaFillWords(dmaVal | (dmaVal<<16),(void*) bgGetMapPtr(bg1b),32*24*2);
}

// ---------------------------------------------------------------------------
// Setup the bottom screen - mostly for menu, high scores, options, etc.
// ---------------------------------------------------------------------------
void BottomScreenKeypad(void)
{
    swiWaitForVBlank();
    
    if (myGlobalConfig.debugger == 3)  // Full Z80 Debug overrides things... put up the debugger overlay
    {
      //  Init bottom screen
      decompress(debug_ovlTiles, bgGetGfxPtr(bg0b),  LZ77Vram);
      decompress(debug_ovlMap, (void*) bgGetMapPtr(bg0b),  LZ77Vram);
      dmaCopy((void*) bgGetMapPtr(bg0b)+32*30*2,(void*) bgGetMapPtr(bg1b),32*24*2);
      dmaCopy((void*) debug_ovlPal,(void*) BG_PALETTE_SUB,256*2);
    }
    else
    {
      //  Init bottom screen for Spectrum Keyboard
      decompress(speccy_kbdTiles, bgGetGfxPtr(bg0b),  LZ77Vram);
      decompress(speccy_kbdMap, (void*) bgGetMapPtr(bg0b),  LZ77Vram);
      dmaCopy((void*) bgGetMapPtr(bg0b)+32*30*2,(void*) bgGetMapPtr(bg1b),32*24*2);
      dmaCopy((void*) speccy_kbdPal,(void*) BG_PALETTE_SUB,256*2);
    }
    
    unsigned  short dmaVal = *(bgGetMapPtr(bg1b)+24*32);
    dmaFillWords(dmaVal | (dmaVal<<16),(void*)  bgGetMapPtr(bg1b),32*24*2);

    DisplayStatusLine(true);
}

/*********************************************************************************
 * Init CPU for the current game
 ********************************************************************************/
void speccyDSInitCPU(void)
{
  //  -----------------------------------------
  //  Init Main Memory and VDP Video Memory
  //  -----------------------------------------
  memset(RAM_Memory,    0x00, sizeof(RAM_Memory));
  memset(RAM_Memory128, 0x00, sizeof(RAM_Memory128));

  // -----------------------------------------------
  // Init bottom screen do display the ZX Keyboard
  // -----------------------------------------------
  BottomScreenKeypad();
}

// -------------------------------------------------------------
// Only used for basic timing of splash screen fade-out
// -------------------------------------------------------------
void irqVBlank(void)
{
 // Manage time
  vusCptVBL++;
}

// ----------------------------------------------------------------------
// Look for the 48.rom and 128.rom bios in several possible locations...
// ----------------------------------------------------------------------
void LoadBIOSFiles(void)
{
    int size = 0;

    // --------------------------------------------------
    // We will look for the 48K and 128K BIOS ROMs
    // --------------------------------------------------
    bSpeccyBiosFound = false;

    // -----------------------------------------------------------
    // Next try to load the Spectrum BIOS files
    // -----------------------------------------------------------
    size = ReadFileCarefully("48.rom", SpectrumBios, 0x4000, 0);
    if (!size) size = ReadFileCarefully("/roms/bios/48.rom", SpectrumBios, 0x4000, 0);
    if (!size) size = ReadFileCarefully("/data/bios/48.rom", SpectrumBios, 0x4000, 0);
    
    if (!size) size = ReadFileCarefully("48k.rom", SpectrumBios, 0x4000, 0);
    if (!size) size = ReadFileCarefully("/roms/bios/48k.rom", SpectrumBios, 0x4000, 0);
    if (!size) size = ReadFileCarefully("/data/bios/48k.rom", SpectrumBios, 0x4000, 0);

    if (!size) size = ReadFileCarefully("speccy.rom", SpectrumBios, 0x4000, 0);
    if (!size) size = ReadFileCarefully("/roms/bios/speccy.rom", SpectrumBios, 0x4000, 0);
    if (!size) size = ReadFileCarefully("/data/bios/speccy.rom", SpectrumBios, 0x4000, 0);
    
    if (!size) size = ReadFileCarefully("zxs48.rom", SpectrumBios, 0x4000, 0);
    if (!size) size = ReadFileCarefully("/roms/bios/zxs48.rom", SpectrumBios, 0x4000, 0);
    if (!size) size = ReadFileCarefully("/data/bios/zxs48.rom", SpectrumBios, 0x4000, 0);
    
    if (!size) size = ReadFileCarefully("spec48.rom", SpectrumBios, 0x4000, 0);
    if (!size) size = ReadFileCarefully("/roms/bios/spec48.rom", SpectrumBios, 0x4000, 0);
    if (!size) size = ReadFileCarefully("/data/bios/spec48.rom", SpectrumBios, 0x4000, 0);
    
    if (size) bSpeccyBiosFound = true; else memset(SpectrumBios, 0xFF, 0x4000);

    size = ReadFileCarefully("128.rom", SpectrumBios128, 0x8000, 0);
    if (!size) size = ReadFileCarefully("/roms/bios/128.rom", SpectrumBios128, 0x8000, 0);
    if (!size) size = ReadFileCarefully("/data/bios/128.rom", SpectrumBios128, 0x8000, 0);
    
    if (!size) size = ReadFileCarefully("128k.rom", SpectrumBios128, 0x8000, 0);
    if (!size) size = ReadFileCarefully("/roms/bios/128k.rom", SpectrumBios128, 0x8000, 0);
    if (!size) size = ReadFileCarefully("/data/bios/128k.rom", SpectrumBios128, 0x8000, 0);
    
    if (!size) size = ReadFileCarefully("zxs128.rom", SpectrumBios128, 0x8000, 0);
    if (!size) size = ReadFileCarefully("/roms/bios/zxs128.rom", SpectrumBios128, 0x8000, 0);
    if (!size) size = ReadFileCarefully("/data/bios/zxs128.rom", SpectrumBios128, 0x8000, 0);
    
    if (size) bSpeccyBiosFound = true; else memset(SpectrumBios128, 0xFF, 0x8000);
}

/*********************************************************************************
 * Program entry point - check if an argument has been passed in probably from TWL++
 ********************************************************************************/
int main(int argc, char **argv)
{
  //  Init sound
  consoleDemoInit();

  if  (!fatInitDefault()) {
     iprintf("Unable to initialize libfat!\n");
     return -1;
  }

  highscore_init();

  lcdMainOnTop();

  //  Init timer for frame management
  TIMER2_DATA=0;
  TIMER2_CR=TIMER_ENABLE|TIMER_DIV_1024;
  dsInstallSoundEmuFIFO();

  //  Show the fade-away intro logo...
  intro_logo();

  SetYtrigger(190); //trigger 2 lines before vsync

  irqSet(IRQ_VBLANK,  irqVBlank);
  irqEnable(IRQ_VBLANK);

  // -----------------------------------------------------------------
  // Grab the BIOS before we try to switch any DIRECTORYories around...
  // -----------------------------------------------------------------
  useVRAM();
  LoadBIOSFiles();
  
  // -----------------------------------------------------------------
  // And do an initial load of configuration... We'll match it up
  // with the game that was selected later...
  // -----------------------------------------------------------------
  LoadConfig();

  //  Handle command line argument... mostly for TWL++
  if  (argc > 1)
  {
      //  We want to start in the DIRECTORYory where the file is being launched...
      if  (strchr(argv[1], '/') != NULL)
      {
          static char  path[128];
          strcpy(path,  argv[1]);
          char  *ptr = &path[strlen(path)-1];
          while (*ptr !=  '/') ptr--;
          ptr++;
          strcpy(cmd_line_file,  ptr);
          *ptr=0;
          chdir(path);
      }
      else
      {
          strcpy(cmd_line_file,  argv[1]);
      }
  }
  else
  {
      cmd_line_file[0]=0; // No file passed on command line...
      chdir("/roms");     // Try to start in roms area... doesn't matter if it fails
      chdir("speccy");    // And try to start in the subdir /speccy... doesn't matter if it fails.
      chdir("zx");        // And try to start in the subdir /zx... doesn't matter if it fails.
      chdir("spectrum");  // And try to start in the subdir /spectrum... doesn't matter if it fails.
  }

  SoundPause();
  
  srand(time(NULL));

  //  ------------------------------------------------------------
  //  We run this loop forever until game exit is selected...
  //  ------------------------------------------------------------
  while(1)
  {
    speccyDSInit();

    // ---------------------------------------------------------------
    // Let the user know what BIOS files were found - the only BIOS
    // that must exist is 48.rom or else the show is off...
    // ---------------------------------------------------------------
    if (!bSpeccyBiosFound)
    {
        DSPrint(2,10,0,"ERROR: ZX SPECTRUM 48.rom");
        DSPrint(2,12,0,"       WAS NOT FOUND");
        DSPrint(2,14,0,"Put 48.rom in same dir");
        DSPrint(2,15,0,"as EMULATOR or /ROMS/BIOS");
        while(1) ;  // We're done... Need a Spectrum bios to run this emulator
    }

    while(1)
    {
      SoundPause();
      //  Choose option
      if  (cmd_line_file[0] != 0)
      {
          ucGameChoice=0;
          ucGameAct=0;
          strcpy(gpFic[ucGameAct].szName, cmd_line_file);
          cmd_line_file[0] = 0;    // No more initial file...
          ReadFileCRCAndConfig(); // Get CRC32 of the file and read the config/keys
      }
      else
      {
          speccyDSChangeOptions();
      }

      //  Run Machine
      speccyDSInitCPU();
      SpeccyDS_main();
    }
  }
  return(0);
}


// -----------------------------------------------------------------------
// The code below is a handy set of debug tools that allows us to 
// write printf() like strings out to a file. Basically we accumulate
// the strings into a large RAM buffer and then when the L+R shoulder
// buttons are pressed and held, we will snapshot out the debug.log file.
// The DS-Lite only gets a small 16K debug buffer but the DSi gets 4MB!
// -----------------------------------------------------------------------

#define MAX_DPRINTF_STR_SIZE  256
u32     MAX_DEBUG_BUF_SIZE  = 0;

char *debug_buffer = 0;
u32  debug_len = 0;
extern char szName[]; // Reuse buffer which has no other in-game use

void debug_init()
{
    if (!debug_buffer)
    {
        if (isDSiMode()) 
        {
            MAX_DEBUG_BUF_SIZE = (1024*1024*2); // 2MB!!
            debug_buffer = malloc(MAX_DEBUG_BUF_SIZE);
        }
        else
        {
            MAX_DEBUG_BUF_SIZE = (1024*16);     // 16K only
            debug_buffer = malloc(MAX_DEBUG_BUF_SIZE);
        }
    }
    memset(debug_buffer, 0x00, MAX_DEBUG_BUF_SIZE);
    debug_len = 0;
}

void debug_printf(const char * str, ...)
{
    va_list ap = {0};

    va_start(ap, str);
    vsnprintf(szName, MAX_DPRINTF_STR_SIZE, str, ap);
    va_end(ap);

    strcat(debug_buffer, szName);
    debug_len += strlen(szName);
}

void debug_save()
{
    if (debug_len > 0) // Only if we have debug data to write...
    {
        FILE *fp = fopen("debug.log", "w");
        if (fp)
        {
            fwrite(debug_buffer, 1, debug_len, fp);
            fclose(fp);
        }
    }
}

// End of file
