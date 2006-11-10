/*****************************************************************************
 * puzzle.c : Puzzle game
 *****************************************************************************
 * Copyright (C) 2005-2006 the VideoLAN team
 * $Id$
 *
 * Authors: Antoine Cellerier <dionoea -at- videolan -dot- org>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>                                      /* malloc(), free() */
#include <string.h>

#include <vlc/vlc.h>
#include <vlc/vout.h>

#include <math.h>

#include "filter_common.h"
#include "vlc_image.h"
#include "vlc_input.h"
#include "vlc_playlist.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  Create    ( vlc_object_t * );
static void Destroy   ( vlc_object_t * );

static int  Init      ( vout_thread_t * );
static void End       ( vout_thread_t * );
static void Render    ( vout_thread_t *, picture_t * );

static int  SendEvents   ( vlc_object_t *, char const *,
                           vlc_value_t, vlc_value_t, void * );
static int  MouseEvent   ( vlc_object_t *, char const *,
                           vlc_value_t, vlc_value_t, void * );

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/

#define ROWS_TEXT N_("Number of puzzle rows")
#define ROWS_LONGTEXT N_("Number of puzzle rows")
#define COLS_TEXT N_("Number of puzzle columns")
#define COLS_LONGTEXT N_("Number of puzzle columns")
#define BLACKSLOT_TEXT N_("Make one tile a black slot")
#define BLACKSLOT_LONGTEXT N_("Make one slot black. Other tiles can only be swapped with the black slot.")

vlc_module_begin();
    set_description( _("Puzzle interactive game video filter") );
    set_shortname( _( "Puzzle" ));
    set_capability( "video filter", 0 );
    set_category( CAT_VIDEO );
    set_subcategory( SUBCAT_VIDEO_VFILTER );

    add_integer_with_range( "puzzle-rows", 4, 1, 128, NULL,
                            ROWS_TEXT, ROWS_LONGTEXT, VLC_FALSE );
    add_integer_with_range( "puzzle-cols", 4, 1, 128, NULL,
                            COLS_TEXT, COLS_LONGTEXT, VLC_FALSE );
    add_bool( "puzzle-black-slot", 0, NULL,
              BLACKSLOT_TEXT, BLACKSLOT_LONGTEXT, VLC_FALSE );

    set_callbacks( Create, Destroy );
vlc_module_end();

/*****************************************************************************
 * vout_sys_t: Magnify video output method descriptor
 *****************************************************************************/
struct vout_sys_t
{
    vout_thread_t *p_vout;

    image_handler_t *p_image;

    int i_cols;
    int i_rows;
    int *pi_order;
    int i_selected;
    vlc_bool_t b_finished;

    vlc_bool_t b_blackslot;
};

/*****************************************************************************
 * Control: control facility for the vout (forwards to child vout)
 *****************************************************************************/
static int Control( vout_thread_t *p_vout, int i_query, va_list args )
{
    return vout_vaControl( p_vout->p_sys->p_vout, i_query, args );
}

/*****************************************************************************
 * Misc stuff...
 *****************************************************************************/
static vlc_bool_t finished( vout_sys_t *p_sys )
{
    int i;
    for( i = 0; i < p_sys->i_cols * p_sys->i_rows; i++ )
    {
        if( i != p_sys->pi_order[i] ) return VLC_FALSE;
    }
    return VLC_TRUE;
}
static void shuffle( vout_sys_t *p_sys )
{
    int i, c;
    free( p_sys->pi_order );
    p_sys->pi_order = malloc( p_sys->i_cols * p_sys->i_rows * sizeof( int ) );
    do
    {
        for( i = 0; i < p_sys->i_cols * p_sys->i_rows; i++ )
        {
            p_sys->pi_order[i] = -1;
        }
        i = 0;
        for( c = 0; c < p_sys->i_cols * p_sys->i_rows; )
        {
            i = rand()%( p_sys->i_cols * p_sys->i_rows );
            if( p_sys->pi_order[i] == -1 )
            {
                p_sys->pi_order[i] = c;
                c++;
            }
        }
        p_sys->b_finished = finished( p_sys );
    } while( p_sys->b_finished == VLC_TRUE );

    if( p_sys->b_blackslot == VLC_TRUE )
    {
        for( i = 0; i < p_sys->i_cols * p_sys->i_rows; i++ )
        {
            if( p_sys->pi_order[i] ==
                ( p_sys->i_cols - 1 ) * p_sys->i_rows )
            {
                p_sys->i_selected = i;
                break;
            }
        }
    }
    else
    {
        p_sys->i_selected = -1;
    }
    printf( "selected: %d\n", p_sys->i_selected );
}

