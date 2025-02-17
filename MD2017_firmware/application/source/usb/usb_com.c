/*
 * Copyright (C) 2019      Kai Ludwig, DG4KLU
 * Copyright (C) 2019-2024 Roger Clark, VK3KYY / G4KYF
 *                         Daniel Caujolle-Bert, F1RMB
 *
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden. This includes, without limitation,
 *    incorporation in a commercial product or incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include <stdarg.h>
#include "functions/hotspot.h"
#include "functions/settings.h"
#include "user_interface/uiUtilities.h"
#include "user_interface/menuSystem.h"
#include "usb/usb_com.h"
#include "functions/ticks.h"
#include "interfaces/wdog.h"
#include "hardware/HR-C6000.h"
#include "functions/sound.h"
#include "hardware/SPI_Flash.h"
#include "user_interface/uiLocalisation.h"
#include "functions/rxPowerSaving.h"
#include "main.h"
#include <interfaces/clockManager.h>
#include "interfaces/settingsStorage.h"
#include "interfaces/gps.h"

// for debug mode
#include "hardware/radioHardwareInterface.h"
#include "hardware/AT1846S.h"



enum CPS_ACCESS_AREA
{
	CPS_ACCESS_FLASH = 1,
	CPS_ACCESS_EEPROM = 2,
	CPS_ACCESS_MCU_ROM = 5,
	CPS_ACCESS_DISPLAY_BUFFER = 6,
	CPS_ACCESS_WAV_BUFFER = 7,
	CPS_COMPRESS_AND_ACCESS_AMBE_BUFFER = 8,
	CPS_ACCESS_RADIO_INFO = 9,
#if ! defined(CPU_MK22FN512VLL12)
	CPS_ACCESS_FLASH_SECURITY_REGISTERS = 10,
#endif
    CPS_READ_SETTINGS = 11,
	CPS_WRITE_SETTINGS = 12,
    CPS_READ_FACTORY_CALIBRATIONS = 13,
	CPS_READ_BANDLIMITS = 14,
};


#if defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_DM1801) || defined(PLATFORM_DM1801A) || defined(PLATFORM_RD5R)
#define TASK_LOCK_WRITE()	  do { } while(0)
#define TASK_UNLOCK_WRITE()	  do { } while(0)
#elif defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
#define TASK_LOCK_WRITE()	  do { taskENTER_CRITICAL(); } while(0)
#define TASK_UNLOCK_WRITE()	  do { taskEXIT_CRITICAL(); } while(0)
#else
#error configure this platform about tasks locking
#endif


static void handleCPSRequest(void);


volatile int com_request = 0;
volatile uint8_t com_requestbuffer[COM_REQUESTBUFFER_SIZE];
volatile uint8_t usbComSendBuf[COM_BUFFER_SIZE];

static int sector = -1;
volatile int comRecvMMDVMIndexIn = 0;
volatile int comRecvMMDVMIndexOut = 0;
volatile int comRecvMMDVMFrameCount = 0;
static bool flashingDMRIDs = false;
static bool channelsRewritten = false;
static bool luczRewritten = false;
#if defined(HAS_GPS)
static gpsMode_t previousGPSState = GPS_NOT_DETECTED;
#endif

bool isCompressingAMBE = false;

volatile static bool hasToReply = false;
volatile static uint32_t replyLength = 0;

volatile bool usbIsResetting = false;

/*
static void hexDump2(uint8_t *ptr, int len,char *msg)
{
	char buf[8];
    msg[0] = 0;
    for (int i = 0; i < len; i++)
    {
		sprintf(buf,"%02x",*ptr);
		strcat(msg,buf);
	    ptr++;
    }
}*/

static bool addressInSegment(uint32_t address, uint32_t length, uint32_t segmentStart, uint32_t segmentSize)
{
	return ((address >= segmentStart) && ((address + length) <= (segmentStart + segmentSize)));
}

void tick_com_request(void)
{
	if (com_request != 1)
	{
		return;
	}

	switch (settingsUsbMode)
	{
		case USB_MODE_CPS:
			{
				TASK_LOCK_WRITE();
				handleCPSRequest();
				com_request = 0;
				if (hasToReply)
				{
					CDC_Transmit_FS((uint8_t *) usbComSendBuf, replyLength);
					hasToReply = false;
					replyLength = 0;
				}
				TASK_UNLOCK_WRITE();
			}
			break;

		case USB_MODE_HOTSPOT:
			// That will happen once when MMDVMHost send the first frame
			// out of the Hotspot mode.
			//if (com_request == 1)
			{
				com_request = 0;

				if ((nonVolatileSettings.hotspotType != HOTSPOT_TYPE_OFF) &&
						((comRecvMMDVMFrameCount >= 1) && (com_requestbuffer[1] == MMDVM_FRAME_START)) &&
						(uiDataGlobal.dmrDisabled == false)) // DMR (digital) is disabled.
				{

					if (menuSystemGetCurrentMenuNumber() != UI_HOTSPOT_MODE)
					{
						menuSystemPushNewMenu(UI_HOTSPOT_MODE);
					}
				}
			}
			break;


	}
}

