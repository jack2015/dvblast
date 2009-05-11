/*****************************************************************************
 * dvblast.c
 *****************************************************************************
 * Copyright (C) 2004, 2008-2009 the VideoLAN team
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
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

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#include "dvblast.h"

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
output_t **pp_outputs = NULL;
int i_nb_outputs = 0;
output_t output_dup = { 0 };
static char *psz_conf_file = NULL;
char *psz_srv_socket = NULL;
int i_ttl = 64;
in_addr_t i_ssrc = 0;
static int i_priority = -1;
int i_adapter = 0;
int i_frequency = 0;
int i_srate = 27500000;
int i_voltage = 13;
int b_tone = 0;
int i_bandwidth = 8;
char *psz_modulation = NULL;
int b_budget_mode = 0;
volatile int b_hup_received = 0;

/*****************************************************************************
 * Configuration files
 *****************************************************************************/
static void ReadConfiguration( char *psz_file )
{
    FILE *p_file;
    char psz_line[2048];
    int i;

    if ( (p_file = fopen( psz_file, "r" )) == NULL )
    {
        msg_Err( NULL, "can't fopen config file %s", psz_file );
        return;
    }

    while ( fgets( psz_line, sizeof(psz_line), p_file ) != NULL )
    {
        output_t *p_output = NULL;
        char *psz_parser, *psz_token, *psz_token2;
        struct in_addr maddr;
        uint16_t i_port = DEFAULT_PORT;
        uint16_t i_sid = 0;
        uint16_t *pi_pids = NULL;
        int i_nb_pids = 0;
        int b_watch;

        psz_token = strtok_r( psz_line, "\t\n ", &psz_parser );
        if ( psz_token == NULL )
            continue;

        if ( (psz_token2 = strrchr( psz_token, ':' )) != NULL )
        {
            *psz_token2 = '\0';
            i_port = atoi( psz_token2 + 1 );
        }
        if ( !inet_aton( psz_token, &maddr ) )
            continue;

        psz_token = strtok_r( NULL, "\t\n ", &psz_parser );
        if ( psz_token == NULL )
            continue;
        b_watch = atoi( psz_token );

        psz_token = strtok_r( NULL, "\t\n ", &psz_parser );
        if ( psz_token == NULL )
            continue;
        i_sid = strtol(psz_token, NULL, 0);

        psz_token = strtok_r( NULL, "\t\n ", &psz_parser );
        if ( psz_token != NULL )
        {
            psz_parser = NULL;
            for ( ; ; )
            {
                psz_token = strtok_r( psz_token, ",", &psz_parser );
                if ( psz_token == NULL )
                    break;
                pi_pids = realloc( pi_pids,
                                   (i_nb_pids + 1) * sizeof(uint16_t) );
                pi_pids[i_nb_pids++] = strtol(psz_token, NULL, 0);
                psz_token = NULL;
            }
        }

        msg_Dbg( NULL, "conf: %s:%u w=%d sid=%d pids[%d]=%d,%d,%d,%d,%d...",
                 inet_ntoa( maddr ), i_port, b_watch, i_sid, i_nb_pids,
                 i_nb_pids < 1 ? -1 : pi_pids[0],
                 i_nb_pids < 2 ? -1 : pi_pids[1],
                 i_nb_pids < 3 ? -1 : pi_pids[2],
                 i_nb_pids < 4 ? -1 : pi_pids[3],
                 i_nb_pids < 5 ? -1 : pi_pids[4] );

        for ( i = 0; i < i_nb_outputs; i++ )
        {
            if ( pp_outputs[i]->i_maddr == maddr.s_addr
                  && pp_outputs[i]->i_port == i_port )
            {
                p_output = pp_outputs[i];
                break;
            }
        }
        if ( i == i_nb_outputs )
            p_output = output_Create( maddr.s_addr, i_port );

        if ( p_output != NULL )
        {
            demux_Change( p_output, i_sid, pi_pids, i_nb_pids );
            p_output->b_watch = (b_watch == 1);
            p_output->b_still_present = 1;
        }

        free( pi_pids );
    }

    fclose( p_file );

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        if ( pp_outputs[i]->i_maddr && !pp_outputs[i]->b_still_present )
        {
            struct in_addr s;
            s.s_addr = pp_outputs[i]->i_maddr;
            msg_Dbg( NULL, "closing %s:%u", inet_ntoa( s ),
                     pp_outputs[i]->i_port );
            demux_Change( pp_outputs[i], 0, NULL, 0 );
            output_Close( pp_outputs[i] );
        }

        pp_outputs[i]->b_still_present = 0;
    }
}

