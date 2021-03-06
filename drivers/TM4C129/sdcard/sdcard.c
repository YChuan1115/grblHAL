/*
  sdcard.c - An embedded CNC Controller with rs274/ngc (g-code) support

  Driver code for Texas Instruments Tiva C (TM4C123GH6PM) ARM processor

  Part of Grbl

  Copyright (c) 2018 Terje Io

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>

#include "base/driver.h"
#include "sdcard.h"
#include "GRBL/grbl.h"

// https://e2e.ti.com/support/tools/ccs/f/81/t/428524?Linking-error-unresolved-symbols-rom-h-pinout-c-

/* uses fatfs - http://www.elm-chan.org/fsw/ff/00index_e.html */

#define MAX_PATHLEN 128
#define LCAPS(c) ((c >= 'A' && c <= 'Z') ? c | 0x20 : c)

#ifdef __MSP432E401Y__
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include <ti/drivers/SDFatFS.h>
#include <ti/boards/MSP_EXP432E401Y/Board.h>
#else
#include "fatfs/src/ff.h"
#include "fatfs/src/diskio.h"
#endif

#if FF_USE_LFN
//#define _USE_LFN FF_USE_LFN
#define _MAX_LFN FF_MAX_LFN
#endif

char const *const filetypes[] = {
    "nc",
    "gcode",
    "txt",
    "text",
    "tap",
    "ngc",
    ""
};

static FIL cncfile;

typedef enum {
    Filename_Filtered = 0,
    Filename_Valid,
    Filename_Invalid
} file_status_t;

typedef struct
{
    FATFS *fs;
    FIL *handle;
    size_t size;
    size_t pos;
    uint32_t line;
    uint8_t eol;
} file_t;

static file_t file = {
    .fs = NULL,
    .handle = NULL,
    .size = 0,
    .pos = 0
};

static io_stream_t active_stream;
//static report_t active_reports;

#ifdef __MSP432E401Y__
/*---------------------------------------------------------*/
/* User Provided Timer Function for FatFs module           */
/*---------------------------------------------------------*/
/* This is a real time clock service to be called from     */
/* FatFs module. Any valid time must be returned even if   */
/* the system does not support a real time clock.          */

DWORD fatfs_getFatTime (void)
{

    return    ((2007UL-1980) << 25)    // Year = 2007
            | (6UL << 21)            // Month = June
            | (5UL << 16)            // Day = 5
            | (11U << 11)            // Hour = 11
            | (38U << 5)            // Min = 38
            | (0U >> 1)                // Sec = 0
            ;

}
#endif

static file_status_t allowed (char *filename, bool is_file)
{
    uint_fast8_t idx = 0;
    char filetype[8], *ftptr;
    file_status_t status = is_file ? Filename_Filtered : Filename_Valid;

    if(is_file && (ftptr = strrchr(filename, '.'))) {
        ftptr++;
        if(strlen(ftptr) > sizeof(filetype) - 1)
            return status;
        while(ftptr[idx]) {
            filetype[idx] = LCAPS(ftptr[idx]);
            idx++;
        }
        filetype[idx] = '\0';
        idx = 0;
        while(status == Filename_Filtered && filetypes[idx][0]) {
            if(!strcmp(filetype, filetypes[idx]))
                status = Filename_Valid;
            idx++;;
        }
    }

    if(status == Filename_Valid) {
        if(strchr(filename, ' ') ||
            strchr(filename, CMD_STATUS_REPORT) ||
             strchr(filename, CMD_CYCLE_START) ||
              strchr(filename, CMD_FEED_HOLD))
            status = Filename_Invalid;
    //TODO: check for top bit set characters
    }

    return status;
}

static inline char *get_name (FILINFO *file)
{
#if _USE_LFN
    return *file->lfname == '\0' ? file->fname : file->lfname;
#else
    return file->fname;
#endif
}

static FRESULT scan_dir (char *path, uint_fast8_t depth, char *buf)
{
    DIR dir;
    FILINFO fno;
    FRESULT res;
    file_status_t status;
#if _USE_LFN
    static TCHAR lfn[_MAX_LFN + 1];   /* Buffer to store the LFN */
    fno.lfname = lfn;
    fno.lfsize = sizeof(lfn);
#endif

   if((res = f_opendir(&dir, path)) != FR_OK)
        return res;

    while(true) {

        if((res = f_readdir(&dir, &fno)) != FR_OK || fno.fname[0] == '\0')
            break;

        if(fno.fattrib & AM_DIR) { // It is a directory
            if((status = allowed(get_name(&fno), false)) == Filename_Valid) {
                if(strlen(path) + strlen(get_name(&fno)) > (MAX_PATHLEN - 1))
                    break;
                sprintf(&path[strlen(path)], "/%s", get_name(&fno));
                if(!(--depth && (res = scan_dir(path, depth, buf)) == FR_OK))
                    break;
            }
        } else if((status = allowed(get_name(&fno), true)) != Filename_Filtered) { // It is a file
            sprintf(buf, "[FILE:%s/%s|SIZE:%d%s]\r\n", path, get_name(&fno), fno.fsize, status == Filename_Invalid ? "|UNUSABLE" : "");
            hal.stream.write(buf);
        }
    }

#ifdef __MSP432E401Y__
    f_closedir(&dir);
#endif

    return res;
}

static void file_close (void)
{
    if(file.handle) {
        f_close(file.handle);
        file.handle = NULL;
    }
}

static bool file_open (char *filename)
{
    if(file.handle)
        file_close();

    if(f_open(&cncfile, filename, FA_READ) == FR_OK) {
        file.handle = &cncfile;
        file.size = f_size(file.handle);
        file.pos = 0;
        file.line = 0;
        file.eol = false;
    }

    return file.handle != NULL;
}