static void cpsHandleReadCommand(void)
{
	uint32_t address = (com_requestbuffer[2] << 24) + (com_requestbuffer[3] << 16) + (com_requestbuffer[4] << 8) + (com_requestbuffer[5] << 0);
	uint32_t length = (com_requestbuffer[6] << 8) + (com_requestbuffer[7] << 0);
	bool result = false;

	if (length > (COM_REQUESTBUFFER_SIZE - 3))
	{
		length = (COM_REQUESTBUFFER_SIZE - 3);
	}

	switch(com_requestbuffer[1])
	{
		case CPS_ACCESS_FLASH:
		{
			// Calibration register, returns local copy
			if (addressInSegment(address, length, 0x10000, 0x200))
			{
				uint8_t *p = calibrationGetLocalDataPointer();
				memcpy((uint8_t *)&usbComSendBuf[3], (p + (address - 0x10000)), length);
				result = true;
			}
			else
			{
				TASK_UNLOCK_WRITE();
				result = SPI_Flash_read(address, (uint8_t *)&usbComSendBuf[3], length);
				uint32_t end = address + length - 1;
				const uint32_t VFOs_END = CODEPLUG_ADDR_VFO_A_CHANNEL + (sizeof(struct_codeplugChannel_t) * 2);

				// if CPS is writing the second part of the EEPROM (emulated in Flash) then the VFO's are being updated.
				if ((address <= CODEPLUG_ADDR_VFO_A_CHANNEL) && (end  >= VFOs_END))
				{
					uint32_t offset = CODEPLUG_ADDR_VFO_A_CHANNEL - address;

					struct_codeplugChannel_t * destPtr = (struct_codeplugChannel_t *)&usbComSendBuf[3 + offset];

					memcpy((uint8_t *)destPtr, (uint8_t *)&settingsVFOChannel[0], CODEPLUG_CHANNEL_DATA_STRUCT_SIZE);
					codeplugConvertChannelInternalToCodeplug(destPtr, destPtr);

					destPtr = (struct_codeplugChannel_t *)&usbComSendBuf[3 + offset + CODEPLUG_CHANNEL_DATA_STRUCT_SIZE];

					memcpy((uint8_t *)destPtr, (uint8_t *)&settingsVFOChannel[1], CODEPLUG_CHANNEL_DATA_STRUCT_SIZE);
					codeplugConvertChannelInternalToCodeplug(destPtr, destPtr);
				}

				TASK_LOCK_WRITE();
			}
			}
			break;

		case CPS_ACCESS_EEPROM:
			TASK_UNLOCK_WRITE();
			result = EEPROM_Read(address, (uint8_t *)&usbComSendBuf[3], length);
			TASK_LOCK_WRITE();
			break;

		case CPS_ACCESS_MCU_ROM:
			// Base address of MCU ROM on the STM32 is 0x8000000 not 0x00000000
			memcpy((uint8_t *)&usbComSendBuf[3], (uint8_t *)address + 0x8000000, length);
			result = true;
			break;

		case CPS_ACCESS_DISPLAY_BUFFER:
			rxPowerSavingSetState(ECOPHASE_POWERSAVE_INACTIVE); // Avoiding going into a state that USB doesn't work.
			memcpy((uint8_t *)&usbComSendBuf[3], (uint8_t *)displayGetPrimaryScreenBuffer() + address, length);
			result = true;
			break;

		case CPS_ACCESS_WAV_BUFFER:
			memcpy((uint8_t *)&usbComSendBuf[3], (uint8_t *)&audioAndHotspotDataBuffer.rawBuffer[address], length);
			result = true;
			break;

		case CPS_COMPRESS_AND_ACCESS_AMBE_BUFFER:// read from ambe audio buffer
			{
				uint8_t ambeBuf[32];// ambe data is up to 27 bytes long, but the normal transfer length for the CPS is 32, so make the buffer big enough for that transfer size
				memset(ambeBuf, 0, 32);// Clear the ambe output buffer
				codecEncode((uint8_t *)ambeBuf, 3);
				memcpy((uint8_t *)&usbComSendBuf[3], ambeBuf, length);// The ambe data is only 27 bytes long but the normal CPS request size is 32
				memset((uint8_t *)&audioAndHotspotDataBuffer.rawBuffer[0], 0x00, 960);// clear the input wave buffer, in case the next transfer is not a complete AMBE frame. 960 bytes compresses to 27 bytes of AMBE
				result = true;
			}
			break;

		case CPS_ACCESS_RADIO_INFO:
			{
				struct __attribute__((__packed__))
				{
					uint32_t structVersion;
					uint32_t radioType;
					char identifier[16];
					char buildDateTime[16];
					uint32_t settingsIdentifier;
					uint16_t features;
				} radioInfo;

				radioInfo.structVersion = 0x04;
#if defined(PLATFORM_GD77)
				radioInfo.radioType = 0;
#elif defined(PLATFORM_GD77S)
				radioInfo.radioType = 1;
#elif defined(PLATFORM_DM1801)
				radioInfo.radioType = 2;
#elif defined(PLATFORM_RD5R)
				radioInfo.radioType = 3;
#elif defined(PLATFORM_DM1801A)
				radioInfo.radioType = 4;
#elif defined(PLATFORM_MD9600)
				radioInfo.radioType = 5;
#elif defined(PLATFORM_MDUV380)
				radioInfo.radioType = 6;
#elif defined(PLATFORM_MD380)
				radioInfo.radioType = 7;
#elif defined(PLATFORM_RT84_DM1701) // because of BGR565 / RGB656 colour formats
				radioInfo.radioType = ((DISPLAYLCD_TYPE_IS_RGB(displayLCD_Type) != 0) ? 10 : 8);
#elif defined(PLATFORM_MD2017)
				radioInfo.radioType = 9;
#endif
				// 10 is reserved for DM1701 with RGB panel

				snprintf(radioInfo.identifier, sizeof(radioInfo.identifier), "%s", "RUSSIAN");
				snprintf(radioInfo.buildDateTime, sizeof(radioInfo.buildDateTime), "%04d%02d%02d", BUILD_YEAR, BUILD_MONTH, BUILD_DAY);
				radioInfo.settingsIdentifier = STORAGE_MAGIC_NUMBER;
				// Features bitfield (16 bits)
				radioInfo.features = (settingsIsOptionBitSet(BIT_INVERSE_VIDEO) ? 1 : 0);
				radioInfo.features |= (((dmrIDDatabaseMemoryLocation2 == VOICE_PROMPTS_FLASH_HEADER_ADDRESS) ? 1 : 0) << 1);
				radioInfo.features |= ((voicePromptDataIsLoaded ? 1 : 0) << 2);
                                radioInfo.features |= 0x08; //флаг "читаемые настройки" для CPS
				length = sizeof(radioInfo);
				memcpy((uint8_t *)&usbComSendBuf[3], &radioInfo, length);
				result = true;
			}
			break;

#if ! defined(CPU_MK22FN512VLL12)
		case CPS_ACCESS_FLASH_SECURITY_REGISTERS:
			TASK_UNLOCK_WRITE();
			result = SPI_Flash_readSecurityRegisters(address, (uint8_t *)&usbComSendBuf[3], length);
			TASK_LOCK_WRITE();
			break;
#endif
		case CPS_READ_SETTINGS:
			length = sizeof(settingsAlignedStruct_t);
			settingsAlignedStruct_t settingsBuffer;
			settingsBuffer.magicNumber = nonVolatileSettings.magicNumber;
			settingsBuffer.timezone = nonVolatileSettings.timezone;
			settingsBuffer.beepOptions = nonVolatileSettings.beepOptions;
			settingsBuffer.bitfieldOptions = nonVolatileSettings.bitfieldOptions;
			settingsBuffer.aprsBeaconingSettingsPart1 = nonVolatileSettings.aprsBeaconingSettingsPart1[0];
			settingsBuffer.aprsBeaconingSettingsPart2 = nonVolatileSettings.aprsBeaconingSettingsPart1[1];
			settingsBuffer.aprsBeaconingSettingsPart3 = nonVolatileSettings.aprsBeaconingSettingsPart2;
			settingsBuffer.txPowerLevel = nonVolatileSettings.txPowerLevel;
			settingsBuffer.txTimeoutBeepX5Secs = nonVolatileSettings.txTimeoutBeepX5Secs;
			settingsBuffer.beepVolumeDivider = nonVolatileSettings.beepVolumeDivider;
			settingsBuffer.micGainDMR = nonVolatileSettings.micGainDMR;
			settingsBuffer.micGainFM = nonVolatileSettings.micGainFM;
			settingsBuffer.backlightMode = nonVolatileSettings.backlightMode;
			settingsBuffer.backLightTimeout = nonVolatileSettings.backLightTimeout;
			settingsBuffer.displayContrast = nonVolatileSettings.displayContrast;
			settingsBuffer.displayBacklightPercentageDay = nonVolatileSettings.displayBacklightPercentage[0];
			settingsBuffer.displayBacklightPercentageNight = nonVolatileSettings.displayBacklightPercentage[1];
			settingsBuffer.displayBacklightPercentageOff = nonVolatileSettings.displayBacklightPercentageOff;
			settingsBuffer.extendedInfosOnScreen = nonVolatileSettings.extendedInfosOnScreen;
			settingsBuffer.scanModePause = nonVolatileSettings.scanModePause;
			settingsBuffer.scanDelay = nonVolatileSettings.scanDelay;
			settingsBuffer.DMR_RxAGC = nonVolatileSettings.DMR_RxAGC;
			settingsBuffer.hotspotType = nonVolatileSettings.hotspotType;
			settingsBuffer.scanStepTime = nonVolatileSettings.scanStepTime;
			settingsBuffer.dmrCaptureTimeout = nonVolatileSettings.dmrCaptureTimeout;
			settingsBuffer.privateCalls = nonVolatileSettings.privateCalls;
			settingsBuffer.contactDisplayPriority = nonVolatileSettings.contactDisplayPriority;
			settingsBuffer.splitContact = nonVolatileSettings.splitContact;
			settingsBuffer.voxThreshold = nonVolatileSettings.voxThreshold;
			settingsBuffer.voxTailUnits = nonVolatileSettings.voxTailUnits;
			settingsBuffer.audioPromptMode = nonVolatileSettings.audioPromptMode;
			settingsBuffer.batteryCalibration = nonVolatileSettings.batteryCalibration;
			settingsBuffer.squelchDefaultVHF = nonVolatileSettings.squelchDefaults[0];
			settingsBuffer.squelchDefaultUHF = nonVolatileSettings.squelchDefaults[1];
			settingsBuffer.placeHolder = 0;
			settingsBuffer.ecoLevel = nonVolatileSettings.ecoLevel;
			settingsBuffer.apo = nonVolatileSettings.apo;
			settingsBuffer.keypadTimerLong = nonVolatileSettings.keypadTimerLong;
			settingsBuffer.keypadTimerRepeat = nonVolatileSettings.keypadTimerRepeat;
			settingsBuffer.autolockTimer = nonVolatileSettings.autolockTimer;
			settingsBuffer.buttonP3 = nonVolatileSettings.buttonSK1;
			settingsBuffer.buttonP3Long = nonVolatileSettings.buttonSK1Long;
			settingsBuffer.scanPriority = nonVolatileSettings.scanPriority;
			settingsBuffer.txFreqLimited = nonVolatileSettings.txFreqLimited;
			memcpy((uint8_t *)&usbComSendBuf[3], (uint8_t *)&settingsBuffer, length);
			result = true;
            break;
		case CPS_READ_FACTORY_CALIBRATIONS:
			length = 0x200;
			TASK_UNLOCK_WRITE();
			result = SPI_Flash_readSecurityRegisters(0, (uint8_t *)&usbComSendBuf[3], length);
			TASK_LOCK_WRITE();
			break;
		case CPS_READ_BANDLIMITS:
			struct __attribute__((__packed__))
			{
				uint32_t VHFLowCal;
				uint32_t VHFLow;
				uint32_t VHFHigh;
				uint32_t UHFLowCal;
				uint32_t UHFLow;
				uint32_t UHFHigh;
			} radioBands;
			radioBands.VHFLowCal = RADIO_HARDWARE_FREQUENCY_BANDS[0].calTableMinFreq;
			radioBands.VHFLow = RADIO_HARDWARE_FREQUENCY_BANDS[0].minFreq;
			radioBands.VHFHigh = RADIO_HARDWARE_FREQUENCY_BANDS[0].maxFreq;
			radioBands.UHFLowCal = RADIO_HARDWARE_FREQUENCY_BANDS[1].calTableMinFreq;
			radioBands.UHFLow = RADIO_HARDWARE_FREQUENCY_BANDS[1].minFreq;
			radioBands.UHFHigh = RADIO_HARDWARE_FREQUENCY_BANDS[1].maxFreq;

			length = sizeof(radioBands);
			memcpy((uint8_t *)&usbComSendBuf[3], &radioBands, length);
			result = true;
			break;

	}

	hasToReply = true;
	if (result)
	{
		usbComSendBuf[0] = com_requestbuffer[0];
		usbComSendBuf[1] = (length >> 8) & 0xFF;
		usbComSendBuf[2] = (length >> 0) & 0xFF;
		replyLength = length + 3;
	}
	else
	{
		usbComSendBuf[0] = '-';
		replyLength = 1;
	}
}

