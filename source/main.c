/****************************************************************************
* ds bomb gamecube memory card manager
*based on libOGC Memory Card Backup by askot
* delete support + saves information made by dsbomb
* Gui original design by dsbomb
* Gui adds and user interaction by justb
* Uses freetype.
* libFreeType is available from the downloads sections.
*****************************************************************************/
#include <gccore.h>
#include <ogcsys.h>
#include <network.h>
#include <smb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fat.h>
#include <sys/dir.h>
#include <sys/time.h>

#ifdef HW_RVL
#include <wiiuse/wpad.h>
#endif

#include "mcard.h"
#include "sdsupp.h"
#include "freetype.h"
#include "bitmap.h"

#define PSOSDLOADID 0x7c6000a6
int mode;
int cancel;
/*** 2D Video Globals ***/
GXRModeObj *vmode;		/*** Graphics Mode Object ***/
u32 *xfb[2] = { NULL, NULL };	/*** Framebuffers ***/
int whichfb = 0;		/*** Frame buffer toggle ***/
int screenheight;
static int vmode_60hz = 0;

extern u8 filelist[1024][1024];

static void updatePAD(){
	PAD_ScanPads();
#ifdef HW_RVL
	WPAD_ScanPads();
#endif
}
static int initFAT(){
//init fat filesystem	
	fatInit(8, false);
	//sleep(2); // espero dos segundos, no se muy bien si para darle tiempo a que el dispositivo negocie o est� preparado para funcionar
	//fatSetDefaultInterface(PI_INTERNAL_SD); // usa SD como fat:
	int have_device=0; // bits que me indican la presencia de dispositivos
	DIR_ITER *dir;
	char path_initfat[]="fatX:/";
#ifdef	HW_RVL
	path_initfat[3]=48+PI_INTERNAL_SD; // path_file ="fatX:/";
#else
	path_initfat[3]=48+PI_SDGECKO_A; // path_file ="fatX:/";
#endif
	
	int n;
	for(n=0;n<5;n++) // numero de reintentos por si la unidad no est� preparada o est� "enojada" :lol:
	{
		dir = diropen(path_initfat);
		if (dir) {
			dirclose(dir);
				//have_device|=1;
				have_device=1;
			#ifdef	HW_RVL				
				fatEnableReadAhead(PI_INTERNAL_SD, 12, 32);// monta la cache
			#endif
				break;
		} 
		//usleep(200*1000);
	}
	
	return have_device;

}

/****************************************************************************
* Initialise Video
*
* Before doing anything in libogc, it's recommended to configure a video
* output.
****************************************************************************/
static void
Initialise (void)
{
  VIDEO_Init ();		/*** ALWAYS CALL FIRST IN ANY LIBOGC PROJECT!
				     Not only does it initialise the video
				     subsystem, but also sets up the ogc os
				***/

  PAD_Init ();			/*** Initialise pads for input ***/
  #ifdef HW_RVL
  WPAD_Init ();
  #endif

	// get default video mode
	vmode = VIDEO_GetPreferredMode(NULL);

	switch (vmode->viTVMode >> 2)
	{
		case VI_PAL:
			// 576 lines (PAL 50Hz)
			// display should be centered vertically (borders)
			//Make all video modes the same size so menus doesn't screw up
			vmode = &TVPal574IntDfScale;
			vmode->xfbHeight = 480;
			vmode->viYOrigin = (VI_MAX_HEIGHT_PAL - 480)/2;
			vmode->viHeight = 480;

			vmode_60hz = 0;
			break;

		case VI_NTSC:
			// 480 lines (NTSC 60hz)
			vmode_60hz = 1;
			break;

		default:
			// 480 lines (PAL 60Hz)
			vmode_60hz = 1;
			break;
	}

#ifdef HW_DOL
/* we have component cables, but the preferred mode is interlaced
 * why don't we switch into progressive?
 * on the Wii, the user can do this themselves on their Wii Settings */
	if(VIDEO_HaveComponentCable())
		vmode = &TVNtsc480Prog;
#endif

/*	// check for progressive scan // bool progressive = FALSE;
	if (vmode->viTVMode == VI_TVMODE_NTSC_PROG)
		progressive = true;
*/

#ifdef HW_RVL
	// widescreen fix
	if(CONF_GetAspectRatio())
	{
		vmode->viWidth = 678;
		vmode->viXOrigin = (VI_MAX_WIDTH_PAL - 678) / 2;
	}
#endif

	// configure VI
	VIDEO_Configure (vmode);

	// always 480 lines /*** Update screen height for font engine ***/
	screenheight = vmode->xfbHeight;

	/*** Now configure the framebuffer.
	     Really a framebuffer is just a chunk of memory
	     to hold the display line by line.
	***/
	// Allocate the video buffers
	xfb[0] = (u32 *) MEM_K0_TO_K1 (SYS_AllocateFramebuffer (vmode));
	/*** I prefer also to have a second buffer for double-buffering.
	     This is not needed for the console demo.
	***/
	xfb[1] = (u32 *) MEM_K0_TO_K1 (SYS_AllocateFramebuffer (vmode));

	/*** Define a console ***/
  console_init (xfb[0], 20, 64, vmode->fbWidth, vmode->xfbHeight, vmode->fbWidth * 2);

	/*** Clear framebuffer to black ***/
  VIDEO_ClearFrameBuffer (vmode, xfb[0], COLOR_BLACK);
  VIDEO_ClearFrameBuffer (vmode, xfb[1], COLOR_BLACK);

	/*** Set the framebuffer to be displayed at next VBlank ***/
  VIDEO_SetNextFramebuffer (xfb[0]);

	/*** Get the PAD status updated by libogc ***/
  VIDEO_SetPostRetraceCallback (updatePAD);
  VIDEO_SetBlack (0);

	/*** Update the video for next vblank ***/
  VIDEO_Flush ();

  VIDEO_WaitVSync ();		/*** Wait for VBL ***/
  if (vmode->viTVMode & VI_NON_INTERLACE)
    VIDEO_WaitVSync ();

}



