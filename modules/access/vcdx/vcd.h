/*****************************************************************************
 * vcd.h : VCD input module for vlc
 *          using libcdio, libvcd and libvcdinfo
 *****************************************************************************
 * Copyright (C) 2003 VideoLAN
 * $Id: vcd.h,v 1.2 2003/11/20 02:17:33 rocky Exp $
 *
 * Authors: Rocky Bernstein <rocky@panix.com> 
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

#include <libvcd/info.h>

/*****************************************************************************
 * dvd_data_t: structure for communication between dvdplay access, demux
 * and intf.
 *****************************************************************************/

/*****************************************************************************
 * vcd_data_t: structure for communication between access and intf.
 *****************************************************************************/
typedef struct
{
#if FINISHED
    vcdplay_ptr             vmg;
#endif
    intf_thread_t *         p_intf;

#if DEMUX_FINISHED
    int                     i_audio_nb;
    int                     i_spu_nb;
#endif

    int                     i_still_time;
    bool                    b_end_of_cell;

#if FINISHED
    vcdplay_event_t         event;
    vcdplay_ctrl_t          control;   
    vcdplay_highlight_t     hli;
#endif

} vcd_data_t;

int  VCDSetArea      ( input_thread_t *, input_area_t * );
void VCDSeek         ( input_thread_t *, off_t );
int  VCDPlay         ( input_thread_t *, vcdinfo_itemid_t );