static void cpsHandleWriteCommand(void)
{
	bool ok = false;

	switch(com_requestbuffer[1])
	{
		case 1: // Flash Prepare Sector
			if (sector == -1)
			{
				sector = (com_requestbuffer[2] << 16) + (com_requestbuffer[3] << 8) + (com_requestbuffer[4] << 0);

				if ((sector * 4096) == 0x10000) // Local calibration
				{
					ok = true;
					break;
				}
				else if ((sector * 4096) == 0x30000) // start address of DMRIDs DB
				{
					flashingDMRIDs = true;
				}

				TASK_UNLOCK_WRITE();
				ok = SPI_Flash_read(sector * 4096, SPI_Flash_sectorbuffer, 4096);
				TASK_LOCK_WRITE();
			}
			break;

		case 2: // Flash Send Data
			if (sector >= 0)
			{
				uint32_t address = (com_requestbuffer[2] << 24) + (com_requestbuffer[3] << 16) + (com_requestbuffer[4] << 8) + (com_requestbuffer[5] << 0);
				uint32_t length = (com_requestbuffer[6] << 8) + (com_requestbuffer[7] << 0);
				bool calibrationWriting = false;

#if defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
				if ((calibrationWriting == false) && addressInSegment(address, length, 0x10000, 0x200)) // Local calibration
				{
					calibrationWriting = true;
				}
				// Channel is going to be rewritten, will need to reset current zone/etc...
				else if ((channelsRewritten == false) && addressInSegment(address, length, CODEPLUG_ADDR_CHANNEL_HEADER_EEPROM, 0x1C10 /*128 first channels*/))
				{
					channelsRewritten = true;
				}
				else if ((luczRewritten == false) && addressInSegment(address, length, CODEPLUG_ADDR_LUCZ, (4 + CODEPLUG_ALL_ZONES_MAX + 1)))
				{
					luczRewritten = true;
				}
				else
#endif
#if !defined(PLATFORM_GD77S)
				// Temporary hack to automatically set Prompt to Level 1
				// A better solution will be added to the CPS and firmware at a later date.
				if ((address == VOICE_PROMPTS_FLASH_HEADER_ADDRESS) || (address == VOICE_PROMPTS_FLASH_OLD_HEADER_ADDRESS))
				{
					if (voicePromptsCheckMagicAndVersion((uint32_t *)&com_requestbuffer[8]))
					{
						nonVolatileSettings.audioPromptMode = AUDIO_PROMPT_MODE_VOICE_LEVEL_1;
					}
				}
#endif

				if (length > (COM_REQUESTBUFFER_SIZE - 8))
				{
					length = (COM_REQUESTBUFFER_SIZE - 8);
				}

#if defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
				// Temporary hack to prevent the QuickKeys getting overwritten by the codeplug
				const int QUICKKEYS_BLOCK_END = (CODEPLUG_ADDR_QUICKKEYS + (CODEPLUG_QUICKKEYS_SIZE * sizeof(uint16_t)) - 1);
				int end = (address + length) - 1;

				if (((address >= CODEPLUG_ADDR_QUICKKEYS) && (address <= QUICKKEYS_BLOCK_END))
						|| ((end >= CODEPLUG_ADDR_QUICKKEYS) && (end <= QUICKKEYS_BLOCK_END))
						|| ((address < CODEPLUG_ADDR_QUICKKEYS) && (end > QUICKKEYS_BLOCK_END)))
				{
					if (address < CODEPLUG_ADDR_QUICKKEYS)
					{
						for (int i = 0; i < (CODEPLUG_ADDR_QUICKKEYS - address); i++)
						{
							if (sector == (address + i) / 4096)
							{
								SPI_Flash_sectorbuffer[(address + i) % 4096] = com_requestbuffer[i + 8];
							}
						}

						if ((end > QUICKKEYS_BLOCK_END))
						{
							for (int i = 0; i <  (end - QUICKKEYS_BLOCK_END); i++)
							{
								if (sector == ((QUICKKEYS_BLOCK_END + 1) + i) / 4096)
								{
									SPI_Flash_sectorbuffer[((QUICKKEYS_BLOCK_END + 1) + i) % 4096] = com_requestbuffer[i + 8 + ((QUICKKEYS_BLOCK_END + 1) - address)];
								}
							}
						}

					}
					else
					{
						if ((address <= QUICKKEYS_BLOCK_END) && (end > QUICKKEYS_BLOCK_END))
						{
							for (int i = 0; i < (end - QUICKKEYS_BLOCK_END); i++)
							{
								if (sector == ((QUICKKEYS_BLOCK_END + 1) + i) / 4096)
								{
									SPI_Flash_sectorbuffer[((QUICKKEYS_BLOCK_END + 1) + i) % 4096] = com_requestbuffer[i + 8 + ((QUICKKEYS_BLOCK_END + 1) - address)];
								}
							}
						}
					}
				}
				else
#endif
				{
#if defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
					if (calibrationWriting)
					{
						uint8_t *p = calibrationGetLocalDataPointer();

						memcpy((p + (address - 0x10000)), (uint8_t *)&com_requestbuffer[8], length);
						sector = -2; // special case in Flash Write;
						ok = true;
						break;
					}
#endif

					for (int i = 0; i < length; i++)
					{
						if (sector == (address + i) / 4096)
						{
							SPI_Flash_sectorbuffer[(address + i) % 4096] = com_requestbuffer[i + 8];
						}
					}
				}

				ok = true;
			}
			break;

		case 3: // Flash Write
			if (sector >= 0)
			{
				TASK_UNLOCK_WRITE();
				ok = SPI_Flash_eraseSector(sector * 4096);
				TASK_LOCK_WRITE();
				if (ok)
				{
					for (int i = 0; i < 16; i++)
					{
						TASK_UNLOCK_WRITE();
						ok = SPI_Flash_writePage(sector * 4096 + i * 256, SPI_Flash_sectorbuffer + i * 256);
						TASK_LOCK_WRITE();
						if (!ok)
						{
							break;
						}
					}
				}
				sector = -1;
			}
			else if (sector == -2)
			{
				TASK_UNLOCK_WRITE();
				calibrationSaveLocal();
				TASK_LOCK_WRITE();
				ok = true;
				sector = -1;
			}
			break;

		case 4: // EEPROM
			{
#if defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_DM1801) || defined(PLATFORM_DM1801A) || defined(PLATFORM_RD5R)
				uint32_t address = (com_requestbuffer[2] << 24) + (com_requestbuffer[3] << 16) + (com_requestbuffer[4] << 8) + (com_requestbuffer[5] << 0);
				uint32_t length = (com_requestbuffer[6] << 8) + (com_requestbuffer[7] << 0);

				// Channel is going to be rewritten, will need to reset current zone/etc...
				if ((channelsRewritten == false) && addressInSegment(address, length, CODEPLUG_ADDR_CHANNEL_HEADER_EEPROM, 0x1C10 /*128 first channels*/))
				{
					channelsRewritten = true;
				}
				else if ((luczRewritten == false) && addressInSegment(address, length, CODEPLUG_ADDR_LUCZ, (4 + CODEPLUG_ALL_ZONES_MAX + 1)))
				{
					luczRewritten = true;
				}

				if (length > (COM_REQUESTBUFFER_SIZE - 8))
				{
					length = (COM_REQUESTBUFFER_SIZE - 8);
				}

				// Temporary hack to prevent the QuickKeys getting overwritten by the codeplug
				const int QUICKKEYS_BLOCK_END = (CODEPLUG_ADDR_QUICKKEYS + (CODEPLUG_QUICKKEYS_SIZE * sizeof(uint16_t)) - 1);
				int end = (address + length) - 1;

				if (((address >= CODEPLUG_ADDR_QUICKKEYS) && (address <= QUICKKEYS_BLOCK_END))
						|| ((end >= CODEPLUG_ADDR_QUICKKEYS) && (end <= QUICKKEYS_BLOCK_END))
						|| ((address < CODEPLUG_ADDR_QUICKKEYS) && (end > QUICKKEYS_BLOCK_END)))
				{
					if (address < CODEPLUG_ADDR_QUICKKEYS)
					{
						TASK_UNLOCK_WRITE();
						ok = EEPROM_Write(address, (uint8_t*)com_requestbuffer + 8, (CODEPLUG_ADDR_QUICKKEYS - address));
						TASK_LOCK_WRITE();

						if (ok && (end > QUICKKEYS_BLOCK_END))
						{
							TASK_UNLOCK_WRITE();
							ok = EEPROM_Write((QUICKKEYS_BLOCK_END + 1), (uint8_t *)com_requestbuffer + 8 + ((QUICKKEYS_BLOCK_END + 1) - address),  (end - QUICKKEYS_BLOCK_END));
							TASK_LOCK_WRITE();
						}

					}
					else
					{
						if ((address <= QUICKKEYS_BLOCK_END) && (end > QUICKKEYS_BLOCK_END))
						{
							TASK_UNLOCK_WRITE();
							ok = EEPROM_Write((QUICKKEYS_BLOCK_END + 1), (uint8_t *)com_requestbuffer + 8 + ((QUICKKEYS_BLOCK_END + 1) - address), (end - QUICKKEYS_BLOCK_END));
							TASK_LOCK_WRITE();
						}
						else
						{
							ok = true;
						}
					}
					//	SEGGER_RTT_printf(0, "0x%06x\t0x%06x\n",address,end);
				}
				else
				{
					TASK_UNLOCK_WRITE();
					ok = EEPROM_Write(address, (uint8_t *)com_requestbuffer + 8, length);
					TASK_LOCK_WRITE();
				}
#else
				ok = true;
#endif
			}
			break;

		case CPS_ACCESS_WAV_BUFFER:// write to raw audio buffer
			{
				uint32_t address = (com_requestbuffer[2] << 24) + (com_requestbuffer[3] << 16) + (com_requestbuffer[4] << 8) + (com_requestbuffer[5] << 0);
				uint32_t length = (com_requestbuffer[6] << 8) + (com_requestbuffer[7] << 0);

				wavbuffer_count = (address + length) / WAV_BUFFER_SIZE;
				memcpy((uint8_t *)&audioAndHotspotDataBuffer.rawBuffer[address], (uint8_t *)&com_requestbuffer[8], length);
				ok = true;
			}
			break;

		case CPS_ACCESS_RADIO_INFO:
			break;
		case CPS_WRITE_SETTINGS:
			settingsAlignedStruct_t settingsBuffer;
			memcpy((uint8_t *)&settingsBuffer, (uint8_t *)&usbComSendBuf[8], sizeof(settingsAlignedStruct_t));
			nonVolatileSettings.magicNumber = settingsBuffer.magicNumber;
			nonVolatileSettings.timezone = settingsBuffer.timezone;
			nonVolatileSettings.beepOptions = settingsBuffer.beepOptions;
			nonVolatileSettings.bitfieldOptions = settingsBuffer.bitfieldOptions;
			nonVolatileSettings.aprsBeaconingSettingsPart1[0] = settingsBuffer.aprsBeaconingSettingsPart1;
			nonVolatileSettings.aprsBeaconingSettingsPart1[1] = settingsBuffer.aprsBeaconingSettingsPart2;
			nonVolatileSettings.aprsBeaconingSettingsPart2 = settingsBuffer.aprsBeaconingSettingsPart3;
			nonVolatileSettings.txPowerLevel = settingsBuffer.txPowerLevel;
			nonVolatileSettings.txTimeoutBeepX5Secs = settingsBuffer.txTimeoutBeepX5Secs;
			nonVolatileSettings.beepVolumeDivider = settingsBuffer.beepVolumeDivider;
			nonVolatileSettings.micGainDMR = settingsBuffer.micGainDMR;
			nonVolatileSettings.micGainFM = settingsBuffer.micGainFM;
			nonVolatileSettings.backlightMode = settingsBuffer.backlightMode;
			nonVolatileSettings.backLightTimeout = settingsBuffer.backLightTimeout;
			nonVolatileSettings.displayContrast = settingsBuffer.displayContrast;
			nonVolatileSettings.displayBacklightPercentage[0] = settingsBuffer.displayBacklightPercentageDay;
			nonVolatileSettings.displayBacklightPercentage[1] = settingsBuffer.displayBacklightPercentageNight;
			nonVolatileSettings.displayBacklightPercentageOff = settingsBuffer.displayBacklightPercentageOff;
			nonVolatileSettings.extendedInfosOnScreen = settingsBuffer.extendedInfosOnScreen;
			nonVolatileSettings.scanModePause = settingsBuffer.scanModePause;
			nonVolatileSettings.scanDelay = settingsBuffer.scanDelay;
			nonVolatileSettings.DMR_RxAGC = settingsBuffer.DMR_RxAGC;
			nonVolatileSettings.hotspotType = settingsBuffer.hotspotType;
			nonVolatileSettings.scanStepTime = settingsBuffer.scanStepTime;
			nonVolatileSettings.dmrCaptureTimeout = settingsBuffer.dmrCaptureTimeout;
			nonVolatileSettings.privateCalls = settingsBuffer.privateCalls;
			nonVolatileSettings.contactDisplayPriority = settingsBuffer.contactDisplayPriority;
			nonVolatileSettings.splitContact = settingsBuffer.splitContact;
			nonVolatileSettings.voxThreshold = settingsBuffer.voxThreshold;
			nonVolatileSettings.voxTailUnits = settingsBuffer.voxTailUnits;
			nonVolatileSettings.audioPromptMode = settingsBuffer.audioPromptMode;
			nonVolatileSettings.batteryCalibration = settingsBuffer.batteryCalibration;
			nonVolatileSettings.squelchDefaults[0] = settingsBuffer.squelchDefaultVHF;
			nonVolatileSettings.squelchDefaults[1] = settingsBuffer.squelchDefaultUHF;
			nonVolatileSettings.placeHolder = settingsBuffer.placeHolder;
			nonVolatileSettings.ecoLevel = settingsBuffer.ecoLevel;
			nonVolatileSettings.apo = settingsBuffer.apo;
			nonVolatileSettings.keypadTimerLong = settingsBuffer.keypadTimerLong;
			nonVolatileSettings.keypadTimerRepeat = settingsBuffer.keypadTimerRepeat;
			nonVolatileSettings.autolockTimer = settingsBuffer.autolockTimer;
			nonVolatileSettings.buttonSK1 = settingsBuffer.buttonP3;
			nonVolatileSettings.buttonSK1Long = settingsBuffer.buttonP3Long;
			nonVolatileSettings.scanPriority = settingsBuffer.scanPriority;
			nonVolatileSettings.txFreqLimited = settingsBuffer.txFreqLimited;
			settingsSaveSettings(false);
			ok = true;
			break;
	}

	hasToReply = true;
	if (ok)
	{
		usbComSendBuf[0] = com_requestbuffer[0];
		usbComSendBuf[1] = com_requestbuffer[1];
		replyLength = 2;
	}
	else
	{
		sector = -1;
		usbComSendBuf[0] = '-';
		replyLength = 1;
	}
}
#ifdef USB_DEBUG_COMMANDS
static void cpsHandleDebugCommand(void)
{
	const pins[5] = { GPIO_PIN_11, GPIO_PIN_12, GPIO_PIN_13, GPIO_PIN_14, GPIO_PIN_15};
	int command = com_requestbuffer[1];
	switch(command)
	{
		case 'W':
			HAL_GPIO_WritePin(GPIOE, GPIO_PIN_11,com_requestbuffer[2]=='1'?GPIO_PIN_SET:GPIO_PIN_RESET);
			HAL_GPIO_WritePin(GPIOE, GPIO_PIN_12,com_requestbuffer[3]=='1'?GPIO_PIN_SET:GPIO_PIN_RESET);
			HAL_GPIO_WritePin(GPIOE, GPIO_PIN_13,com_requestbuffer[4]=='1'?GPIO_PIN_SET:GPIO_PIN_RESET);
			HAL_GPIO_WritePin(GPIOE, GPIO_PIN_14,com_requestbuffer[5]=='1'?GPIO_PIN_SET:GPIO_PIN_RESET);
			HAL_GPIO_WritePin(GPIOE, GPIO_PIN_15,com_requestbuffer[6]=='1'?GPIO_PIN_SET:GPIO_PIN_RESET);

			memcpy(usbComSendBuf," OK\n\0",5);
			hasToReply = true;
			replyLength = strlen(usbComSendBuf);
			break;
		case 'R':
			for(int i=0;i<5;i++)
			{
				usbComSendBuf[i]= (HAL_GPIO_ReadPin(GPIOE, pins[i])==GPIO_PIN_SET)?'1':'0';
			}
			usbComSendBuf[5] = '\n';
			usbComSendBuf[6] = 0;
			hasToReply = true;
			replyLength = strlen(usbComSendBuf);
			break;
	}
}
#endif

