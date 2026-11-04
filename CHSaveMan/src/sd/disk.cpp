#ifndef CHSIM   // the board's card; the simulator uses sdio_sim.cpp
/* SPDX-License-Identifier: GPL-3.0-or-later */
// From CHGame's CHSDtoSerial app (disk.cpp). Changed here: the CHSIM guard, the include paths, and a 250 ms
// CMD0 timeout (Sd2Card::init's cmd0Timeout, as CHSDtoUSB probes): an empty slot costs 250 ms, not 2 s.
#include <Arduino.h>
#include "Sd2Card.h"
extern "C" {
#include "../fatfs/ff.h"
#include "../fatfs/diskio.h"
}
static Sd2Card card;
static DSTATUS state=STA_NOINIT;
extern "C" { uint8_t chsd_init_err=0; uint32_t chweb_error_sector=0; uint8_t chweb_error_code=0,chweb_error_operation=0; }
// CHSaveMan: chsd_stream = 1 keeps a CMD18 read open across disk_read calls while the sectors follow on (the
// scan reads a .CHG 512 B at a time: one CMD18 + CMD12 per sector cost ~2 ms). chsd_end() must close it before
// anything else uses SPI1 (the panel flush); any other card call closes it first.
static bool s_open=false; static LBA_t s_next=0;
extern "C" { uint8_t chsd_stream=0, chsd_fail=0; }
static void streamEnd(){if(s_open){card.readStop();s_open=false;}}
extern "C" void chsd_end(void){streamEnd();}
extern "C" DSTATUS disk_initialize(BYTE drive){if(drive)return STA_NOINIT;streamEnd();bool ok=card.init(SPI_FULL_SPEED,PIN_SD_CS,250);if(ok)ok=card.crcOn();chsd_init_err=ok?0:(card.errorCode()?card.errorCode():0xFF);state=ok?0:STA_NOINIT;return state;}
extern "C" DSTATUS disk_status(BYTE drive){return drive?STA_NOINIT:state;}
extern "C" DRESULT disk_read(BYTE drive,BYTE *buf,LBA_t sector,UINT count){
 if(drive||state)return RES_NOTRDY;
 if(s_open&&sector!=s_next)streamEnd();
 for(UINT i=0;i<count;i++){bool ok=false;for(int retry=0;retry<4&&!ok;retry++){if(!s_open)s_open=card.readStart(sector+i);ok=s_open&&card.readBlockChecked(buf+512*i);if(!ok)streamEnd();}if(!ok){chsd_fail=1;chweb_error_sector=sector+i;chweb_error_code=card.errorCode();chweb_error_operation=1;return RES_ERROR;}}
 s_next=sector+count;
 if(chsd_stream)return RES_OK;
 bool ok=card.readStop();s_open=false;return ok?RES_OK:RES_ERROR;
}
extern "C" DRESULT disk_write(BYTE drive,const BYTE *buf,LBA_t sector,UINT count){
 if(drive||state)return RES_NOTRDY;
 streamEnd();
 // FatFs commonly updates one metadata sector. CMD24 includes busy/status
 // verification and avoids a pre-erase/multi-block transaction for one sector.
 for(UINT i=0;i<count;i++){bool ok=false;for(int retry=0;retry<4&&!ok;retry++)ok=card.writeBlock(sector+i,buf+512*i,true);if(!ok){chsd_fail=1;chweb_error_sector=sector+i;chweb_error_code=card.errorCode();chweb_error_operation=2;return RES_ERROR;}}return RES_OK;
}
extern "C" DRESULT disk_ioctl(BYTE drive,BYTE cmd,void *buf){if(drive||state)return RES_NOTRDY;streamEnd();switch(cmd){case CTRL_SYNC:return RES_OK;case GET_SECTOR_COUNT:*(LBA_t*)buf=card.cardSize();return RES_OK;case GET_BLOCK_SIZE:*(DWORD*)buf=1;return RES_OK;case GET_SECTOR_SIZE:*(WORD*)buf=512;return RES_OK;default:return RES_PARERR;}}

#endif