/****************************************************************************
* BackupMode -SD Mode
*
* Perform backup of a memory card file from a SD Card.
****************************************************************************/
void SD_BackupMode (){
  int memitems;
  int selected = 0;
  int bytestodo;

  clearRightPane();
  DrawText(390,130,"B a c k u p   M o d e");
  writeStatusBar("Pick a file using UP or DOWN ", "Press A to backup to SD Card ") ;
	/*** Get the directory listing from the memory card ***/
  memitems = CardGetDirectory (CARD_SLOTB);

	/*** If it's a blank card, get out of here ***/
  if (!memitems) {WaitPrompt ("No saved games to backup !");}
else
{
  selected = ShowSelector ();
  if (cancel) {WaitPrompt ("Backup action cancelled !");}
    else
{
	/*** Backup the file ***/
  ShowAction ("Reading File From MC SLOT B");
  bytestodo = CardReadFile (CARD_SLOTB, selected);
  if (bytestodo){
     ShowAction ("Saving to SD CARD");
     if (SDSaveMCImage ()){
	    WaitPrompt ("Backup complete");
     }
     else{
	     WaitPrompt ("Backup failed");
      }
  }
  else{
       WaitPrompt ("Error reading MC file");
        }

  }
}


}

/****************************************************************************
* RestoreMode
*
* Restore a file to Memory Card from SD Card
****************************************************************************/
void SD_RestoreMode (){
  int files;
  int selected;

 clearRightPane();
 DrawText(390,130,"R e s t o r e   M o d e");
 writeStatusBar("Pick a file using UP or DOWN", "Press A to restore to Memory Card ") ;
  files = SDGetFileList ();
  if (!files) {WaitPrompt ("No saved games in SD Card to restore !");}

else
{
  selected = ShowSelector ();

 if (cancel) {WaitPrompt ("Restore action cancelled !");}
 else
{
  ShowAction ("Reading from SD Card");
  if (SDLoadMCImage (filelist[selected])){
      ShowAction ("Updating Memory Card");
      if (CardWriteFile (CARD_SLOTB)){
         WaitPrompt ("Restore Complete");
      }
      else {
         WaitPrompt ("Error during restore");
      }
  }
  else{
      WaitPrompt ("Error reading image");
  }
}



}

}

/****************************************************************************
* TestMode
*
* 
****************************************************************************/
/*void testMode (){

 clearRightPane();
 DrawText(390,130,"T e s t   M o d e");
 
  int bytestodo;

  clearRightPane();


  ShowAction ("Reading From MC SLOT B");
  bytestodo = testreadimage (CARD_SLOTB);
  if (bytestodo){
     ShowAction ("Saving to SD CARD");
     if (SDSaveraw ()){
	    WaitPrompt ("Backup complete");
     }
     else{
	     WaitPrompt ("Backup failed");
      }
  }
  else{
       WaitPrompt ("Error reading MC file");
  }

// writeStatusBar("Pick a file using UP or DOWN", "Press A to restore to Memory Card ") ;


//WaitPrompt ("No saved games in SD Card to restore !");
//ShowAction ("Reading from SD Card");


}*/

/* Reboot the system */
/*void Reboot() {
    *((volatile u32*)0xCC003024) = 0x00000000;
}
*/
/****************************************************************************
* Main
****************************************************************************/
int main (){


#ifdef HW_DOL
  int *psoid = (int *) 0x80001800;
  void (*PSOReload) () = (void (*)()) 0x80001800;
#endif

  Initialise ();	/*** Start video ***/
  FT_Init ();		/*** Start FreeType ***/
  if(!initFAT())
	WaitPrompt("No SDCard!");

#ifdef HW_RVL
  initialise_power();
#endif

  for (;;){
		/*** Select Mode ***/
      ClearScreen();
      cancel = 0;/******a global value to track action aborted by user pressing button B********/
      mode = SelectMode ();


	  /*** Mode == 100 for backup, 200 for restore ***/
      switch (mode){
         case 100 :
			//SMB_BackupMode();
            //WaitPrompt ("Inactive");
            break;
         case 200 :
            MC_DeleteMode(CARD_SLOTB);
            break;
         case 300 :
            SD_BackupMode();
            break;
         case 400 :
            SD_RestoreMode();
            break;
         case 500 :
			#ifdef HW_RVL
			//if there's a loader stub load it, if not return to wii menu.
			if (!!*(u32*)0x80001800) exit(1);
			else SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
			#else
			if (psoid[0] == PSOSDLOADID) PSOReload ();
			else SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
			#endif
            break; //PSO_Reload
      }
  }
  while (1);
  return 0;
}