#if defined(HAS_GPS)
static void cpsStopGPSNMEA(void)
{
	if (nonVolatileSettings.gps >= GPS_MODE_ON_NMEA)
	{
#if defined(LOG_GPS_DATA)
		TASK_UNLOCK_WRITE();
		gpsLoggingStop();
		TASK_LOCK_WRITE();
#endif
		previousGPSState = nonVolatileSettings.gps;
		nonVolatileSettings.gps = GPS_MODE_ON;
	}
}
#endif

static void cpsHandleCommand(void)
{
	int command = com_requestbuffer[1];
	switch(command)
	{
		case 0:
#if defined(HAS_GPS)
			cpsStopGPSNMEA();
#endif

			if (menuSystemGetCurrentMenuNumber() == MENU_SATELLITE)
			{
				menuSystemPopAllAndDisplayRootMenu();
			}

			// Show CPS screen
			menuSystemPushNewMenu(UI_CPS);
			break;
		case 1:
			// Clear CPS screen
			uiCPSUpdate(CPS2UI_COMMAND_CLEARBUF, 0, 0, FONT_SIZE_1, TEXT_ALIGN_LEFT, 0, NULL);
			break;
		case 2:
			// Write a line of text to CPS screen
			uiCPSUpdate(CPS2UI_COMMAND_PRINT, com_requestbuffer[2], com_requestbuffer[3], (ucFont_t)com_requestbuffer[4], (ucTextAlign_t)com_requestbuffer[5], com_requestbuffer[6], (char *)&com_requestbuffer[7]);
			break;
		case 3:
			// Render CPS screen
			uiCPSUpdate(CPS2UI_COMMAND_RENDER_DISPLAY, 0, 0, FONT_SIZE_1, TEXT_ALIGN_LEFT, 0, NULL);
			break;
		case 4:
			// Turn on the display backlight
			uiCPSUpdate(CPS2UI_COMMAND_BACKLIGHT, 0, 0, FONT_SIZE_1, TEXT_ALIGN_LEFT, 0, NULL);
			break;
		case 5:
			// Close
			if (flashingDMRIDs)
			{
				dmrIDCacheInit();
				flashingDMRIDs = false;
			}
			isCompressingAMBE = false;
			rxPowerSavingSetLevel(nonVolatileSettings.ecoLevel);
			uiCPSUpdate(CPS2UI_COMMAND_END, 0, 0, FONT_SIZE_1, TEXT_ALIGN_LEFT, 0, NULL);
			break;
		case 6:
			{
				int subCommand = com_requestbuffer[2];
				uint32_t m = ticksGetMillis();

				// Do some other processing
				switch(subCommand)
				{
					case 0:
						// Channels has be rewritten, switch currentZone to All Channels
						if (channelsRewritten)
						{
							int16_t firstContact = 1;

							//
							// Give it a bit of time before reading the zone count as DM-1801 EEPROM looks slower
							// than GD-77 to write
							m = ticksGetMillis();
							while (true)
							{
								TASK_UNLOCK_WRITE();
								vTaskDelay(10 / portTICK_PERIOD_MS);
								TASK_LOCK_WRITE();

								if ((ticksGetMillis() - m) > 50)
								{
									break;
								}
							}

							TASK_UNLOCK_WRITE();
							codeplugZonesInitCache(); // Re-read zone cache, as we're using codeplugZonesGetCount() below.
							codeplugAllChannelsInitCache(); // Rebuild channels cache
							TASK_LOCK_WRITE();

							nonVolatileSettings.currentZone = (int16_t) (codeplugZonesGetCount() - 1); // Set to All Channels zone

							TASK_UNLOCK_WRITE();
							codeplugInitLastUsedChannelInZone(); // Re-read the LUCZ
							TASK_LOCK_WRITE();

							// Search for the first assigned contact
							int16_t luczAllChannels = codeplugGetLastUsedChannelInZone(-1);
							if (codeplugAllChannelsIndexIsInUse(luczAllChannels))
							{
								firstContact = luczAllChannels;
							}
							else
							{
								for (int16_t i = CODEPLUG_CHANNELS_MIN; i <= CODEPLUG_CHANNELS_MAX; i++)
								{
									if (codeplugAllChannelsIndexIsInUse(i))
									{
										firstContact = i;
										break;
									}

									// Call tick_watchdog() ??
								}
							}
#if defined(PLATFORM_RD5R)
							nonVolatileSettings.currentChannelIndexInAllZone = firstContact;
							nonVolatileSettings.currentChannelIndexInZone = 0;
#endif
							codeplugSetLastUsedChannelInZone(-1, firstContact);

							luczRewritten = false;
						}

#if defined(HAS_GPS)
						// restore GPS state if needed
						if (previousGPSState >= GPS_MODE_ON_NMEA)
						{
							nonVolatileSettings.gps = previousGPSState;
						}
#endif
						// save current settings and reboot
						m = ticksGetMillis();
						TASK_UNLOCK_WRITE();
						settingsSaveSettings(false);// Need to save these channels prior to reboot, as reboot does not save
						TASK_LOCK_WRITE();

						if (! luczRewritten)
						{
							TASK_UNLOCK_WRITE();
							codeplugSaveLastUsedChannelInZone();
							TASK_LOCK_WRITE();
						}

						// Give it a bit of time before pulling the plug as DM-1801 EEPROM looks slower
						// than GD-77 to write, then quickly power cycling triggers settings reset.
						while (true)
						{
							TASK_UNLOCK_WRITE();
							vTaskDelay(10 / portTICK_PERIOD_MS);
							TASK_LOCK_WRITE();

							if ((ticksGetMillis() - m) > 50)
							{
								break;
							}
						}

						addTimerCallback(NVIC_SystemReset, 500, MENU_ANY, false);
						break;
					case 1:
#if defined(HAS_GPS)
						if (previousGPSState >= GPS_MODE_ON_NMEA)
						{
							nonVolatileSettings.gps = previousGPSState;
						}
#endif
						addTimerCallback(NVIC_SystemReset, 500, MENU_ANY, false);
						break;
					case 2:
#if defined(HAS_GPS)
						if (previousGPSState >= GPS_MODE_ON_NMEA)
						{
							nonVolatileSettings.gps = previousGPSState;
						}
#endif
						// Save settings VFO's to codeplug
						TASK_UNLOCK_WRITE();
						settingsSaveSettings(true);
						TASK_LOCK_WRITE();

#if defined(HAS_GPS)
						if (previousGPSState >= GPS_MODE_ON_NMEA)
						{
							nonVolatileSettings.gps = GPS_MODE_ON;
						}
#endif
						break;
					case 3:
						// flash green LED
						uiCPSUpdate(CPS2UI_COMMAND_GREEN_LED, 0, 0, FONT_SIZE_1, TEXT_ALIGN_LEFT, 0, NULL);
						break;
					case 4:
						// flash red LED
						uiCPSUpdate(CPS2UI_COMMAND_RED_LED, 0, 0, FONT_SIZE_1, TEXT_ALIGN_LEFT, 0, NULL);
						break;
					case 5:
						rxPowerSavingSetLevel(0);
						isCompressingAMBE = true;
						codecInitInternalBuffers();
						break;
					case 6:
						soundInit();// Resets the sound buffers
						memset((uint8_t *)&audioAndHotspotDataBuffer.rawBuffer[0], 0, (6 * WAV_BUFFER_SIZE));// clear 1 dmr frame size of wav buffer memory
						break;
					case 7:
						memcpy(&uiDataGlobal.dateTimeSecs, (uint8_t *)&com_requestbuffer[3], sizeof(uint32_t));// update date with data from the CPS
#if ! defined(PLATFORM_GD77S)
						daytimeThemeChangeUpdate(true);
#endif
#if defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
						setRtc_custom(uiDataGlobal.dateTimeSecs);
#endif
						menuSatelliteScreenClearPredictions(true);
						break;
					// 8:
					// 9:
					case 10: // wait 10ms
						m = ticksGetMillis();
						while (true)
						{
							TASK_UNLOCK_WRITE();
							vTaskDelay(5 / portTICK_PERIOD_MS);
							TASK_LOCK_WRITE();

							if ((ticksGetMillis() - m) > 10)
							{
								break;
							}
						}
						break;
					default:
						break;
				}
			}
			break;
		case 7:
#if defined(HAS_GPS)
			if (previousGPSState >= GPS_MODE_ON_NMEA)
			{
				nonVolatileSettings.gps = previousGPSState;
				previousGPSState = GPS_NOT_DETECTED;
#if defined(LOG_GPS_DATA)
				TASK_UNLOCK_WRITE();
				gpsLoggingStart();
				TASK_LOCK_WRITE();
#endif
			}
#endif
			break;
		case 77: //сброс настроек
			settingsRestoreDefaultSettings();
			break;

		case 254: // PING
#if defined(HAS_GPS)
			cpsStopGPSNMEA();
#endif
			break;
		default:
			break;
	}
	// Send something generic back.
	// Probably need to send a response code in the future
	usbComSendBuf[0] = '-';
	hasToReply = true;
	replyLength = 1;
}