/*****************************************************************************
 * Signal Handler
 *****************************************************************************/
static void SigHandler( int i_signal )
{
    b_hup_received = 1;
}

/*****************************************************************************
 * Entry point
 *****************************************************************************/
void usage()
{
    msg_Err( NULL, "Usage: dvblast -c <config file> [-r <remote socket>] [-t <ttl>] [-o <SSRC IP>] [-i <RT priority>] [-a <adapter>] -f <frequency> [-s <symbol rate>] [-v <0|13|18>] [-p] [-b <bandwidth>] [-m <modulation] [-u] [-d <dest IP:port>]" );
    msg_Err( NULL, "-v: voltage to apply to the LNB (QPSK)" );
    msg_Err( NULL, "-p: force 22kHz pulses for high-band selection (DVB-S)" );
    msg_Err( NULL, "-m: DVB-C  qpsk|qam_16|qam_32|qam_64|qam_128|qam_256 (default qam_auto)" );
    msg_Err( NULL, "    DVB-T  qam_16|qam_32|qam_64|qam_128|qam_256 (default qam_auto)" );
    msg_Err( NULL, "    DVB-S2 qpsk|psk_8 (default legacy DVB-S)" );
    msg_Err( NULL, "-u: turn on budget mode (no hardware PID filtering)" );
    msg_Err( NULL, "-d: duplicate all received packets to a given port" );
    exit(1);
}

int main( int i_argc, char **pp_argv )
{
    struct sched_param param;
    int i_error;

    if ( i_argc == 1 )
        usage();

    msg_Err( NULL, "restarting" );

    for ( ; ; )
    {
        char c;

        if ( (c = getopt(i_argc, pp_argv, "c:r:t:o:i:a:f:s:v:pb:m:ud:h")) == -1 )
            break;

        switch ( c )
        {
        case 'c':
            psz_conf_file = optarg;
            break;

        case 'r':
            psz_srv_socket = optarg;
            break;

        case 't':
            i_ttl = strtol( optarg, NULL, 0 );
            break;

        case 'o':
        {
            struct in_addr maddr;
            if ( !inet_aton( optarg, &maddr ) )
                usage();
            i_ssrc = maddr.s_addr;
            break;
        }

        case 'i':
            i_priority = strtol( optarg, NULL, 0 );
            break;

        case 'a':
            i_adapter = strtol( optarg, NULL, 0 );
            break;

        case 'f':
            i_frequency = strtol( optarg, NULL, 0 );
            break;

        case 's':
            i_srate = strtol( optarg, NULL, 0 );
            break;

        case 'v':
            i_voltage = strtol( optarg, NULL, 0 );
            break;

        case 'p':
            b_tone = 1;
            break;

        case 'b':
            i_bandwidth = strtol( optarg, NULL, 0 );
            break;

        case 'm':
            psz_modulation = optarg;
            break;

        case 'u':
            b_budget_mode = 1;
            break;

        case 'd':
        {
            char *psz_token;
            uint16_t i_port = DEFAULT_PORT;
            struct in_addr maddr;
            if ( (psz_token = strrchr( optarg, ':' )) != NULL )
            {
                *psz_token = '\0';
                i_port = atoi( psz_token + 1 );
            }
            if ( !inet_aton( optarg, &maddr ) )
                usage();
            output_Init( &output_dup, maddr.s_addr, i_port );
            break;
        }

        case 'h':
        default:
            usage();
        }
    }

    signal( SIGHUP, SigHandler );
    srand( time(NULL) * getpid() );

    demux_Open();

    if ( i_priority > 0 )
    {
        memset( &param, 0, sizeof(struct sched_param) );
        param.sched_priority = i_priority;
        if ( (i_error = pthread_setschedparam( pthread_self(), SCHED_RR,
                                               &param )) )
        {
            msg_Warn( NULL, "couldn't set thread priority: %s",
                      strerror(i_error) );
        }
    }

    ReadConfiguration( psz_conf_file );

    if ( psz_srv_socket != NULL )
        comm_Open();

    for ( ; ; )
    {
        if ( b_hup_received )
        {
            b_hup_received = 0;
            msg_Warn( NULL, "HUP received, reloading" );
            ReadConfiguration( psz_conf_file );
        }

        demux_Run();
    }
}