/*****************************************************************************
 * Create: allocates Magnify video thread output method
 *****************************************************************************/
static int Create( vlc_object_t *p_this )
{
    vout_thread_t *p_vout = (vout_thread_t *)p_this;

    /* Allocate structure */
    p_vout->p_sys = malloc( sizeof( vout_sys_t ) );
    if( p_vout->p_sys == NULL )
    {
        msg_Err( p_vout, "out of memory" );
        return VLC_ENOMEM;
    }

    p_vout->p_sys->p_image = image_HandlerCreate( p_vout );

    p_vout->p_sys->i_rows = config_GetInt( p_vout, "puzzle-rows" );
    p_vout->p_sys->i_cols = config_GetInt( p_vout, "puzzle-cols" );
    p_vout->p_sys->b_blackslot = config_GetInt( p_vout, "puzzle-black-slot" );

    p_vout->p_sys->pi_order = NULL;
    shuffle( p_vout->p_sys );

    p_vout->pf_init = Init;
    p_vout->pf_end = End;
    p_vout->pf_manage = NULL;
    p_vout->pf_render = Render;
    p_vout->pf_display = NULL;
    p_vout->pf_control = Control;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Init: initialize Magnify video thread output method
 *****************************************************************************/
static int Init( vout_thread_t *p_vout )
{
    int i_index;
    picture_t *p_pic;
    video_format_t fmt = {0};

    I_OUTPUTPICTURES = 0;

    /* Initialize the output structure */
    p_vout->output.i_chroma = p_vout->render.i_chroma;
    p_vout->output.i_width  = p_vout->render.i_width;
    p_vout->output.i_height = p_vout->render.i_height;
    p_vout->output.i_aspect = p_vout->render.i_aspect;

    p_vout->fmt_out = p_vout->fmt_in;
    fmt = p_vout->fmt_out;

    /* Try to open the real video output */
    msg_Dbg( p_vout, "spawning the real video output" );

    p_vout->p_sys->p_vout = vout_Create( p_vout, &fmt );

    /* Everything failed */
    if( p_vout->p_sys->p_vout == NULL )
    {
        msg_Err( p_vout, "cannot open vout, aborting" );
        return VLC_EGENERIC;
    }

    var_AddCallback( p_vout->p_sys->p_vout, "mouse-x", MouseEvent, p_vout );
    var_AddCallback( p_vout->p_sys->p_vout, "mouse-y", MouseEvent, p_vout );
    var_AddCallback( p_vout->p_sys->p_vout, "mouse-clicked",
                     MouseEvent, p_vout);

    ALLOCATE_DIRECTBUFFERS( VOUT_MAX_PICTURES );
    ADD_CALLBACKS( p_vout->p_sys->p_vout, SendEvents );
    ADD_PARENT_CALLBACKS( SendEventsToChild );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * End: terminate Magnify video thread output method
 *****************************************************************************/
static void End( vout_thread_t *p_vout )
{
    int i_index;

    /* Free the fake output buffers we allocated */
    for( i_index = I_OUTPUTPICTURES ; i_index ; )
    {
        i_index--;
        free( PP_OUTPUTPICTURE[ i_index ]->p_data_orig );
    }

    var_DelCallback( p_vout->p_sys->p_vout, "mouse-x", MouseEvent, p_vout);
    var_DelCallback( p_vout->p_sys->p_vout, "mouse-y", MouseEvent, p_vout);
    var_DelCallback( p_vout->p_sys->p_vout, "mouse-clicked", MouseEvent, p_vout);
}

/*****************************************************************************
 * Destroy: destroy Magnify video thread output method
 *****************************************************************************/
static void Destroy( vlc_object_t *p_this )
{
    vout_thread_t *p_vout = (vout_thread_t *)p_this;

    if( p_vout->p_sys->p_vout )
    {
        DEL_CALLBACKS( p_vout->p_sys->p_vout, SendEvents );
        vlc_object_detach( p_vout->p_sys->p_vout );
        vout_Destroy( p_vout->p_sys->p_vout );
    }

    image_HandlerDelete( p_vout->p_sys->p_image );
    free( p_vout->p_sys->pi_order );

    DEL_PARENT_CALLBACKS( SendEventsToChild );

    free( p_vout->p_sys );
}

/*****************************************************************************
 * Render: displays previously rendered output
 *****************************************************************************/
static void Render( vout_thread_t *p_vout, picture_t *p_pic )
{
    picture_t *p_outpic;

    //video_format_t fmt_out = {0};
    //picture_t *p_converted;

    int i_plane;

    int i_rows = p_vout->p_sys->i_rows;
    int i_cols = p_vout->p_sys->i_cols;

    /* This is a new frame. Get a structure from the video_output. */
    while( ( p_outpic = vout_CreatePicture( p_vout->p_sys->p_vout, 0, 0, 0 ) )
              == NULL )
    {
        if( p_vout->b_die || p_vout->b_error )
        {
            return;
        }
        msleep( VOUT_OUTMEM_SLEEP );
    }

    vout_DatePicture( p_vout->p_sys->p_vout, p_outpic, p_pic->date );

    for( i_plane = 0; i_plane < p_outpic->i_planes; i_plane++ )
    {
        plane_t *p_in = p_pic->p+i_plane;
        plane_t *p_out = p_outpic->p+i_plane;
        int i_pitch = p_in->i_pitch;
        int i;

        for( i = 0; i < i_cols * i_rows; i++ )
        {
            int i_col = i % i_cols;
            int i_row = i / i_cols;
            int i_ocol = p_vout->p_sys->pi_order[i] % i_cols;
            int i_orow = p_vout->p_sys->pi_order[i] / i_cols;
            int i_last_row = i_row + 1;
            i_orow *= p_in->i_lines / i_rows;
            i_row *= p_in->i_lines / i_rows;
            i_last_row *= p_in->i_lines / i_rows;

            if( p_vout->p_sys->b_blackslot == VLC_TRUE
                && i == p_vout->p_sys->i_selected )
            {
                uint8_t color = ( i_plane == Y_PLANE ? 0x0 : 0x80 );
                for( ; i_row < i_last_row; i_row++, i_orow++ )
                {
                    memset( p_out->p_pixels + i_row * i_pitch
                                            + i_col * i_pitch / i_cols,
                            color, i_pitch / i_cols );
                }
            }
            else
            {
                for( ; i_row < i_last_row; i_row++, i_orow++ )
                {
                    memcpy( p_out->p_pixels + i_row * i_pitch
                                            + i_col * i_pitch / i_cols,
                            p_in->p_pixels + i_orow * i_pitch
                                           + i_ocol * i_pitch / i_cols,
                            i_pitch / i_cols );
                }
            }
        }
    }

    if(    p_vout->p_sys->i_selected != -1
        && p_vout->p_sys->b_blackslot == VLC_FALSE )
    {
        plane_t *p_in = p_pic->p+Y_PLANE;
        plane_t *p_out = p_outpic->p+Y_PLANE;
        int i_pitch = p_in->i_pitch;
        int i_col = p_vout->p_sys->i_selected % i_cols;
        int i_row = p_vout->p_sys->i_selected / i_cols;
        int i_last_row = i_row + 1;
        i_row *= p_in->i_lines / i_rows;
        i_last_row *= p_in->i_lines / i_rows;
        memset( p_out->p_pixels + i_row * i_pitch
                                + i_col * i_pitch / i_cols,
                0xff, i_pitch / i_cols );
        for( ; i_row < i_last_row; i_row++ )
        {
            p_out->p_pixels[   i_row * i_pitch
                             + i_col * i_pitch / i_cols ] = 0xff;
            p_out->p_pixels[ i_row * i_pitch
                             + (i_col+1) * i_pitch / i_cols - 1 ] = 0xff;
        }
        i_row--;
        memset( p_out->p_pixels + i_row * i_pitch
                                + i_col * i_pitch / i_cols,
                0xff, i_pitch / i_cols );
    }

    vout_DisplayPicture( p_vout->p_sys->p_vout, p_outpic );
}

/*****************************************************************************
 * SendEvents: forward mouse and keyboard events to the parent p_vout
 *****************************************************************************/
static int SendEvents( vlc_object_t *p_this, char const *psz_var,
                       vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    var_Set( (vlc_object_t *)p_data, psz_var, newval );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * SendEventsToChild: forward events to the child/children vout
 *****************************************************************************/
static int SendEventsToChild( vlc_object_t *p_this, char const *psz_var,
                       vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    vout_thread_t *p_vout = (vout_thread_t *)p_this;
    var_Set( p_vout->p_sys->p_vout, psz_var, newval );
    return VLC_SUCCESS;
}

/*****************************************************************************
 * MouseEvent: callback for mouse events
 *****************************************************************************/
static int MouseEvent( vlc_object_t *p_this, char const *psz_var,
                       vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    vout_thread_t *p_vout = (vout_thread_t*)p_data;
    int i_x, i_y;
    int i_v;

#define MOUSE_DOWN    1
#define MOUSE_CLICKED 2
#define MOUSE_MOVE_X  4
#define MOUSE_MOVE_Y  8
#define MOUSE_MOVE    12
    uint8_t mouse= 0;

    int v_h = p_vout->output.i_height;
    int v_w = p_vout->output.i_width;
    int i_pos;

    if( psz_var[6] == 'x' ) mouse |= MOUSE_MOVE_X;
    if( psz_var[6] == 'y' ) mouse |= MOUSE_MOVE_Y;
    if( psz_var[6] == 'c' ) mouse |= MOUSE_CLICKED;

    i_v = var_GetInteger( p_vout->p_sys->p_vout, "mouse-button-down" );
    if( i_v & 0x1 ) mouse |= MOUSE_DOWN;
    i_y = var_GetInteger( p_vout->p_sys->p_vout, "mouse-y" );
    i_x = var_GetInteger( p_vout->p_sys->p_vout, "mouse-x" );

    if( mouse & MOUSE_CLICKED )
    {
        i_pos = p_vout->p_sys->i_cols * ( ( p_vout->p_sys->i_rows * i_y ) / v_h ) + (p_vout->p_sys->i_cols * i_x ) / v_w;
        if( p_vout->p_sys->b_finished == VLC_TRUE )
        {
            shuffle( p_vout->p_sys );
        }
        else if( p_vout->p_sys->i_selected == -1 )
        {
            p_vout->p_sys->i_selected = i_pos;
        }
        else if( p_vout->p_sys->i_selected == i_pos
                 && p_vout->p_sys->b_blackslot == VLC_FALSE )
        {
            p_vout->p_sys->i_selected = -1;
        }
        else if(    p_vout->p_sys->i_selected == i_pos + 1
                 || p_vout->p_sys->i_selected == i_pos - 1
                 || p_vout->p_sys->i_selected == i_pos + p_vout->p_sys->i_cols
                 || p_vout->p_sys->i_selected == i_pos - p_vout->p_sys->i_cols )
        {
            int a = p_vout->p_sys->pi_order[ p_vout->p_sys->i_selected ];
            p_vout->p_sys->pi_order[ p_vout->p_sys->i_selected ] =
                p_vout->p_sys->pi_order[ i_pos ];
            p_vout->p_sys->pi_order[ i_pos ] = a;
            if( p_vout->p_sys->b_blackslot == VLC_TRUE )
                p_vout->p_sys->i_selected = i_pos;
            else
                p_vout->p_sys->i_selected = -1;

            p_vout->p_sys->b_finished = finished( p_vout->p_sys );
        }
    }
    return VLC_SUCCESS;
}