static const GPIO_TypeDef* GPIO_PORTS[5] =
{
		GPIOA,GPIOB,GPIOC,GPIOD,GPIOE
};

volatile char debugKey = 0 ;
volatile uint32_t debugbutton = 0;
volatile int keyPhase = 0;

void debugModeUpdateKeysAndButtons(keyboardCode_t *keys, int *keyEvent, uint32_t *buttons, int *buttonEvent)
{
	if (keyPhase != 0)
	{
		keyPhase++;
		keys->key = debugKey;
		if (keyPhase == 2)
		{
			keys->event = KEY_MOD_PRESS + KEY_MOD_DOWN;
		}
		else
		{
			keys->event = KEY_MOD_UP;
			keyPhase = 0;
		}

		switch(debugKey)
		{
			case 'M':
			case 'm':
				keys->key = 0x0D;

				break;
			case 'B':
			case 'b':
				keys->key = 0x1B;
				//keys->event = KEY_MOD_UP;
				break;
			case 'U':
			case 'u':
				keys->key = 0x09;

				break;
			case 'D':
			case 'd':
				keys->key = 0x0A;
				break;
			case 'L':
			case 'l':
				keys->key = 0x03;
				//keys->event = KEY_MOD_UP;
				break;
			case 'R':
			case 'r':
				keys->key = 0x04;
				//keys->event = KEY_MOD_UP;
				break;
			case '#':
			case '*':
				//keys->event = KEY_MOD_UP;
				break;
		}

		*keyEvent = 1;

		//HAL_GPIO_TogglePin(LED_GREEN_GPIO_Port,LED_GREEN_Pin);
		return;
	}

	if (debugbutton != 0)
	{
		*buttons = debugbutton;
		*buttonEvent = EVENT_BUTTON_CHANGE;
		debugbutton = 0;
	}
}