static int16_t file_read (void)
{
    signed char c;
    UINT count;

    if(f_read(file.handle, &c, 1, &count) == FR_OK && count == 1)
        file.pos = f_tell(file.handle);
    else
        c = -1;

    if(c == '\r' || c == '\n')
        file.eol++;
    else
        file.eol = 0;

    return (int16_t)c;
}

static bool sdcard_mount (void)
{
#ifdef __MSP432E401Y__
    return SDFatFS_open(Board_SDFatFS0, 0) != NULL;
#else
    if(file.fs == NULL)
        file.fs = malloc(sizeof(FATFS));

    if(file.fs && f_mount(0, file.fs) != FR_OK) {

        free(file.fs);
        file.fs = NULL;
    }

    return file.fs != NULL;
#endif
}

static status_code_t sdcard_ls (char *buf)
{
    char path[MAX_PATHLEN] = ""; // NB! also used as work area when recursing directories

    return scan_dir(path, 10, buf) == FR_OK ? Status_OK : Status_SDFailedOpenDir;
}

static void sdcard_end_job (void)
{
    file_close();
    memcpy(&hal.stream, &active_stream, sizeof(io_stream_t));   // Restore stream pointers
    hal.stream.reset_read_buffer();                             // and flush input buffer
    hal.driver_rt_report = NULL;
    hal.report.status_message = report_status_message;
    hal.report.feedback_message = report_feedback_message;
    sys.block_input_stream = false;
}

void trap_status_report (status_code_t status_code)
{
    if(status_code != Status_OK) { // TODO: all errors should terminate job?
        char buf[50]; // TODO: check if extended error reports are permissible
        sprintf(buf, "error:%d in SD file at line %d\r\n", (uint8_t)status_code, file.line);
        hal.stream.write(buf);

        sdcard_end_job();
    }
    // else
    //     file.line++; ??
}

void trap_feedback_message (message_code_t message_code)
{
    report_feedback_message(message_code);

    // TODO: rewind and wait for cycle start in order to comply with NIST?
    if(message_code == Message_ProgramEnd)
        sdcard_end_job();
}

static int16_t sdcard_read (void)
{
    int16_t c = -1;

    if(file.eol == 1)
        file.line++;

    if(file.handle) {

        if(sys.state == STATE_IDLE || (sys.state & (STATE_CYCLE|STATE_HOLD)))
            c = file_read();

        if(c == -1) { // EOF or error reading or grbl problem
            file_close();
            if(file.eol == 0) // Return newline if line was incorrectly terminated
                c = '\n';
        }

    } else if(sys.state == STATE_IDLE) // TODO: end on ok count match line count?
        sdcard_end_job();

    return c;
}

static void sdcard_report (stream_write_ptr stream_write)
{
    stream_write("|SD:");
    stream_write(ftoa((float)file.pos / (float)file.size * 100.0f, 1));
//    stream_write(",");
//    stream_write(get_name(file.handle));
}

static bool sdcard_suspend (bool suspend)
{
    sys.block_input_stream = !suspend;
    if(suspend) {
        hal.stream.reset_read_buffer();
        hal.stream.read = active_stream.read;               // Restore normal stream input for tool change (jog etc)
        hal.report.status_message = report_status_message;  // as well as normal status messages reporting
    } else {
        hal.stream.read = sdcard_read;                      // Resume reading from SD card
        hal.report.status_message = trap_status_report;     // and redirect status messages back to us
    }

    return true;
}

static status_code_t sdcard_parse (uint_fast16_t state, char *line, char *lcline)
{
    status_code_t retval = Status_Unhandled;

    if(line[1] == 'F') switch(line[2]) {

        case '\0':
            retval = sdcard_ls(line); // (re)use line buffer for reporting filenames
            break;

        case 'M':
            retval = sdcard_mount() ? Status_OK : Status_SDMountError;
            break;

        case '=':
            if (state != STATE_IDLE)
                retval = Status_SystemGClock;
            else {
                if(file_open(&line[3])) {
                    gc_state.last_error = Status_OK;                            // Start with no errors
                    hal.report.status_message(Status_OK);                       // and confirm command to originator
                    memcpy(&active_stream, &hal.stream, sizeof(io_stream_t));   // Save current stream pointers
                    hal.stream.type = StreamSetting_SDCard;                     // then redirect to read from SD card instead
                    hal.stream.read = sdcard_read;                              // ...
#if M6_ENABLE
                    hal.stream.suspend_read = sdcard_suspend;                   // ...
#else
                    hal.stream.suspend_read = NULL;                             // ...
#endif
                    hal.driver_rt_report = sdcard_report;                       // Add percent complete to real time report
                    hal.report.status_message = trap_status_report;             // Redirect status message and feedback message
                    hal.report.feedback_message = trap_feedback_message;        // reports here
                    sys.block_input_stream = true;                              // Block serial input other than real time commands TODO: remove?
                    retval = Status_OK;
                } else
                    retval = Status_SDReadError;
            }
            break;

        default:
            retval = Status_InvalidStatement;
            break;
    }

    return retval;
}

void sdcard_reset (void)
{
    if(hal.stream.type == StreamSetting_SDCard) {
        char buf[70];
        sprintf(buf, "[MSG:Reset during streaming of SD file at line: %d]\r\n", file.line);
        hal.stream.write(buf);
        sdcard_end_job();
    }
}

void sdcard_init (void)
{
#ifdef __MSP432E401Y__
    SDFatFS_init();
#endif
    hal.driver_sys_command_execute = sdcard_parse;
}