static void handleDebugRequest(void)
{
	switch(com_requestbuffer[0])
	{
		case 'D':// Debug
			sprintf((char *)usbComSendBuf, "Debug mode ON\n");
			CDC_Transmit_FS((uint8_t *) usbComSendBuf, strlen((char *)usbComSendBuf));
			break;
		case 'P':// individual pin control
		{
			char pin,portLetter,action[32];
			int num;

			if (sscanf((char *)com_requestbuffer, "%c%c%d %s", &pin, &portLetter, &num, &action) == 4)
			{
				sprintf((char *)usbComSendBuf, "%c%c%d %s > ", pin, portLetter, num, action);
				CDC_Transmit_FS((uint8_t *) usbComSendBuf, strlen((char *)usbComSendBuf));
				osDelay(5);// leave a little time for this to complete sending

				uint32_t portIndex = portLetter - 'A';
				if (portIndex > 4)
				{
					sprintf((char *)usbComSendBuf, "Invalid port letter\n");
					CDC_Transmit_FS((uint8_t *) usbComSendBuf, strlen((char *)usbComSendBuf));
				}
				else
				{
					GPIO_TypeDef* port = (GPIO_TypeDef*)GPIO_PORTS[portIndex];
					switch(action[0])
					{
						case 'I':
						case 'i':
						{
							GPIO_InitTypeDef GPIO_InitStruct = {0};

							GPIO_InitStruct.Pin = 1<<num;
							GPIO_InitStruct.Mode = GPIO_MODE_INPUT;//GPIO_MODE_INPUT;//GPIO_MODE_OUTPUT_PP;//;
							GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

							switch(action[1])
							{
								case 'U':
								case 'u':
									GPIO_InitStruct.Pull = GPIO_PULLUP;//GPIO_PULLDOWN;//GPIO_PULLUP;//GPIO_PULLDOWN; GPIO_NOPULL;
									break;
								case 'D':
								case 'd':
									GPIO_InitStruct.Pull = GPIO_PULLDOWN;//GPIO_PULLDOWN;//GPIO_PULLUP;//GPIO_PULLDOWN; GPIO_NOPULL;
									break;
								case 'N':
								case 'n':
								default:
									GPIO_InitStruct.Pull = GPIO_NOPULL;//GPIO_PULLDOWN;//GPIO_PULLUP;//GPIO_PULLDOWN; GPIO_NOPULL;
									break;

							}

							HAL_GPIO_Init(port, &GPIO_InitStruct);

							sprintf((char *)usbComSendBuf, "Input %c%d = %s\n", portLetter, num, HAL_GPIO_ReadPin(port, 1<<num)?"High\n":"Low\n");
						}
						break;
						case 'O':
						case 'o':
						{
							GPIO_InitTypeDef GPIO_InitStruct = {0};

							GPIO_InitStruct.Pin = 1<<num;
							GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;//GPIO_MODE_INPUT;//GPIO_MODE_OUTPUT_PP;//;
							GPIO_InitStruct.Pull = GPIO_NOPULL;//GPIO_PULLDOWN;//GPIO_PULLUP;//GPIO_PULLDOWN; GPIO_NOPULL;
							GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
							HAL_GPIO_Init(port, &GPIO_InitStruct);

							HAL_GPIO_WritePin(port,1<<num, GPIO_PIN_RESET);
							sprintf((char *)usbComSendBuf, "Output %c%d = Low\n",portLetter ,num);
						}
						break;

						case 'H':
						case 'h':
							HAL_GPIO_WritePin(port,1<<num, GPIO_PIN_SET);
							sprintf((char *)usbComSendBuf, "Write pin %c%d = High\n",portLetter ,num);
							break;
						case 'L':
						case 'l':
							HAL_GPIO_WritePin(port,1<<num, GPIO_PIN_RESET);
							sprintf((char *)usbComSendBuf, "Write pin %c%d = LOW\n",portLetter ,num);
							break;
						case 'T':
						case 't':
							HAL_GPIO_TogglePin(port,1<<num);
							sprintf((char *)usbComSendBuf, "Toggle pin %c%d = %s\n",portLetter ,num , HAL_GPIO_ReadPin(port,1<<num)?"High\n":"Low\n");
							break;
						case '?':
							sprintf((char *)usbComSendBuf, "Read %c%d = %s\n",portLetter ,num , HAL_GPIO_ReadPin(port,1<<num)?"High\n":"Low\n");
							break;

					}

					CDC_Transmit_FS((uint8_t *) usbComSendBuf, strlen((char *)usbComSendBuf));
				}
			}
		}
		break;
		case 'R':// set radio number
		case 'r':
		{
			char ignore;
			int32_t radioId;

			if ((sscanf((char *)com_requestbuffer, "%c %d", &ignore , &radioId) == 2) &&
					((radioId > RADIO_DEVICE_PRIMARY) && (radioId <= RADIO_DEVICE_MAX)))
			{
				radioSetTRxDevice(((RadioDevice_t)(radioId - 1)));
				USB_DEBUG_printf("Setting to radio #%d\n", radioId);
			}
		}
		break;
		case 'F':// frequency
		case 'f':
		{
			char ignore;
			uint32_t freq;
			uint32_t radioId;

			if ((sscanf((char *)com_requestbuffer,"%c %u %d", &ignore, &radioId, &freq) == 3) &&
					((radioId > RADIO_DEVICE_PRIMARY) && (radioId <= RADIO_DEVICE_MAX)))
			{
				RadioDevice_t previousDevice = radioSetTRxDevice(((RadioDevice_t)(radioId - 1)));
				radioSetFrequency(freq * 100, false);
				radioSetTRxDevice(previousDevice);

				USB_DEBUG_printf("Setting radio #%u -> Freq %dkHz\n", radioId, freq);
			}
		}
		break;
		case 'K':// keypad functions
		case 'k':
		{
			char ignore;

			if (sscanf((char *)com_requestbuffer, "%c %c", &ignore , &debugKey) == 2)
			{
				keyPhase = 1;
			}
		}
		break;
		case 'H':
		case 'h':
			settingsUsbModeDebugHaltRenderingKeypad = !settingsUsbModeDebugHaltRenderingKeypad;
			if (settingsUsbModeDebugHaltRenderingKeypad)
			{
				GPIO_InitTypeDef GPIO_InitStruct = {0};

				GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
				GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
				GPIO_InitStruct.Pull = GPIO_PULLDOWN;

				GPIO_InitStruct.Pin = LCD_D0_Pin | LCD_D1_Pin | LCD_D2_Pin | LCD_D3_Pin;
				HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

				GPIO_InitStruct.Pin = LCD_D4_Pin | LCD_D5_Pin | LCD_D6_Pin | LCD_D7_Pin;
				HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);
			}
			sprintf((char *)usbComSendBuf, settingsUsbModeDebugHaltRenderingKeypad?"Halt\n":"Resume\n");
			CDC_Transmit_FS((uint8_t *) usbComSendBuf, strlen((char *)usbComSendBuf));
			break;
		case 'A':
		case 'a':
		{
			int reg, value16;
			uint8_t val1, val2;
			uint32_t radioId;
			RadioDevice_t lastDeviceId = currentRadioDeviceId;

			switch(com_requestbuffer[1])
			{
				case 'R':
				case 'r':
					if (sscanf((char *)&com_requestbuffer[3],"%u %X", &radioId, &reg) == 2)
					{
						if (radioId < RADIO_DEVICE_MAX)
						{
							currentRadioDeviceId = (RadioDevice_t)radioId;
							radioReadReg2byte(reg, &val1 , &val2);
							sprintf((char *)usbComSendBuf, "AT1846S:%u Read 0x%02X, 0x%02X, 0x%02X\n", radioId, reg, val1, val2);
							CDC_Transmit_FS((uint8_t *) usbComSendBuf, strlen((char *)usbComSendBuf));
						}
					}
					break;
				case 'W':
				case 'w':
					if (sscanf((char *)&com_requestbuffer[3],"%u %X %X", &radioId, &reg, &value16) == 3)
					{
						if (radioId < RADIO_DEVICE_MAX)
						{
							currentRadioDeviceId = (RadioDevice_t)radioId;
							radioWriteReg2byte(reg, (value16 & 0xFF00) >> 8 , (value16 & 0xFF));
							sprintf((char *)usbComSendBuf, "AT1846S:%u Write 0x%02X, 0x%02X, 0x%02X\n",radioId, reg, (value16 & 0xFF00) >> 8 , (value16 & 0xFF));
							CDC_Transmit_FS((uint8_t *) usbComSendBuf, strlen((char *)usbComSendBuf));
						}
					}
					break;
			}
			currentRadioDeviceId = lastDeviceId;
		}
		break;
		case 'N':
		case 'n':
		{
			uint32_t radioId;

			if ((sscanf((char *)&com_requestbuffer[2], "%u", &radioId) == 1)
					&& (radioId < RADIO_DEVICE_MAX))
			{
				sprintf((char *)usbComSendBuf, "#:%u RSSI:%d S/N:%d\n", radioId, radioDevices[radioId].trxRxSignal, radioDevices[radioId].trxRxNoise);
				CDC_Transmit_FS((uint8_t *) usbComSendBuf, strlen((char *)usbComSendBuf));
			}
		}
		break;
	}
}

static void handleCPSRequest(void)
{
	//Handle read
	switch(com_requestbuffer[0])
	{
		case 'R':
			cpsHandleReadCommand();
			break;
		case 'X'://W
			cpsHandleWriteCommand();
			break;
		case 'W': // Fake write
			{
				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = com_requestbuffer[1];
				hasToReply = true;
				replyLength = 2;
			}
			break;
		case 'C':
			cpsHandleCommand();
			break;
#ifdef USB_DEBUG_COMMANDS
		case 'D':
			cpsHandleDebugCommand();
			break;
#endif

#if 0 // has to be rewritten
		case '$':// NMEA
			{
				gpsPower(false);

				nonVolatileSettings.gps = GPS_MODE_ON;
				//HAL_GPIO_TogglePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin);

				int bufPos = 0;
				char rxchar;
				do
				{
					rxchar = com_requestbuffer[bufPos];
					if (rxchar != 13)
					{
						gpsLine[bufPos] = rxchar;
					}
					bufPos++;
				} while((rxchar != 13) && (bufPos < COM_REQUESTBUFFER_SIZE)) ;

				gpsLineLength = bufPos;
				gpsLineReady = true;
			}
			break;
#endif
		default:
			usbComSendBuf[0] = '-';
			hasToReply = true;
			replyLength = 1;
			break;
	}
}
#if 0
__attribute__((section(".ccmram"))) volatile uint8_t com_buffer[COM_BUFFER_SIZE];
int com_buffer_write_idx = 0;
int com_buffer_read_idx = 0;
volatile int com_buffer_cnt = 0;

void send_packet(uint8_t val_0x82, uint8_t val_0x86, int ram)
{
	taskENTER_CRITICAL();
	if ((HR_C6000_datalogging) && ((com_buffer_cnt+8+(ram+1))<=COM_BUFFER_SIZE))
	{
		add_to_commbuffer((com_buffer_cnt >> 8) & 0xff);
		add_to_commbuffer((com_buffer_cnt >> 0) & 0xff);
		add_to_commbuffer(val_0x82);
		add_to_commbuffer(val_0x86);
		add_to_commbuffer(tmp_val_0x51);
		add_to_commbuffer(tmp_val_0x52);
		add_to_commbuffer(tmp_val_0x57);
		add_to_commbuffer(tmp_val_0x5f);
		for (int i=0;i<=ram;i++)
		{
			add_to_commbuffer(DMR_frame_buffer[i]);
		}
	}
	taskEXIT_CRITICAL();
}

uint8_t tmp_ram1[256];
uint8_t tmp_ram2[256];

void send_packet_big(uint8_t val_0x82, uint8_t val_0x86, int ram1, int ram2)
{
	taskENTER_CRITICAL();
	if ((HR_C6000_datalogging) && ((com_buffer_cnt+8+(ram1+1)+(ram2+1))<=COM_BUFFER_SIZE))
	{
		add_to_commbuffer((com_buffer_cnt >> 8) & 0xff);
		add_to_commbuffer((com_buffer_cnt >> 0) & 0xff);
		add_to_commbuffer(val_0x82);
		add_to_commbuffer(val_0x86);
		add_to_commbuffer(tmp_val_0x51);
		add_to_commbuffer(tmp_val_0x52);
		add_to_commbuffer(tmp_val_0x57);
		add_to_commbuffer(tmp_val_0x5f);
		for (int i=0;i<=ram1;i++)
		{
			add_to_commbuffer(tmp_ram1[i]);
		}
		for (int i=0;i<=ram2;i++)
		{
			add_to_commbuffer(tmp_ram2[i]);
		}
	}
	taskEXIT_CRITICAL();
}

void add_to_commbuffer(uint8_t value)
{
	com_buffer[com_buffer_write_idx]=value;
	com_buffer_cnt++;
	com_buffer_write_idx++;
	if (com_buffer_write_idx==COM_BUFFER_SIZE)
	{
		com_buffer_write_idx=0;
	}
}
#endif

void USB_DEBUG_PRINT(char *str)
{
	strncpy((char *)usbComSendBuf, str, COM_BUFFER_SIZE);
	usbComSendBuf[COM_BUFFER_SIZE - 1] = 0; // SAFETY: strncpy won't NULL terminate the buffer if length is exceeding.

	CDC_Transmit_FS((uint8_t *)usbComSendBuf, strlen((char *)usbComSendBuf));
}

void USB_DEBUG_printf(const char *format, ...)
{
	char buf[COM_BUFFER_SIZE];
	va_list params;

	va_start(params, format);
	vsnprintf(buf, (sizeof(buf) - 2), format, params);
	//strcat(buf, "\n");
	va_end(params);
	USB_DEBUG_PRINT(buf);
}


extern USBD_HandleTypeDef hUsbDeviceFS;

bool USB_DeviceIsResetting(void)
{
	if ((hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED) && (clockManagerGetRunMode() != CLOCK_MANAGER_SPEED_RUN))
	{
		clockManagerSetRunMode(kAPP_PowerModeRun, CLOCK_MANAGER_SPEED_RUN);
	}

	return usbIsResetting;
}
