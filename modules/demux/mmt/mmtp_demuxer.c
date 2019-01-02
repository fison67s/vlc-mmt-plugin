/*****************************************************************************
 * mmtp_demuxer.c : mmtp demuxer for ngbp
 *****************************************************************************
 *
 *
 * Author: jason@jasonjustman.com
 *
 * 2018-12-21
 *
 * Notes: alpha MMT parser and MPU/MFU chained demuxing
 *
 * Dependencies: pcap MMT unicast replay files or live ATSC 3.0 network mulitcast reception/reflection (see https://redzonereceiver.tv/)
 *
 * airwavez redzone SDR USB dongle
 *
 * the user space module can be flakey with recovery if the usb connection drops.
 * i use a script similar to the following to turn up, tune and monitor:
 *

#!/bin/bash

# Allow Multicast IP on the enp0s6 interface and route it there instead of to the wired interface
sudo ifconfig lo -multicast
sudo ifconfig enp0s5 -multicast
sudo ifconfig enp0s6 multicast
sudo route del -net 224.0.0.0 netmask 240.0.0.0 dev lo
sudo route add -net 224.0.0.0 netmask 240.0.0.0 dev enp0s6


#start userspace driver
klatsc30_web_ui -f -p 8080 &

sleep 10
#tune to channel 43 - you'll need to find a testing market (e.g. dallas or phenix)
wget 'http://127.0.0.1:8080/networktuner/tunefrequency?json={"operating_mode":"ATSC3","frequency_plan":"US_BROADCAST","frequency_Hz":647000000, "plp0_id":0}'


sleep 5

#start ff for monitoring
firefox http://127.0.0.1:8080

 *
 *
 * replay:
 * wireshark tcp captures must be in libpcap format, and most likely need to have packet checksums realcualted before replay:
 * e.g. tcprewrite --fixcsum -i 2018-12-17-mmt-airwavz-bad-checksums.pcap -o 2018-12-17-mmt-airwavz-recalc.pcap

 * replay via, e.g. bittwist -i enp0s6 2018-12-17-mmt-airwavz-recalc.pcap -v
 *
 *
 * lastly, i then have a host only interface between my ubuntu and mac configured in parallels, but mac's management of the mulitcast routes is a bit weird,
 * the two scripts will revoke any autoconfigured interface mulitcast routes, and then manually add the dedicated 224 route to the virtual host-only network:
 *
 *
cat /usr/local/bin/deleteMulticastRoute
netstat -nr
sudo route delete -net 224.0.0.0/4
sudo route delete -host 239.255.10.2
sudo route delete -host 239.255.255.250
sudo route delete -net 255.255.255.255/32

 cat /usr/local/bin/addVnic1MulitcastRoute
sudo route -nv add -net 224.0.0.0/4 -interface vnic1

 *
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

/** cascasde libmp4 headers here ***/
#include "mp4.h"

#include <vlc_demux.h>
#include <vlc_charset.h>                           /* EnsureUTF8 */
#include <vlc_input.h>
#include <vlc_aout.h>
#include <vlc_plugin.h>
#include <vlc_dialog.h>
#include <vlc_url.h>
#include <assert.h>
#include <limits.h>
#include "../codec/cc.h"
#include "heif.h"
#include "../av1_unpack.h"




#define ACCESS_TEXT N_("MMTP Demuxer module")
#define FILE_TEXT N_("Dump filename")
#define FILE_LONGTEXT N_( \
    "Name of the file to which the raw stream will be dumped." )
#define APPEND_TEXT N_("Append to existing file")
#define APPEND_LONGTEXT N_( \
    "If the file already exists, it will not be overwritten." )

static int  Open( vlc_object_t * );
static void Close ( vlc_object_t * );

#define MIN(a,b) (((a)<(b))?(a):(b))

#define DEMUX_INCREMENT VLC_TICK_FROM_MS(250) /* How far the pcr will go, each round */
#define DEMUX_TRACK_MAX_PRELOAD VLC_TICK_FROM_SEC(15) /* maximum preloading, to deal with interleaving */

#define INVALID_PRELOAD  UINT_MAX

#define VLC_DEMUXER_EOS (VLC_DEMUXER_EGENERIC - 1)
#define VLC_DEMUXER_FATAL (VLC_DEMUXER_EGENERIC - 2)

const uint32_t rgi_pict_atoms[2] = { ATOM_PICT, ATOM_pict };
const char *psz_meta_roots[] = { "/moov/udta/meta/ilst",
                                 "/moov/meta/ilst",
                                 "/moov/udta/meta",
                                 "/moov/udta",
                                 "/meta/ilst",
                                 "/udta",
                                 NULL };

static int __MFU_COUNTER=1;

/*
 * hack
 *
 *

/**

 * Ansi C "itoa" based on Kernighan & Ritchie's "Ansi C"


 * with slight modification to optimize for specific architecture:

 */

void strreverse(char* begin, char* end) {
	char aux;
	while(end>begin)
		aux=*end, *end--=*begin, *begin++=aux;
}

void itoa(int value, char* str, int base) {

	static char num[] = "0123456789abcdefghijklmnopqrstuvwxyz";
	char* wstr=str;
	int sign;
	div_t res;

	// Validate base
	if (base<2 || base>35){ *wstr='\0'; return; }

	// Take care of sign
	if ((sign=value) < 0) value = -value;

	// Conversion. Number is reversed.
	do {
		res = div(value,base);
		*wstr++ = num[res.rem];
	}while(value=res.quot);

	if(sign<0) *wstr++='-';

	*wstr='\0';

	// Reverse string
	strreverse(str,wstr-1);
}


vlc_module_begin ()
    set_shortname("MMTP")
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_DEMUX )
    set_description( N_("MMTP Demuxer") )
    set_capability( "demux", 500 )
  //  add_module("demuxdump-access", "sout access", "file",
  //            ACCESS_TEXT, ACCESS_TEXT)
  //  add_savefile("demuxdump-file", "stream-demux.dump",
  //               FILE_TEXT, FILE_LONGTEXT)
  //  add_bool( "demuxdump-append", false, APPEND_TEXT, APPEND_LONGTEXT,
  //           false )
    set_callbacks( Open, Close )
    add_shortcut( "MMTP" )
vlc_module_end ()




typedef struct
{
	vlc_object_t *obj;

	//reconsititue mfu's into a p_out_muxed fifo

	block_t *p_mpu_block;

	//everthing below here is from libmp4

    MP4_Box_t    *p_root;      /* container for the whole file */

    vlc_tick_t   i_pcr;

    uint64_t     i_moov_duration;
    uint64_t     i_duration;           /* Declared fragmented duration (movie time scale) */
    uint64_t     i_cumulated_duration; /* Same as above, but not from probing, (movie time scale) */
    uint32_t     i_timescale;          /* movie time scale */
    vlc_tick_t   i_nztime;             /* time position of the presentation (CLOCK_FREQ timescale) */
    unsigned int i_tracks;       /* number of tracks */
    mp4_track_t  *track;         /* array of track */
    float        f_fps;          /* number of frame per seconds */

    bool         b_fragmented;   /* fMP4 */
    bool         b_seekable;
    bool         b_fastseekable;
    bool         b_error;        /* unrecoverable */

    bool            b_index_probed;     /* mFra sync points index */
    bool            b_fragments_probed; /* moof segments index created */

    MP4_Box_t *p_moov;

    struct
    {
        uint32_t        i_current_box_type;
        MP4_Box_t      *p_fragment_atom;
        uint64_t        i_post_mdat_offset;
        uint32_t        i_lastseqnumber;
    } context;

    /* */
    MP4_Box_t    *p_tref_chap;

    /* */
    bool seekpoint_changed;
    int          i_seekpoint;
    input_title_t *p_title;
    vlc_meta_t    *p_meta;

    /* ASF in MP4 */
    asf_packet_sys_t asfpacketsys;
    vlc_tick_t i_preroll;       /* foobar */
    vlc_tick_t i_preroll_start;

    struct
    {
        int es_cat_filters;
    } hacks;

    mp4_fragments_index_t *p_fragsindex;

    sig_atomic_t has_processed_ftype_moov;
    vlc_thread_t  demux_frag_thread;
    vlc_sem_t demux_frag_new_data_semaphore;
    stream_t *s_frag;

    /** temp hacks until we have a map of mpu_sequence_numbers, use -1 for default values (0 is valid in mmtp spec)**/
    sig_atomic_t last_mpu_sequence_number;
    sig_atomic_t last_mpu_fragment_type;

} demux_sys_t;



static int   Demux   ( demux_t * );
static int   DemuxRef( demux_t *p_demux ){ (void)p_demux; return 0;}
static int   DemuxFrag( demux_t * );
static int   Control ( demux_t *, int, va_list );


//old sig -
//void processMpuPacket(demux_t* p_demux, uint16_t mmtp_packet_id, uint8_t mpu_fragment_type, uint8_t mpu_fragmentation_indicator, block_t *tmp_mpu_fragment );
void   processMpuPacket(demux_t* p_demux, uint16_t mmtp_packet_id, uint32_t mpu_sequence_number, uint32_t sample_number, uint32_t mpu_offset, uint8_t mpu_fragment_type, uint8_t mpu_fragmentation_indicator, block_t *tmp_mpu_fragment );


static void LoadChapter( demux_t  *p_demux );
static int LoadInitFrag( demux_t *p_demux );
static int __mp4_Demux( demux_t *p_demux );

void dumpMpu(demux_t *p_demux, block_t *mpu);
void dumpMfu(demux_t *p_demux, block_t *mpu);



/*****************************************************************************
 * Declaration of local function
 *****************************************************************************/
static void MP4_TrackSetup( demux_t *, mp4_track_t *, MP4_Box_t  *, bool, bool );
static void MP4_TrackInit( mp4_track_t * );
static void MP4_TrackClean( es_out_t *, mp4_track_t * );

static void MP4_Block_Send( demux_t *, mp4_track_t *, block_t * );

static void MP4_TrackSelect  ( demux_t *, mp4_track_t *, bool );
static int  MP4_TrackSeek   ( demux_t *, mp4_track_t *, vlc_tick_t );

static uint64_t MP4_TrackGetPos    ( mp4_track_t * );
static uint32_t MP4_TrackGetReadSize( mp4_track_t *, uint32_t * );
static int      MP4_TrackNextSample( demux_t *, mp4_track_t *, uint32_t );
static void     MP4_TrackSetELST( demux_t *, mp4_track_t *, vlc_tick_t );

static void     MP4_UpdateSeekpoint( demux_t *, vlc_tick_t );

static MP4_Box_t * MP4_GetTrexByTrackID( MP4_Box_t *p_moov, const uint32_t i_id );
static void MP4_GetDefaultSizeAndDuration( MP4_Box_t *p_moov,
                                           const MP4_Box_data_tfhd_t *p_tfhd_data,
                                           uint32_t *pi_default_size,
                                           uint32_t *pi_default_duration );
static int CreateTracks( demux_t *p_demux, unsigned i_tracks );

static stime_t GetMoovTrackDuration( demux_sys_t *p_sys, unsigned i_track_ID );

static int  ProbeFragments( demux_t *p_demux, bool b_force, bool *pb_fragmented );
static int  ProbeIndex( demux_t *p_demux );

static int FragCreateTrunIndex( demux_t *, MP4_Box_t *, MP4_Box_t *, stime_t );

static int FragGetMoofBySidxIndex( demux_t *p_demux, vlc_tick_t i_target_time,
                                   uint64_t *pi_moof_pos, vlc_tick_t *pi_sampletime );
static int FragGetMoofByTfraIndex( demux_t *p_demux, const vlc_tick_t i_target_time, unsigned i_track_ID,
                                   uint64_t *pi_moof_pos, vlc_tick_t *pi_sampletime );
static void FragResetContext( demux_sys_t * );

/* ASF Handlers */
static asf_track_info_t * MP4ASF_GetTrackInfo( asf_packet_sys_t *p_packetsys, uint8_t i_stream_number );
static void MP4ASF_Send(asf_packet_sys_t *p_packetsys, uint8_t i_stream_number, block_t **pp_frame);
static void MP4ASF_ResetFrames( demux_sys_t *p_sys );

/* RTP Hint track */
static block_t * MP4_RTPHint_Convert( demux_t *p_demux, block_t *p_block, vlc_fourcc_t i_codec );
static block_t * MP4_RTPHintToFrame( demux_t *p_demux, block_t *p_block, uint32_t packetcount );

static int MP4_LoadMeta( demux_sys_t *p_sys, vlc_meta_t *p_meta );
static void MP4_GetInterleaving( demux_t *p_demux, vlc_tick_t *pi_max_contiguous, bool *pb_flat );



//short reads from UDP may happen on starutp buffering or truncation
#define MIN_MMTP_SIZE 32
#define MAX_MMTP_SIZE 1514
#define MAX_MMT_REFRAGMENT_SIZE 65535

static int Demux( demux_t * );
static int Control( demux_t *, int,va_list );

/**
 * marry these two
typedef struct  {
	//reconsititue mfu's into a p_out_muxed fifo
    vlc_thread_t  thread;
    int update_chained;
	vlc_demux_chained_t *p_out_muxed;
	void *p_mp4_sys; //aggregate private from internal libmp4 structs
} demux_mmtp_sys_t;
*/



/**
 *
 *
 *  MMTP Packet V=0
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |V=0|C|FEC|r|X|R|RES|   type    |            packet_id          |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |					     	timestamp						   | 64
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |					 packet_sequence_number				 	   | 96
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |					     packet_counter				 	       | 128
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | 						 Header Extension				   ..... 160 == 20 bytes
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | 						   Payload Data				       ..... 192
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |				      source_FEC_payload_ID					   | 224
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *  ---
 *
 *  MMTP Packet V=1
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |V=1|C|FEC|X|R|Q|F|E|B|I| type  |           packet_id           |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |					     	timestamp						   | 64
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |					 packet_sequence_number				 	   | 96
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |					     packet_counter				 	       | 128
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |r|TB | DS  | TP  | flow_label  |         extension_header  ....| 160+n == 20bytes
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | 						   Payload Data				       ..... 192
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |				      source_FEC_payload_ID					   | 224
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * Semantics
 *
 * version  				indicates the version number of the MMTP protocol. This field shall be set
 * (V: 2 bits)			  	to "00" to for ISO 23008-1 2017, and 01 for QoS support.
 *
 * 							NOTE If version is set to 01, the length of type is set to 4 bits.
 *
 * packet_counter_flag   	1 in this field indicates that the packet_counter field is present.
 * (C: 1 bit)
 *
 * FEC_type  				indicates the type of the FEC scheme used for error protection of MMTP packets.
 * (FEC: 2 bits)			Valid values of this field are listed in Table 8.
 *								0	MMTP packet without source_FEC_payload_ID field (NO FEC applied)
 *								1	MMTP packet with source_FEC_payload_ID field
 *								2 	MMTP packet for repair symbol(s) for FEC Payload Mode 0 (FEC repair packet)
 *								3	MMTP packet for repair symbol(s) for FEC Payload Mode 1 (FEC repair packet)
 *
 * reserved					reserved for future use (in V=0, bit position 5 only)
 * (r: 1 bit)
 *
 * extension_flag			when set to 1, this flag indicates the header_extension field is present
 * (X: 1 bit) 					V=0, bit position 6
 *								V=1, bit position 5
 * RAP_flag
 * (R: 1 bit)				when set to 1, this flag indicates that the payload contains a Random Access Point to the datastream of that data type,
 *							defined by the data type itself.
 *
 * reserved					V=0 only - reserverd for future use
 * (RES: 2 bits)
 *
 * Compression_flag			V=1 only - this field will identify if header compression is used.
 * (B: 1 bit) 	 	 	 	 	 	 	 	 	 B=0, full size header will be used
 * 	 	 	 	 	 	 	 	 	 	 	 	 B=1, reduced size header will be used
 *
 * Indicator_flag			V=1 only - if set to I=1, this header is a reference header which will be
 * (I: 1 bit) 									 used later for a future packet with reduced headers.
 *
 * Payload Type				Payload data type and definitions, differs between V=0 and V=1
 * (type: 6 bits when V=0)
 * 							Value		Data Type			Definition of data unit
 * 							-----		---------			-----------------------
 * 							0x00		MPU					media-aware fragment of the MPU
 * 							0x01		generic object		generic such as complete MPU or another type
 * 							0x02		signaling message	one or more signaling messages
 * 							0x03		repair symbol		a single complete repair signal
 * 							0x04-0x1F	reserved 			for ISO use
 * 							0x20-0x3F	reserved			private use
 *
 * Payload Type
 * (type: 4 bits when V=1)
 * 							Value		Data Type			Definition of data unit
 * 							-----		---------			----------------------
 * 							0x0			MPU					media-aware fragment of the MPU
 * 							0x1			generic object		generic such as complete MPU or another type
 * 							0x2			signaling message	one or more signaling messages
 * 							0x3			repair signal		a single complete repair signal
 * 							0x4-0x9		reserved			for ISO use
 * 							0xA-0xF		reserved			private use
 *
 * packet_id				See ISO 23008-1 page 27
 * (16 bits)				used to distinguish one asset from another,
 * 							packet_id to asset_id is captured in the MMT Package Table as part of signaling message
 *
 * packet_sequence_number	used to distinguish between packets with the same packet_id
 * (32 bits)				begings at arbritary value, increases by one for each MMTP packet received,
 * 							and will wraparound to 0 at INT_MAX
 *
 * timestamp				time instance of MMTP packet delivery based upon UTC.
 * (32 bits)				short format defined in IETF RFC 5905 NTPv4 clause 6.
 *
 * packet_counter			integer value for counting MMTP packets, incremented by 1 when a MMTP packet is sent regardless of its packet_id value.
 * (32 bits)				arbitrary value, wraps around to 0
 * 							all packets of an MMTP flow shall have the same setting for packet_counter_flag (c)
 *
 * source_FEC_payload_ID	used only when FEC type=1.  MMTP packet will be AL-FEC Payload ID Mode
 * (32 bits)
 *
 * header_extension			contains user-defined information, used for proprietary extensions to the payload format
 * (16/16bits)						to enable applications and media types that require additional information the payload format header
 *
 * QoS_classifer flag		a value of 1 indicates the Qos classifer information is used
 * (Q: 1 bit)
 *
 * flow_identifer_flag		when set to 1, indicates that the flow identifier is used
 * (F:1 bit)					flow_label and flow_extnesion_flag fields, characteristics or ADC in a package
 *
 * flow_extension_flag		if there are more than 127 flows, this bit set set to 1 and more byte can be used in extension_header
 * (E: 1 bit)
 *
 * reliability_flag			when reliability flag is set to 0, data is loss tolerant (e.g media display), and pre-emptable by "transmission priority"
 * (r: 1 bit)				when reliability flag is set to 1, data is not loss tolerant (e.g signaling) and will pre-empt "transmission priority"
 *
 * type_of_bitrate			00 		constant bitrate, e.g. CBR
 * (TB: 2 bits)				01 		non-constrant bitrate, e.g. nCBR
 * 							10-11	reserved
 *
 * delay_sensitivity		indicates the sensitivty of the delay for end-to-end delivery
 * (DS: 3 bits)
 * 							111		conversational services (~100ms)
 * 							110		live-streming service (~1s)
 * 							101		delay-sensitive interactive service (~2s)
 * 							100		interactive service (~5s)
 * 							011		streaming service (~10s)
 * 							010		non-realtime service
 * 							001		reserved
 * 							000		reserved
 *
 * transmission_priority	provides the transmission priority of the packet, may be mapped
 * (TP: 3 bits)				to the NRI of the NAL, DSCP of IETF or other prioty fields from:
 * 							highest: 7 (1112)
 * 							lowest:  0 (0002)
 *
 * flow label				indicates the flow identifier, representing a bitstream or a group of bitstreams
 * (7 bits)					who's network resources are reserved according to an ADC or packet.
 * 							Range from 0-127, arbitrarily assigned in a session.
 */


/**
 *
 * run our isobmff demuxer/mp4 decoder as a separate thread from mpu processing
 *
 */

static void *DemuxFragThreadLoop(void *p_data) {
    demux_t *p_demux = (demux_t *) p_data;
    demux_sys_t *p_sys = p_demux->p_sys;

    for(;;) {
        vlc_sem_wait(&p_sys->demux_frag_new_data_semaphore);
        int canc = vlc_savecancel();

        DemuxFrag(p_demux);

        vlc_restorecancel (canc);
    }

    return NULL;
}

/*
 * Initializes the MMTP demuxer
 *
 * 	TODO: chain to MPU sub-demuxer when MPT.payload_type_id=0x00
 *
 * 	read the first 32 bits to parse out version, packet_type and packet_id
 * 		if we think this is an MMPT packet, then wire up Demux and Control callbacks
 *
 *
 * TODO:  add mutex in p_demux->p_sys
 *
 * setup mp4 demux thread, wait till we actually have udp data before completing probing
 */

static int Open( vlc_object_t * p_this )
{
    demux_t *p_demux = (demux_t*)p_this;
    demux_sys_t *p_sys = NULL;

    p_demux->pf_demux = Demux;
    p_demux->pf_control = Control;

    msg_Info(p_demux, "mmtp_demuxer.open() - inline libmp4_open");

    MP4_Box_t       *p_ftyp;
    const MP4_Box_t *p_mvhd = NULL;
    const MP4_Box_t *p_mvex = NULL;

    bool      b_enabled_es;
    p_sys = calloc( 1, sizeof( demux_sys_t ) );

    if ( !p_sys )
          return VLC_EGENERIC;

    p_demux->p_sys = p_sys;

    p_sys->obj = p_this;
    p_sys->context.i_lastseqnumber = UINT32_MAX;
    p_sys->p_mpu_block = NULL;
    p_sys->b_seekable = false;
    p_sys->b_fragmented = true;
    p_sys->s_frag = vlc_stream_fifo_New(p_this);
    vlc_sem_init(&p_sys->demux_frag_new_data_semaphore, 0);

    p_sys->last_mpu_sequence_number = -1;
    p_sys->last_mpu_fragment_type = -1;
    p_sys->has_processed_ftype_moov = 0;


    vlc_clone (&p_sys->demux_frag_thread, DemuxFragThreadLoop, p_demux, VLC_THREAD_PRIORITY_INPUT);

    msg_Info(p_demux, "mmtp_demuxer.open() - complete");

    return VLC_SUCCESS;
}

static int __processFirstMpuFragment(demux_t *p_demux) {

    msg_Info(p_demux, "__processFirstMpuFragment::()");

	demux_sys_t *p_sys = p_demux->p_sys;

    MP4_Box_t       *p_ftyp;
    const MP4_Box_t *p_mvhd = NULL;
    const MP4_Box_t *p_mvex = NULL;

    bool      b_enabled_es;

	//TODO - we may need to do this with every MPU
	//

	if( LoadInitFrag( p_demux ) != VLC_SUCCESS )
	           goto error;

//    msg_Info(p_demux, "__processFirstMpuFragment:before MP4_BoxDumpStructure");
//   // MP4_BoxDumpStructure( p_demux->s, p_sys->p_root );

//    msg_Info(p_demux, "__processFirstMpuFragment:before MP4_BoxGet: /moov");
//	p_sys->p_moov = MP4_BoxGet( p_sys->p_root, "/moov" );
//
//    msg_Info(p_demux, "__processFirstMpuFragment:before LoadInitFrag");
//
//	if( LoadInitFrag( p_demux ) != VLC_SUCCESS )
//		goto error;

    msg_Info(p_demux, "__processFirstMpuFragment:before MP4_BoxGet: /ftyp");

	if( ( p_ftyp = MP4_BoxGet( p_sys->p_root, "/ftyp" ) ) )
	{
		switch( BOXDATA(p_ftyp)->i_major_brand )
		{
			case BRAND_isom:
				msg_Dbg( p_demux,
						 "ISO Media (isom) version %d.",
						 BOXDATA(p_ftyp)->i_minor_version );
				break;
			case BRAND_3gp4:
			case BRAND_3gp5:
			case BRAND_3gp6:
			case BRAND_3gp7:
				msg_Dbg( p_demux, "3GPP Media Release: %4.4s",
						 (char *)&BOXDATA(p_ftyp)->i_major_brand );
				break;
			case BRAND_qt__:
				msg_Dbg( p_demux, "Apple QuickTime media" );
				break;
			case BRAND_isml:
				msg_Dbg( p_demux, "PIFF (= isml = fMP4) media" );
				break;
			case BRAND_dash:
				msg_Dbg( p_demux, "DASH Stream" );
				break;
//			case BRAND_M4A:
//				msg_Dbg( p_demux, "iTunes audio" );
//				if( var_InheritBool( p_demux, CFG_PREFIX"m4a-audioonly" ) )
//					p_sys->hacks.es_cat_filters = AUDIO_ES;
//				break;
			default:
				msg_Dbg( p_demux,
						 "unrecognized major media specification (%4.4s).",
						  (char*)&BOXDATA(p_ftyp)->i_major_brand );
				break;
		}
		/* also lookup in compatibility list */
		for(uint32_t i=0; i<BOXDATA(p_ftyp)->i_compatible_brands_count; i++)
		{
			if (BOXDATA(p_ftyp)->i_compatible_brands[i] == BRAND_dash)
			{
				msg_Dbg( p_demux, "DASH Stream" );
			}
			else if (BOXDATA(p_ftyp)->i_compatible_brands[i] == BRAND_smoo)
			{
				msg_Dbg( p_demux, "Handling VLC Smooth Stream" );
			}
		}
	}
	else
	{
		msg_Dbg( p_demux, "file type box missing (assuming ISO Media)" );
	}

    msg_Info(p_demux, "__processFirstMpuFragment:before MP4_BoxGet /moov");

	/* the file need to have one moov box */
	p_sys->p_moov = MP4_BoxGet( p_sys->p_root, "/moov" );
	if( unlikely(!p_sys->p_moov) )
	{
		p_sys->p_moov = MP4_BoxGet( p_sys->p_root, "/foov" );
		if( !p_sys->p_moov )
		{
			msg_Err( p_demux, "MP4 plugin discarded (no moov,foov,moof box)" );
			goto error;
		}
		/* we have a free box as a moov, rename it */
		p_sys->p_moov->i_type = ATOM_moov;
	}

    msg_Info(p_demux, "__processFirstMpuFragment:before MP4_BoxGet /moov/mvhd");

	p_mvhd = MP4_BoxGet( p_sys->p_moov, "mvhd" );
	if( p_mvhd && BOXDATA(p_mvhd) && BOXDATA(p_mvhd)->i_timescale )
	{
		p_sys->i_timescale = BOXDATA(p_mvhd)->i_timescale;
		p_sys->i_moov_duration = p_sys->i_duration = BOXDATA(p_mvhd)->i_duration;
		p_sys->i_cumulated_duration = BOXDATA(p_mvhd)->i_duration;
	}
	else
	{
		msg_Warn( p_demux, "No valid mvhd found" );
		goto error;
	}

//	MP4_Box_t *p_rmra = MP4_BoxGet( p_sys->p_root, "/moov/rmra" );
//	if( p_rmra != NULL && p_demux->p_input_item != NULL )
//	{
//		int        i_count = MP4_BoxCount( p_rmra, "rmda" );
//		int        i;
//
//		msg_Dbg( p_demux, "detected playlist mov file (%d ref)", i_count );
//
//		input_item_t *p_current = p_demux->p_input_item;
//
//		input_item_node_t *p_subitems = input_item_node_Create( p_current );
//
//		for( i = 0; i < i_count; i++ )
//		{
//			MP4_Box_t *p_rdrf = MP4_BoxGet( p_rmra, "rmda[%d]/rdrf", i );
//			char      *psz_ref;
//			uint32_t  i_ref_type;
//
//			if( !p_rdrf || !BOXDATA(p_rdrf) || !( psz_ref = strdup( BOXDATA(p_rdrf)->psz_ref ) ) )
//			{
//				continue;
//			}
//			i_ref_type = BOXDATA(p_rdrf)->i_ref_type;
//
//			msg_Dbg( p_demux, "new ref=`%s' type=%4.4s",
//					 psz_ref, (char*)&i_ref_type );
//
//			if( i_ref_type == VLC_FOURCC( 'u', 'r', 'l', ' ' ) )
//			{
//				if( strstr( psz_ref, "qt5gateQT" ) )
//				{
//					msg_Dbg( p_demux, "ignoring pseudo ref =`%s'", psz_ref );
//					free( psz_ref );
//					continue;
//				}
//				if( !strncmp( psz_ref, "http://", 7 ) ||
//					!strncmp( psz_ref, "rtsp://", 7 ) )
//				{
//					;
//				}
//				else
//				{
//					char *psz_absolute = vlc_uri_resolve( p_demux->psz_url,
//														  psz_ref );
//					msg_Dbg( p_demux, "%d, calling free on psz_ref: %p", __LINE__, (void*)psz_ref);
//
//					free( psz_ref );
//					if( psz_absolute == NULL )
//					{
//						input_item_node_Delete( p_subitems );
//						return VLC_ENOMEM;
//					}
//					psz_ref = psz_absolute;
//				}
//				msg_Dbg( p_demux, "adding ref = `%s'", psz_ref );
//				input_item_t *p_item = input_item_New( psz_ref, NULL );
//				input_item_CopyOptions( p_item, p_current );
//				input_item_node_AppendItem( p_subitems, p_item );
//				input_item_Release( p_item );
//			}
//			else
//			{
//				msg_Err( p_demux, "unknown ref type=%4.4s FIXME (send a bug report)",
//						 (char*)&BOXDATA(p_rdrf)->i_ref_type );
//			}
//			free( psz_ref );
//		}
//
//		/* FIXME: create a stream_filter sub-module for this */
//		if (es_out_Control(p_demux->out, ES_OUT_POST_SUBNODE, p_subitems))
//			input_item_node_Delete(p_subitems);
//	}


	const unsigned i_tracks = MP4_BoxCount( p_sys->p_root, "/moov/trak" );
	if( i_tracks < 1 )
	{
		msg_Err( p_demux, "cannot find any /moov/trak" );
		goto error;
	}
	msg_Dbg( p_demux, "found %u track%c", i_tracks, i_tracks ? 's':' ' );

	if( CreateTracks( p_demux, i_tracks ) != VLC_SUCCESS )
		goto error;

//	/* Search the first chap reference (like quicktime) and
//	 * check that at least 1 stream is enabled */
//	p_sys->p_tref_chap = NULL;
//	b_enabled_es = false;
//	for( unsigned i = 0; i < p_sys->i_tracks; i++ )
//	{
//		MP4_Box_t *p_trak = MP4_BoxGet( p_sys->p_root, "/moov/trak[%d]", i );
//
//		MP4_Box_t *p_tkhd = MP4_BoxGet( p_trak, "tkhd" );
//		if( p_tkhd && BOXDATA(p_tkhd) && (BOXDATA(p_tkhd)->i_flags&MP4_TRACK_ENABLED) )
//			b_enabled_es = true;
//
//		MP4_Box_t *p_chap = MP4_BoxGet( p_trak, "tref/chap", i );
//		if( p_chap && p_chap->data.p_tref_generic &&
//			p_chap->data.p_tref_generic->i_entry_count > 0 && !p_sys->p_tref_chap )
//			p_sys->p_tref_chap = p_chap;
//	}

	/* Set and store metadata */
//	if( (p_sys->p_meta = vlc_meta_New()) )
//		MP4_LoadMeta( p_sys, p_sys->p_meta );
//
	/* now process each track and extract all useful information */
	for( unsigned i = 0; i < p_sys->i_tracks; i++ )
	{
		MP4_Box_t *p_trak = MP4_BoxGet( p_sys->p_root, "/moov/trak[%u]", i );
		MP4_TrackSetup( p_demux, &p_sys->track[i], p_trak, true, !b_enabled_es );

		if( p_sys->track[i].b_ok && !p_sys->track[i].b_chapters_source )
		{
			const char *psz_cat;
			switch( p_sys->track[i].fmt.i_cat )
			{
				case( VIDEO_ES ):
					psz_cat = "video";
					break;
				case( AUDIO_ES ):
					psz_cat = "audio";
					break;
				case( SPU_ES ):
					psz_cat = "subtitle";
					break;

				default:
					psz_cat = "unknown";
					break;
			}

			msg_Dbg( p_demux, "adding track[Id 0x%x] %s (%s) language %s",
					 p_sys->track[i].i_track_ID, psz_cat,
					 p_sys->track[i].b_enable ? "enable":"disable",
					 p_sys->track[i].fmt.psz_language ?
					 p_sys->track[i].fmt.psz_language : "undef" );
		}
		else if( p_sys->track[i].b_ok && p_sys->track[i].b_chapters_source )
		{
			msg_Dbg( p_demux, "using track[Id 0x%x] for chapter language %s",
					 p_sys->track[i].i_track_ID,
					 p_sys->track[i].fmt.psz_language ?
					 p_sys->track[i].fmt.psz_language : "undef" );
		}
		else
		{
			msg_Dbg( p_demux, "ignoring track[Id 0x%x]",
					 p_sys->track[i].i_track_ID );
		}
	}

	p_mvex = MP4_BoxGet( p_sys->p_moov, "mvex" );
	if( p_mvex != NULL )
	{
		const MP4_Box_t *p_mehd = MP4_BoxGet( p_mvex, "mehd");
		if ( p_mehd && BOXDATA(p_mehd) )
		{
			if( BOXDATA(p_mehd)->i_fragment_duration > p_sys->i_duration )
			{
				p_sys->b_fragmented = true;
				p_sys->i_duration = BOXDATA(p_mehd)->i_fragment_duration;
			}
		}

		const MP4_Box_t *p_sidx = MP4_BoxGet( p_sys->p_root, "sidx");
		if( p_sidx )
			p_sys->b_fragmented = true;

		if ( p_sys->b_seekable )
		{
			if( !p_sys->b_fragmented /* as unknown */ )
			{
				/* Probe remaining to check if there's really fragments
				   or if that file is just ready to append fragments */
				ProbeFragments( p_demux, (p_sys->i_duration == 0), &p_sys->b_fragmented );
			}

			if( vlc_stream_Seek( p_sys->s_frag, p_sys->p_moov->i_pos ) != VLC_SUCCESS )
				goto error;
		}
		else /* Handle as fragmented by default as we can't see moof */
		{
			p_sys->context.p_fragment_atom = p_sys->p_moov;
			p_sys->context.i_current_box_type = ATOM_moov;
			p_sys->b_fragmented = true;
		}
	}

// don't stea our demux callback
//	if( p_sys->b_fragmented )
//	{
//		p_demux->pf_demux = DemuxFrag;
//		msg_Dbg( p_demux, "Set Fragmented demux mode" );
//	}
//
//	if( !p_sys->b_seekable && p_demux->pf_demux == Demux )
//	{
//		msg_Warn( p_demux, "MP4 plugin discarded (not seekable)" );
//		goto error;
//	}
//
//	if( p_sys->i_tracks > 1 && !p_sys->b_fastseekable )
//	{
//		vlc_tick_t i_max_continuity;
//		bool b_flat;
//		MP4_GetInterleaving( p_demux, &i_max_continuity, &b_flat );
//		if( b_flat )
//			msg_Warn( p_demux, "that media doesn't look interleaved, will need to seek");
//		else if( i_max_continuity > DEMUX_TRACK_MAX_PRELOAD )
//			msg_Warn( p_demux, "that media doesn't look properly interleaved, will need to seek");
//	}

	/* */
	//LoadChapter( p_demux );

	p_sys->asfpacketsys.p_demux = p_demux;
	p_sys->asfpacketsys.pi_preroll = &p_sys->i_preroll;
	p_sys->asfpacketsys.pi_preroll_start = &p_sys->i_preroll_start;
	p_sys->asfpacketsys.pf_doskip = NULL;
	p_sys->asfpacketsys.pf_send = MP4ASF_Send;
	p_sys->asfpacketsys.pf_gettrackinfo = MP4ASF_GetTrackInfo;
	p_sys->asfpacketsys.pf_updatetime = NULL;
	p_sys->asfpacketsys.pf_setaspectratio = NULL;

	return VLC_SUCCESS;

/** anyone else this would be fatal, but we just skip until our next start of MPU **/
	error:
    	msg_Warn( p_demux, "__processFirstMpuFragment - error, returning success");
    	return VLC_SUCCESS;

    	//'t reset stream position from probing" );
//
//		if( vlc_stream_Tell( p_demux->s ) > 0 )
//	    {
//	        if( vlc_stream_Seek( p_demux->s, 0 ) != VLC_SUCCESS )
//	            msg_Warn( p_demux, "Can't reset stream position from probing" );
//	    }
//
//	    Close( p_this );
//
//	    return VLC_EGENERIC;

}


/**
 * Destroys the MMTP-demuxer, no-op for now
 */
static void Close( vlc_object_t *p_this )
{
    demux_t *p_demux = (demux_t*)p_this;
	demux_sys_t *p_sys = p_demux->p_sys;

    if(p_sys->demux_frag_thread) {
    	vlc_cancel(p_sys->demux_frag_thread);
    	vlc_join(p_sys->demux_frag_thread, NULL);
    	p_sys->demux_frag_thread = NULL;
    }

    //close the fifo once our consumer is shutdown
    if(p_sys->s_frag) {
    	vlc_stream_fifo_Close(p_sys->s_frag);
    }
    p_sys->s_frag = NULL;

    vlc_sem_destroy(&p_sys->demux_frag_new_data_semaphore);

    if(p_sys) {
    	free(p_sys);
    }
    p_demux->p_sys = NULL;

    msg_Info(p_demux, "mmtp_demuxer.close()");
}

/**
 *
 * Un-encapsulate MMTP into type,
 * 	0x00 -> process payload as MPU, re-constituting MFU accordingly
 *
 *
 *
 *
     * dont rely on stream read, use vlc_stream_block for
    block_t *block = block_Alloc( MAX_UDP_BLOCKSIZE );
    if( unlikely(block == NULL) )
        return -1;

    int rd = vlc_stream_Read( p_demux->s, block->p_buffer, MAX_UDP_BLOCKSIZE );
    if ( rd <= 0 )
    {
        block_Release( block );
        return rd;
    }
    block->i_buffer = rd;

    size_t wr = sout_AccessOutWrite( out, block );
    if( wr != (size_t)rd )
    {
        msg_Err( p_demux, "cannot write data" );
        return -1;
    }
    return 1;

    **/
//messy
void* extract(uint8_t *bufPosPtr, uint8_t *dest, int size) {
	for(int i=0; i < size; i++) {
		dest[i] = *bufPosPtr++;
	}
	return bufPosPtr;
}

/**
 *
 * mmtp demuxer,
 *
 * use p_sys->s for udp,
 * use p_sys->s_frag for fragmented mp4 demux / decoding
 *
 */

static int Demux( demux_t *p_demux )
{
	demux_sys_t *p_sys = p_demux->p_sys;

//    msg_Info(p_demux, "mmtp_demuxer.demux()");

    /* Get a new MMTP packet, use p_demux->s as the blocking reference and 1514 as the max mtu in udp.c*/
    //	vlc_stream_Block will try and fill MAX_MTU_SIZE instead of relying on the
    //    MMTP udp frame size

    uint8_t *raw_buf = malloc( MAX_MMTP_SIZE );
    uint8_t *buf = raw_buf; //use buf to walk thru bytes in extract method without touching rawBuf

    ssize_t mmtp_raw_packet_size = -1;
    //readPartial still reads a block_chain
  //  if( !( mmtp_raw_packet_size = vlc_stream_ReadPartial( p_demux->s, (void*)rawBuf, MAX_MMTP_SIZE ) ) )
	block_t *read_block;

    if( !( read_block = vlc_stream_ReadBlock( p_demux->s) ) )
    {
		msg_Err( p_demux, "mmtp_demuxer - access request returned null!");
		return VLC_DEMUXER_SUCCESS;
	}

    mmtp_raw_packet_size =  read_block->i_buffer;
   // msg_Info(p_demux, "mmtp_demuxer: vlc_stream_readblock size is: %d", read_block->i_buffer);

	if( mmtp_raw_packet_size > MAX_MMTP_SIZE || mmtp_raw_packet_size < MIN_MMTP_SIZE) {
		msg_Err( p_demux, "mmtp_demuxer - size from UDP was under/over heureis/max, dropping %d bytes", mmtp_raw_packet_size);
		free(raw_buf); //only free raw_buf
		return VLC_DEMUXER_SUCCESS;
	}

	block_ChainExtract(read_block, raw_buf, MAX_MMTP_SIZE);

	uint8_t mmtp_packet_preamble[20];

	//msg_Warn( p_demux, "buf pos before extract is: %p", (void *)buf);
	buf = extract(buf, mmtp_packet_preamble, 20);
	//msg_Warn( p_demux, "buf pos is now %p vs %p", new_buf_pos, (void *)buf);

//		msg_Dbg( p_demux, "raw packet size is: %d, first byte: 0x%X", mmtp_raw_packet_size, mmtp_packet_preamble[0]);

	if(false) {
		char buffer[(mmtp_raw_packet_size * 3)+1];

		for(int i=0; i < mmtp_raw_packet_size; i++) {
			if(i>1 && (i+1)%8 == 0) {
				snprintf(buffer + (i*3), 4, "%02X\n", raw_buf[i]);
			} else {
				snprintf(buffer + (i*3), 4, "%02X ", raw_buf[i]);
			}
		}
		msg_Info(p_demux, "raw packet payload is:\n%s", buffer);
	}

	uint8_t mmtp_packet_version = (mmtp_packet_preamble[0] & 0xC0) >> 6;
	uint8_t packet_counter_flag = (mmtp_packet_preamble[0] & 0x20) >> 5;
	uint8_t fec_type = (mmtp_packet_preamble[0] & 0x18) >> 3;

	//v=0 vs v=1 attributes in the first 2 octets
	uint8_t  mmtp_payload_type = 0;
	uint8_t  mmtp_header_extension_flag = 0;
	uint8_t  mmtp_rap_flag = 0;
	uint8_t  mmtp_qos_flag = 0;

	uint8_t  mmtp_flow_identifer_flag = 0;
	uint8_t  mmtp_flow_extension_flag = 0;

	uint8_t  mmtp_header_compression = 0;
	uint8_t	 mmtp_indicator_ref_header_flag = 0;

	uint8_t mmtp_type_of_bitrate = 0;
	uint8_t mmtp_delay_sensitivity = 0;
	uint8_t mmtp_transmission_priority = 0;

	uint16_t mmtp_header_extension_type = 0;
	uint16_t mmtp_header_extension_length = 0;

	uint8_t flow_label = 0;

	if(mmtp_packet_version == 0x00) {
		//after fec_type, with v=0, next bitmask is 0x4 >>2
		//0000 0010
		//V0CF E-XR
		mmtp_header_extension_flag = (mmtp_packet_preamble[0] & 0x2) >> 1;
		mmtp_rap_flag = mmtp_packet_preamble[0] & 0x1;

		//6 bits right aligned
		mmtp_payload_type = mmtp_packet_preamble[1] & 0x3f;
		if(mmtp_header_extension_flag & 0x1) {
			mmtp_header_extension_type = (mmtp_packet_preamble[16]) << 8 | mmtp_packet_preamble[17];
			mmtp_header_extension_length = (mmtp_packet_preamble[18]) << 8 | mmtp_packet_preamble[19];
		} else {
			//walk back by 4 bytes
			buf-=4;
		}
	} else if(mmtp_packet_version == 0x01) {
		//bitmask is 0000 00
		//0000 0100
		//V1CF EXRQ
		mmtp_header_extension_flag = mmtp_packet_preamble[0] & 0x4 >> 2; //X
		mmtp_rap_flag = (mmtp_packet_preamble[0] & 0x2) >> 1;				//RAP
		mmtp_qos_flag = mmtp_packet_preamble[0] & 0x1;					//QOS
		//0000 0000
		//FEBI TYPE
		//4 bits for preamble right aligned

		mmtp_flow_identifer_flag = ((mmtp_packet_preamble[1]) & 0x80) >> 7;			//F
		mmtp_flow_extension_flag = ((mmtp_packet_preamble[1]) & 0x40) >> 6;			//E
		mmtp_header_compression = ((mmtp_packet_preamble[1]) &  0x20) >> 5; 		//B
		mmtp_indicator_ref_header_flag = ((mmtp_packet_preamble[1]) & 0x10) >> 4;	//I

		mmtp_payload_type = mmtp_packet_preamble[1] & 0xF;

		//TB 2 bits
		mmtp_type_of_bitrate = ((mmtp_packet_preamble[16] & 0x40) >> 6) | ((mmtp_packet_preamble[16] & 0x20) >> 5);

		//DS 3 bits
		mmtp_delay_sensitivity = ((mmtp_packet_preamble[16] & 0x10) >> 4) | ((mmtp_packet_preamble[16] & 0x8) >> 3) | ((mmtp_packet_preamble[16] & 0x4) >> 2);

		//TP 3 bits
		mmtp_transmission_priority =(( mmtp_packet_preamble[16] & 0x02) << 2) | ((mmtp_packet_preamble[16] & 0x1) << 1) | ((mmtp_packet_preamble[17] & 0x80) >>7);

		flow_label = mmtp_packet_preamble[17] & 0x7f;

		//header extension is offset by 2 bytes in v=1, so an additional block chain read is needed to get extension length
		if(mmtp_header_extension_flag & 0x1) {
			mmtp_header_extension_type = (mmtp_packet_preamble[18] << 8) | mmtp_packet_preamble[19];

			msg_Warn( p_demux, "mmtp_demuxer - dping mmtp_header_extension_length_bytes: %d",  mmtp_header_extension_type);

			uint8_t mmtp_header_extension_length_bytes[2];
			buf = extract(buf, mmtp_header_extension_length_bytes, 2);

			mmtp_header_extension_length = mmtp_header_extension_length_bytes[0] << 8 | mmtp_header_extension_length_bytes[1];
		} else {
			//walk back our buf position by 2 bytes to start for payload ddata
			buf-=2;

		}
	} else {
		msg_Warn( p_demux, "mmtp_demuxer - unknown packet version of 0x%X", mmtp_packet_version);
		free( raw_buf );

		return VLC_DEMUXER_SUCCESS;
	}

	uint16_t mmtp_packet_id			= mmtp_packet_preamble[2]  << 8  | mmtp_packet_preamble[3];
	uint32_t mmtp_timestamp 		= mmtp_packet_preamble[4]  << 24 | mmtp_packet_preamble[5]  << 16 | mmtp_packet_preamble[6]   << 8 | mmtp_packet_preamble[7];
	uint32_t packet_sequence_number = mmtp_packet_preamble[8]  << 24 | mmtp_packet_preamble[9]  << 16 | mmtp_packet_preamble[10]  << 8 | mmtp_packet_preamble[11];
	uint32_t packet_counter 		= mmtp_packet_preamble[12] << 24 | mmtp_packet_preamble[13] << 16 | mmtp_packet_preamble[14]  << 8 | mmtp_packet_preamble[15];

	msg_Dbg( p_demux, "packet version: %d, payload_type: 0x%X, packet_id 0x%hu, timestamp: 0x%X, packet_sequence_number: 0x%X, packet_counter: 0x%X", mmtp_packet_version,
			mmtp_payload_type, mmtp_packet_id, mmtp_timestamp, packet_sequence_number, packet_counter);

	//if our header extension length is set, then block extract the header extension length, adn we should be at our payload data
	uint8_t *mmtp_header_extension_value = NULL;

	if(mmtp_header_extension_flag & 0x1) {
		//clamp mmtp_header_extension_length
		mmtp_header_extension_length = MIN(mmtp_header_extension_length, 2^16);
		msg_Warn( p_demux, "mmtp_demuxer - doing  mmtp_header_extension_flag with size: %d", mmtp_header_extension_length);

		mmtp_header_extension_value = malloc(mmtp_header_extension_length);
		//read the header extension value up to the extension length field 2^16
		buf = extract(buf, &mmtp_header_extension_value, mmtp_header_extension_length);
	}

	if(mmtp_payload_type == 0) {
		//pull the mpu and frag iformation

		uint8_t mpu_payload_length_block[2];
		uint16_t mpu_payload_length = 0;

//			msg_Warn( p_demux, "buf pos before mpu_payload_length extract is: %p", (void *)buf);
		buf = extract(buf, &mpu_payload_length_block, 2);
		mpu_payload_length = (mpu_payload_length_block[0] << 8) | mpu_payload_length_block[1];
		//msg_Dbg( p_demux, "mmtp_demuxer - doing mpu_payload_length: %hu (0x%X 0x%X)",  mpu_payload_length, mpu_payload_length_block[0], mpu_payload_length_block[1]);

		uint8_t mpu_fragmentation_info;
		//msg_Warn( p_demux, "buf pos before extract is: %p", (void *)buf);
		buf = extract(buf, &mpu_fragmentation_info, 1);

		uint8_t mpu_fragment_type = (mpu_fragmentation_info & 0xF0) >> 4;
		uint8_t mpu_timed_flag = (mpu_fragmentation_info & 0x8) >> 3;
		uint8_t mpu_fragmentation_indicator = (mpu_fragmentation_info & 0x6) >> 1;
		uint8_t mpu_aggregation_flag = (mpu_fragmentation_info & 0x1);

		uint8_t mpu_fragmentation_counter;
//			msg_Warn( p_demux, "buf pos before extract is: %p", (void *)buf);
		buf = extract(buf, &mpu_fragmentation_counter, 1);

		//re-fanagle
		uint8_t mpu_sequence_number_block[4];
		uint32_t mpu_sequence_number;
//			msg_Warn( p_demux, "buf pos before extract is: %p", (void *)buf);

		buf = extract(buf, &mpu_sequence_number_block, 4);
		mpu_sequence_number = (mpu_sequence_number_block[0] << 24)  | (mpu_sequence_number_block[1] <<16) | (mpu_sequence_number_block[2] << 8) | (mpu_sequence_number_block[3]);
		msg_Dbg( p_demux, "mmtp_demuxer - mmtp packet: mpu_payload_length: %hu (0x%X 0x%X), mpu_fragmentation_counter: %d, mpu_sequence_number: %d",  mpu_payload_length, mpu_payload_length_block[0], mpu_payload_length_block[1], mpu_fragmentation_counter, mpu_sequence_number);

		uint16_t data_unit_length = 0;
		int remainingPacketLen = -1;

		//todo - if FEC_type != 0, parse out source_FEC_payload_ID trailing bits...
		do {
			//pull out aggregate packets data unit length
			int to_read_packet_length = -1;
			//mpu_fragment_type

			//only read DU length if mpu_aggregation_flag=1
			if(mpu_aggregation_flag) {
				uint8_t data_unit_length_block[2];
				buf = extract(buf, &data_unit_length_block, 2);
				data_unit_length = (data_unit_length_block[0] << 8) | (data_unit_length_block[1]);
				to_read_packet_length = data_unit_length;
				msg_Info(p_demux, "%d:mpu data unit size: mpu_aggregation_flag:1, to_read_packet_length: %d", __LINE__, to_read_packet_length);

			} else {
				to_read_packet_length = mmtp_raw_packet_size - (buf-raw_buf);
				msg_Info(p_demux, "%d:mpu data unit size: mpu_aggregation_flag:0, raw packet size: %d, buf: %p, raw_buf: %p, to_read_packet_length: %d", __LINE__, mmtp_raw_packet_size, buf, raw_buf, to_read_packet_length);
			}

			if(mpu_fragment_type != 0x2) {
				//read our packet length just as a mpu metadata fragment or movie fragment metadata
				//read our packet length without any mfu
				block_t *tmp_mpu_fragment = block_Alloc(to_read_packet_length);
//					msg_Info(p_demux, "%d::creating tmp_mpu_fragment, setting block_t->i_buffer to: %d", __LINE__, to_read_packet_length);

				buf = extract(buf, tmp_mpu_fragment->p_buffer, to_read_packet_length);
				tmp_mpu_fragment->i_buffer = to_read_packet_length;

				processMpuPacket(p_demux, mmtp_packet_id, mpu_sequence_number, 0, 0, mpu_fragment_type, mpu_fragmentation_indicator, tmp_mpu_fragment);
				remainingPacketLen = mmtp_raw_packet_size - (buf - raw_buf);
				//this should only be non-zero if mpu_aggregration_flag=1
				msg_Info(p_demux, "%d::mpu_fragment_type: %hu, remainingPacketLen: %d", __LINE__, mpu_fragment_type, remainingPacketLen);

			} else {
				//mfu's have time and un-timed additional DU headers, so recalc to_read_packet_len after doing extract
				//we use the du_header field
				//parse data unit header here based upon mpu timed flag
				uint32_t movie_fragment_sequence_number = 0;
				uint32_t sample_number = 0;
				uint32_t offset = 0;

				/**
				* MFU mpu_fragmentation_indicator==1's are prefixed by the following box, need to remove
				*
				aligned(8) class MMTHSample {
				   unsigned int(32) sequence_number;
				   if (is_timed) {

					//interior block is 152 bits, or 19 bytes
					  signed int(8) trackrefindex;
					  unsigned int(32) movie_fragment_sequence_number
					  unsigned int(32) samplenumber;
					  unsigned int(8)  priority;
					  unsigned int(8)  dependency_counter;
					  unsigned int(32) offset;
					  unsigned int(32) length;
					//end interior block

					  multiLayerInfo();
				} else {
						//additional 2 bytes to chomp for non timed delivery
					  unsigned int(16) item_ID;
				   }
				}

				aligned(8) class multiLayerInfo extends Box("muli") {
				   bit(1) multilayer_flag;
				   bit(7) reserved0;
				   if (multilayer_flag==1) {
					   //32 bits
					  bit(3) dependency_id;
					  bit(1) depth_flag;
					  bit(4) reserved1;
					  bit(3) temporal_id;
					  bit(1) reserved2;
					  bit(4) quality_id;
					  bit(6) priority_id;
				   }  bit(10) view_id;
				   else{
					   //16bits
					  bit(6) layer_id;
					  bit(3) temporal_id;
					  bit(7) reserved3;
				} }
				*/

				uint8_t mmthsample_len;
				uint8_t mmthsample_sequence_number[4];

				if(mpu_timed_flag) {
					//112 bits in aggregate, 14 bytes
					uint8_t timed_mfu_block[14];
					buf = extract(buf, timed_mfu_block, 14);

					movie_fragment_sequence_number = (timed_mfu_block[0] << 24) | (timed_mfu_block[1] << 16) | (timed_mfu_block[2]  << 8) | (timed_mfu_block[3]);
					sample_number				   = (timed_mfu_block[4] << 24) | (timed_mfu_block[5] << 16) | (timed_mfu_block[6]  << 8) | (timed_mfu_block[7]);
					offset     					   = (timed_mfu_block[8] << 24) | (timed_mfu_block[9] << 16) | (timed_mfu_block[10] << 8) | (timed_mfu_block[11]);
					uint8_t priority 						= timed_mfu_block[12];
					uint8_t dep_counter						= timed_mfu_block[13];

					//parse out mmthsample block if this is our first fragment or we are a complete fragment,
					if(mpu_fragmentation_indicator == 0 || mpu_fragmentation_indicator == 1) {

						//MMTHSample does not subclass box...
						//buf = extract(buf, &mmthsample_len, 1);
						buf = extract(buf, mmthsample_sequence_number, 4);

						uint8_t mmthsample_timed_block[19];
						buf = extract(buf, mmthsample_timed_block, 19);

						//read multilayerinfo
						uint8_t multilayerinfo_box_length[4];
						uint8_t multilayerinfo_box_name[4];
						uint8_t multilayer_flag;

						buf = extract(buf, multilayerinfo_box_length, 4);
						buf = extract(buf, multilayerinfo_box_name, 4);

						buf = extract(buf, &multilayer_flag, 1);

						int is_multilayer = (multilayer_flag >> 7) & 0x01;
						//if MSB is 1, then read multilevel struct, otherwise just pull layer info...
						if(is_multilayer) {
							uint8_t multilayer_data_block[4];
							buf = extract(buf, multilayer_data_block, 4);

						} else {
							uint8_t multilayer_layer_id_temporal_id[2];
							buf = extract(buf, multilayer_layer_id_temporal_id, 2);
						}

						msg_Info(p_demux, "mpu mode (0x02), timed MFU, mpu_fragmentation_indicator: %d, movie_fragment_seq_num: %zu, sample_num: %zu, offset: %zu, pri: %d, dep_counter: %d, multilayer: %d",
								mpu_fragmentation_indicator, movie_fragment_sequence_number, sample_number, offset, priority, dep_counter, is_multilayer);
					} else {
						msg_Info(p_demux, "mpu mode (0x02), timed MFU, mpu_fragmentation_indicator: %d, movie_fragment_seq_num: %zu, sample_num: %zu, offset: %zu, pri: %d, dep_counter: %d",
								mpu_fragmentation_indicator, movie_fragment_sequence_number, sample_number, offset, priority, dep_counter);
					}

					//end mfu box read

					to_read_packet_length = mmtp_raw_packet_size - (buf - raw_buf);
				} else {
					uint8_t non_timed_mfu_block[4];
					uint32_t non_timed_mfu_item_id;
					//only 32 bits
					buf = extract(buf, non_timed_mfu_block, 4);
					non_timed_mfu_item_id = (non_timed_mfu_block[0] << 24) | (non_timed_mfu_block[1] << 16) | (non_timed_mfu_block[2] << 8) | non_timed_mfu_block[3];

					if(mpu_fragmentation_indicator == 1) {
						//MMTHSample does not subclass box...
						//buf = extract(buf, &mmthsample_len, 1);

						buf = extract(buf, mmthsample_sequence_number, 4);

						uint8_t mmthsample_item_id[2];
						buf = extract(buf, mmthsample_sequence_number, 2);
						//end reading of mmthsample box
					}

					msg_Info(p_demux, "mpu mode (0x02), non-timed MFU, item_id is: %zu", non_timed_mfu_item_id);
					to_read_packet_length = mmtp_raw_packet_size - (buf - raw_buf);
				}

				//msg_Dbg( p_demux, "before reading fragment packet:  %p", (void*)p_sys->p_mpu_block);

				block_t *tmp_mpu_fragment = block_Alloc(to_read_packet_length);
				//msg_Info(p_demux, "%d::creating tmp_mpu_fragment, setting block_t->i_buffer to: %d", __LINE__, to_read_packet_length);

				buf = extract(buf, tmp_mpu_fragment->p_buffer, to_read_packet_length);
				tmp_mpu_fragment->i_buffer = to_read_packet_length;

				//send off only the CLEAN mdat payload from our MFU
				processMpuPacket(p_demux, mmtp_packet_id, mpu_sequence_number, sample_number, offset, mpu_fragment_type, mpu_fragmentation_indicator, tmp_mpu_fragment);
				remainingPacketLen = mmtp_raw_packet_size - (buf - raw_buf);

			}

		} while(mpu_aggregation_flag && remainingPacketLen>0);
	}

	//mp4 fragmented demuxer (DemuxFrag) will happen on demux_frag_thread
	if(raw_buf)
		free(raw_buf);

	return VLC_DEMUXER_SUCCESS;
}

/**
 * only flush out mpu packet when our mpq_sequence_id changes
 */

void processMpuPacket(demux_t* p_demux, uint16_t mmtp_packet_id, uint32_t mpu_sequence_number, uint32_t mpu_sample_number, uint32_t mpu_offset, uint8_t mpu_fragment_type, uint8_t mpu_fragmentation_indicator, block_t *tmp_mpu_fragment ) {

	demux_sys_t *p_sys = p_demux->p_sys;
    //if we have a new packet id, assume we can send the current payload off
	if(mmtp_packet_id != 35) {
		msg_Info(p_demux, "processMpuPacket - returning because mmtp_packet_id!=35, val is %hu", mmtp_packet_id);
		return;
	}

	//if we have not completed at least one mpu yet, bail until we get the start of a mpu_fragment_type of MPU metadata for ftyp / moov
	if(p_sys->last_mpu_sequence_number == -1 && mpu_fragment_type != 0) {
		msg_Info(p_demux, "processMpuPacket - mmtp_packet_id: %hu, mpu_sequence_number: %u, bailing because last_mpu_sequence_number is still -1", mmtp_packet_id, mpu_sequence_number);

		return;
	}

	//mpu_sequence_number

	msg_Info(p_demux, "processMpuPacket - mmtp_packet_id: %hu, mpu_sequence_number: %u, sample: %u, offset: %u, mpu_fragment_type: %hu, mpu_fragmentation_indication: %u, p_mpu_block is: %p",
											mmtp_packet_id, mpu_sequence_number, mpu_sample_number, mpu_offset, mpu_fragment_type, mpu_fragmentation_indicator, (void*)p_sys->p_mpu_block);

	//only flush out and process the MPU if our sequence number has incremented
	//TODO - check mmpu box for is_complete for mpu_sequence_number, use or conditional as mpu_seuqence_number is uint32...
	if((p_sys->last_mpu_sequence_number == -1 && mpu_fragment_type == 0) || (mpu_sequence_number > abs(p_sys->last_mpu_sequence_number))) {

		//reset our __MFU_COUNTER for debugging
		__MFU_COUNTER = 1;
		//flush out our pending p_mpu block
		//contains full ftyp, moov, etc...

		if(p_sys->p_mpu_block && p_sys->p_mpu_block->i_buffer > 0) {
			msg_Info(p_demux, "processMpuPacket ******* FINALIZING MFU ******** to ISOBMFF - last_mpu_sequence_number: %hu, mpu_sequence_number: %hu, pending p_mpu_block is: %zu", p_sys->last_mpu_sequence_number, mpu_sequence_number, p_sys->p_mpu_block->i_buffer);
			block_t* mpu = block_ChainGather(p_sys->p_mpu_block);
			vlc_stream_fifo_Queue(p_sys->s_frag, block_Duplicate(mpu));

			block_Release(p_sys->p_mpu_block);
			p_sys->p_mpu_block = NULL;

		//	msg_Info(p_demux, "processMpuPacket ********** FINALIZING MFU ********** mpu block i_buffer is: %zu length\nfirst 32 bits are: 0x%x 0x%x 0x%x 0x%x\nnext  32 bits are: 0x%x 0x%x 0x%x 0x%x",mpu->i_buffer, mpu->p_buffer[0], mpu->p_buffer[1], mpu->p_buffer[2], mpu->p_buffer[3], mpu->p_buffer[4], mpu->p_buffer[5], mpu->p_buffer[6], mpu->p_buffer[7]);
			dumpMpu(p_demux, mpu);

			//stream_t *orig_stream = p_demux->s;
		//	msg_Info(p_demux, "processMpuPacket ********** FINALIZING MFU ********** cloning demux_t");
/** fix me to not memcpy **/

//			demux_t *mp4_demux = malloc(sizeof(demux_t));
//			memcpy((void *)mp4_demux, (void *)p_demux, sizeof(demux_t));

		//swap out p_demux->s with re-encapsulated payload
		//	msg_Info(p_demux, "processMpuPacket ********** FINALIZING MFU ************ cloning vlc_stream_MemoryNew");


		//old way
			//p_sys->s_frag = vlc_stream_MemoryNew( p_sys->obj, mpu->p_buffer, mpu->i_buffer, true);

	//		msg_Info(p_demux, "processMpuPacket ******* FINALIZING MFU ******** __processFirstMpuFragment - before");
	//todo, see if this is needed.....

			if(!p_sys->has_processed_ftype_moov) {
				__processFirstMpuFragment(p_demux);
				p_sys->has_processed_ftype_moov = 1;
			}

			//signal the demux_frag for isobmff processing
			vlc_sem_post(&p_sys->demux_frag_new_data_semaphore);


			//		msg_Info(p_demux, "processMpuPacket ******* FINALIZING MFU ******** __processFirstMpuFragment - complete");

			//__mp4_Demux(mp4_demux);
			//use the fragmented demuxer
		//	DemuxFrag(p_demux);

			//clear out block buffer

		//	msg_Info(p_demux, "processMpuPacket ******* FINALIZING MFU ******** block_Release complete");

		} else {
			if(p_sys->p_mpu_block) {
				msg_Warn(p_demux, "processMpuPacket - sequence number change, but p_sys->p_mpu_block->i_buffer is 0, last_mpu_sequence_number: %u, mpu_sequence_number: %u, pending p_mpu_block is: %zu", p_sys->last_mpu_sequence_number, mpu_sequence_number, p_sys->p_mpu_block->i_buffer);
			} else {
				msg_Warn(p_demux, "processMpuPacket - sequence number change, but p_sys->p_mpu_block is not allocated, last_mpu_sequence_number: %u, mpu_sequence_number: %u", p_sys->last_mpu_sequence_number, mpu_sequence_number);
			}
		}

		p_sys->last_mpu_sequence_number = mpu_sequence_number;
	}

	if(tmp_mpu_fragment) {

//		msg_Info(p_demux, "processMpuPacket - before p_mpu_block with mpu_sequence_number: %hu, mpu_sample_number: %hu, mpu_fragment_type: %d, mpu_fragmentation_indicatior: %d, p_mpu_block is: %p,", mpu_sequence_number, mpu_sample_number, mpu_fragment_type, mpu_fragmentation_indicator, (void*)p_sys->p_mpu_block);
		dumpMfu(p_demux, tmp_mpu_fragment);

		block_ChainAppend(&p_sys->p_mpu_block, block_Duplicate(tmp_mpu_fragment));
		p_sys->p_mpu_block = block_ChainGather(p_sys->p_mpu_block);

//		msg_Info(p_demux, "processMpuPacket - NEW p_mpu_block with mpu_sample_number: %hu, mpu_fragment_type: %d, mpu_fragmentation_indicatior: %d, p_mpu_block is: %p, size is now: %d", mpu_sample_number, mpu_fragment_type, mpu_fragmentation_indicator, (void*)p_sys->p_mpu_block, p_sys->p_mpu_block->i_buffer );

		p_sys->last_mpu_fragment_type = mpu_fragment_type;
	}
}


static int __MPU_COUNTER = 0;
void dumpMpu(demux_t *p_demux, block_t *mpu) {

	demux_sys_t *p_sys = p_demux->p_sys;

//	msg_Info(p_demux, "::dumpMpu ******* file dump counter_id is: %d", __MPU_COUNTER);
	//dumping block_t mpu->i_buffer is: %zu, p_buffer[0] is:\n%c", mpu->i_buffer, mpu->p_buffer[0]);
	if(true) {
		char *myFilePathName = malloc(sizeof(char)*20);
		memset(myFilePathName, 0, 20);
		int pos=0;

		strncat(myFilePathName, "mpu/", 4);
		pos = strlen(myFilePathName);
		itoa(p_sys->last_mpu_sequence_number, myFilePathName+pos, 10);

	//	msg_Info(p_demux, "::dumpMfu ******* file dump __MPU_COUNTER is: %d, file: %s", __MPU_COUNTER-1, myFilePathName);

		FILE *f = fopen(myFilePathName, "w");
		if(!f) {
			msg_Err(p_demux, "::dumpMpu ******* UNABLE TO OPEN FILE %s", myFilePathName);
			return;
		}

		for(int i=0; i < mpu->i_buffer; i++) {
			fputc(mpu->p_buffer[i], f);
		}
		fclose(f);
	}

	if(false) {
		char buffer[mpu->i_buffer * 5+1]; //0x00 |
										//12345
		for(int i=0; i < mpu->i_buffer; i++) {
			if(i>0 && (i+1)%32 == 0) {
				snprintf(buffer + (i*3), 4, "%02X\n ", mpu->p_buffer[i]);
			} else if(i>0 && (i+1)%8 == 0) {
				snprintf(buffer + (i*3), 4, "%02X\t ", mpu->p_buffer[i]);
			} else {
				snprintf(buffer + (i*3), 4, "%02X ", mpu->p_buffer[i]);
			}
		//	msg_Info(mp4_demux, "%02X ", mpu->p_buffer[i]);
		}
//		msg_Info(p_demux, "::dumpMpu ******* dumping block_t mpu->i_buffer is: %zu, p_buffer is:\n%s", mpu->i_buffer, buffer);

	}


}



/** todo:
 * change this to proper samples
 */
void dumpMfu(demux_t *p_demux, block_t *mpu) {
	demux_sys_t *p_sys = p_demux->p_sys;

	//dumping block_t mpu->i_buffer is: %zu, p_buffer[0] is:\n%c", mpu->i_buffer, mpu->p_buffer[0]);
	if(true) {
		char *myFilePathName = malloc(sizeof(char)*20);
		memset(myFilePathName, 0, 20);
		int pos = 0;

		strncat(myFilePathName, "mfu/", 4);
		pos = strlen(myFilePathName);
		itoa(p_sys->last_mpu_sequence_number, myFilePathName+pos, 10);
		pos = strlen(myFilePathName);

		myFilePathName[pos] = '-';
		myFilePathName[pos+1] = '\0';
		pos = strlen(myFilePathName);
		itoa(__MFU_COUNTER++, myFilePathName+pos, 10);

//		msg_Info(p_demux, "::dumpMfu ******* file dump __MPU_COUNTER is: %d, __MFU_COUNTER is: %d, file: %s", __MPU_COUNTER, __MFU_COUNTER-1, myFilePathName);

		FILE *f = fopen(myFilePathName, "w");
		if(!f) {
			msg_Err(p_demux, "::dumpMfu ******* UNABLE TO OPEN FILE %s", myFilePathName);
			return;
		}

		for(int i=0; i < mpu->i_buffer; i++) {
			fputc(mpu->p_buffer[i], f);
		}
		fclose(f);
	}

	if(false) {
		msg_Info(p_demux, "::dumpMfu ******* file dump counter_id is: %d", __MFU_COUNTER);

		char buffer[mpu->i_buffer * 5+1]; //0x00 |
										//12345
		for(int i=0; i < mpu->i_buffer; i++) {
			if(i>0 && (i+1)%32 == 0) {
				snprintf(buffer + (i*3), 4, "%02X\n ", mpu->p_buffer[i]);
			} else if(i>0 && (i+1)%8 == 0) {
				snprintf(buffer + (i*3), 4, "%02X\t ", mpu->p_buffer[i]);
			} else {
				snprintf(buffer + (i*3), 4, "%02X ", mpu->p_buffer[i]);
			}
		//	msg_Info(mp4_demux, "%02X ", mpu->p_buffer[i]);
		}
		msg_Info(p_demux, "::dumpMpu ******* dumping block_t mpu->i_buffer is: %zu, p_buffer is:\n%s", mpu->i_buffer, buffer);

	}


}

/** todo - add
 * DEMUX_SET_GROUP_DEFAULT ?
 *
 * DEMUX_FILTER_DISABLE
 *
 */
static int Control( demux_t *p_demux, int i_query, va_list args )
{
   // msg_Info(p_demux, "control: query is: %d", i_query);

    bool *pb;
    unsigned *flags;

    switch ( i_query )
    {
    	case DEMUX_CAN_SEEK:
        case DEMUX_CAN_PAUSE:
        case DEMUX_CAN_CONTROL_PACE:
            pb = va_arg ( args, bool* );
            *pb = false;
            break;


        case DEMUX_GET_PTS_DELAY:
        	return VLC_EGENERIC;

        case DEMUX_GET_META:
        case DEMUX_GET_SIGNAL:
        case DEMUX_GET_TITLE:
        case DEMUX_GET_SEEKPOINT:
        case DEMUX_GET_TITLE_INFO:
        case DEMUX_IS_PLAYLIST:
        	return VLC_EGENERIC;

        case DEMUX_GET_LENGTH:
			*va_arg ( args, vlc_tick_t * ) = 0;

			//	vlc_tick_from_sec( p_sys->frames_total * p_sys->frame_rate_denom / p_sys->frame_rate_num );
			break;

		case DEMUX_GET_TIME:
			*va_arg( args, vlc_tick_t * ) = 0;
			break;

        case DEMUX_GET_ATTACHMENTS:
        	return VLC_EGENERIC;
        	break;
    }

    return VLC_SUCCESS; //demux_vaControlHelper( p_demux->s, 0, -1, 0, 1, i_query, args );

}


/*** copy paste warning from libmp4/mp4.c
 *
 *
 */







/* Helpers */

static int64_t MP4_rescale( int64_t i_value, uint32_t i_timescale, uint32_t i_newscale )
{
    if( i_timescale == i_newscale )
        return i_value;

    if( i_value <= INT64_MAX / i_newscale )
        return i_value * i_newscale / i_timescale;

    /* overflow */
    int64_t q = i_value / i_timescale;
    int64_t r = i_value % i_timescale;
    return q * i_newscale + r * i_newscale / i_timescale;
}

static vlc_tick_t MP4_rescale_mtime( int64_t i_value, uint32_t i_timescale )
{
    return MP4_rescale(i_value, i_timescale, CLOCK_FREQ);
}

static int64_t MP4_rescale_qtime( vlc_tick_t i_value, uint32_t i_timescale )
{
    return MP4_rescale(i_value, CLOCK_FREQ, i_timescale);
}

static uint32_t stream_ReadU32( stream_t *s, void *p_read, uint32_t i_toread )
{
    ssize_t i_return = 0;
    if ( i_toread > INT32_MAX )
    {
        i_return = vlc_stream_Read( s, p_read, (size_t) INT32_MAX );
        if ( i_return < INT32_MAX )
            return i_return;
        else
            i_toread -= INT32_MAX;
    }
    i_return += vlc_stream_Read( s, (uint8_t *)p_read + i_return, (size_t) i_toread );
    return i_return;
}

static MP4_Box_t * MP4_GetTrexByTrackID( MP4_Box_t *p_moov, const uint32_t i_id )
{
    if(!p_moov)
        return NULL;
    MP4_Box_t *p_trex = MP4_BoxGet( p_moov, "mvex/trex" );
    while( p_trex )
    {
        if ( p_trex->i_type == ATOM_trex &&
             BOXDATA(p_trex) && BOXDATA(p_trex)->i_track_ID == i_id )
                break;
        else
            p_trex = p_trex->p_next;
    }
    return p_trex;
}

/**
 * Return the track identified by tid
 */
static mp4_track_t *MP4_GetTrackByTrackID( demux_t *p_demux, const uint32_t tid )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    mp4_track_t *ret = NULL;
    for( unsigned i = 0; i < p_sys->i_tracks; i++ )
    {
        ret = &p_sys->track[i];
        if( ret->i_track_ID == tid )
            return ret;
    }
    return NULL;
}

static MP4_Box_t * MP4_GetTrakByTrackID( MP4_Box_t *p_moov, const uint32_t i_id )
{
    MP4_Box_t *p_trak = MP4_BoxGet( p_moov, "trak" );
    MP4_Box_t *p_tkhd;
    while( p_trak )
    {
        if( p_trak->i_type == ATOM_trak &&
            (p_tkhd = MP4_BoxGet( p_trak, "tkhd" )) && BOXDATA(p_tkhd) &&
            BOXDATA(p_tkhd)->i_track_ID == i_id )
                break;
        else
            p_trak = p_trak->p_next;
    }
    return p_trak;
}

static MP4_Box_t * MP4_GetTrafByTrackID( MP4_Box_t *p_moof, const uint32_t i_id )
{
    MP4_Box_t *p_traf = MP4_BoxGet( p_moof, "traf" );
    MP4_Box_t *p_tfhd;
    while( p_traf )
    {
        if( p_traf->i_type == ATOM_traf &&
            (p_tfhd = MP4_BoxGet( p_traf, "tfhd" )) && BOXDATA(p_tfhd) &&
            BOXDATA(p_tfhd)->i_track_ID == i_id )
                break;
        else
            p_traf = p_traf->p_next;
    }
    return p_traf;
}

static es_out_id_t * MP4_AddTrackES( es_out_t *out, mp4_track_t *p_track )
{
    es_out_id_t *p_es = es_out_Add( out, &p_track->fmt );
    /* Force SPU which isn't selected/defaulted */
    if( p_track->fmt.i_cat == SPU_ES && p_es && p_track->b_forced_spu )
        es_out_Control( out, ES_OUT_SET_ES_DEFAULT, p_es );

    return p_es;
}

/* Return time in microsecond of a track */
static inline vlc_tick_t MP4_TrackGetDTS( demux_t *p_demux, mp4_track_t *p_track )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const mp4_chunk_t *p_chunk = &p_track->chunk[p_track->i_chunk];

    unsigned int i_index = 0;
    unsigned int i_sample = p_track->i_sample - p_chunk->i_sample_first;
    int64_t sdts = p_chunk->i_first_dts;

    while( i_sample > 0 && i_index < p_chunk->i_entries_dts )
    {
        if( i_sample > p_chunk->p_sample_count_dts[i_index] )
        {
            sdts += p_chunk->p_sample_count_dts[i_index] *
                p_chunk->p_sample_delta_dts[i_index];
            i_sample -= p_chunk->p_sample_count_dts[i_index];
            i_index++;
        }
        else
        {
            sdts += i_sample * p_chunk->p_sample_delta_dts[i_index];
            break;
        }
    }

    vlc_tick_t i_dts = MP4_rescale_mtime( sdts, p_track->i_timescale );

    /* now handle elst */
    if( p_track->p_elst && p_track->BOXDATA(p_elst)->i_entry_count )
    {
        MP4_Box_data_elst_t *elst = p_track->BOXDATA(p_elst);

        /* convert to offset */
        if( ( elst->i_media_rate_integer[p_track->i_elst] > 0 ||
              elst->i_media_rate_fraction[p_track->i_elst] > 0 ) &&
            elst->i_media_time[p_track->i_elst] > 0 )
        {
            i_dts -= MP4_rescale_mtime( elst->i_media_time[p_track->i_elst], p_track->i_timescale );
        }

        /* add i_elst_time */
        i_dts += MP4_rescale_mtime( p_track->i_elst_time, p_sys->i_timescale );

        if( i_dts < 0 ) i_dts = 0;
    }

    return i_dts;
}

static inline bool MP4_TrackGetPTSDelta( demux_t *p_demux, mp4_track_t *p_track,
                                         vlc_tick_t *pi_delta )
{
    VLC_UNUSED( p_demux );
    mp4_chunk_t *ck = &p_track->chunk[p_track->i_chunk];

    unsigned int i_index = 0;
    unsigned int i_sample = p_track->i_sample - ck->i_sample_first;

    if( ck->p_sample_count_pts == NULL || ck->p_sample_offset_pts == NULL )
        return false;

    for( i_index = 0; i_index < ck->i_entries_pts ; i_index++ )
    {
        if( i_sample < ck->p_sample_count_pts[i_index] )
        {
            *pi_delta = MP4_rescale_mtime( ck->p_sample_offset_pts[i_index],
                                           p_track->i_timescale );
            return true;
        }

        i_sample -= ck->p_sample_count_pts[i_index];
    }
    return false;
}

static inline vlc_tick_t MP4_GetSamplesDuration( demux_t *p_demux, mp4_track_t *p_track,
                                              unsigned i_nb_samples )
{
    VLC_UNUSED( p_demux );

    const mp4_chunk_t *p_chunk = &p_track->chunk[p_track->i_chunk];
    stime_t i_duration = 0;

    /* Forward to right index, and set remaining count in that index */
    unsigned i_index = 0;
    unsigned i_remain = 0;
    for( unsigned i = p_chunk->i_sample_first;
         i<p_track->i_sample && i_index < p_chunk->i_entries_dts; )
    {
        if( p_track->i_sample - i >= p_chunk->p_sample_count_dts[i_index] )
        {
            i += p_chunk->p_sample_count_dts[i_index];
            i_index++;
        }
        else
        {
            i_remain = p_track->i_sample - i;
            break;
        }
    }

    /* Compute total duration from all samples from index */
    while( i_nb_samples > 0 && i_index < p_chunk->i_entries_dts )
    {
        if( i_nb_samples >= p_chunk->p_sample_count_dts[i_index] - i_remain )
        {
            i_duration += (p_chunk->p_sample_count_dts[i_index] - i_remain) *
                          (int64_t) p_chunk->p_sample_delta_dts[i_index];
            i_nb_samples -= (p_chunk->p_sample_count_dts[i_index] - i_remain);
            i_index++;
            i_remain = 0;
        }
        else
        {
            i_duration += i_nb_samples * p_chunk->p_sample_delta_dts[i_index];
            break;
        }
    }

    return MP4_rescale_mtime( i_duration, p_track->i_timescale );
}

static inline vlc_tick_t MP4_GetMoviePTS(demux_sys_t *p_sys )
{
    return p_sys->i_nztime;
}


static int LoadInitFrag( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    /* Load all boxes ( except raw data ) */
    MP4_Box_t *p_root = MP4_BoxGetRoot( p_sys->s_frag );
    if( p_root == NULL || !MP4_BoxGet( p_root, "/moov" ) )
    {
		msg_Dbg( p_demux, "%d, calling free on p_root: %p", __LINE__, (void*)p_root);

        MP4_BoxFree( p_root );
        goto LoadInitFragError;
    }

    p_sys->p_root = p_root;

    return VLC_SUCCESS;

LoadInitFragError:
    msg_Warn( p_demux, "MP4 plugin discarded (not a valid initialization chunk)" );
    return VLC_SUCCESS;
}

static int CreateTracks( demux_t *p_demux, unsigned i_tracks )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if( SIZE_MAX / i_tracks < sizeof(mp4_track_t) )
        return VLC_EGENERIC;

    p_sys->track = vlc_alloc( i_tracks, sizeof(mp4_track_t)  );
    if( p_sys->track == NULL )
        return VLC_ENOMEM;
    p_sys->i_tracks = i_tracks;

    for( unsigned i=0; i<i_tracks; i++ )
        MP4_TrackInit( &p_sys->track[i] );

    return VLC_SUCCESS;
}

static block_t * MP4_EIA608_Convert( block_t * p_block )
{
    /* Rebuild codec data from encap */
    size_t i_copied = 0;
    size_t i_remaining = __MIN(p_block->i_buffer, INT64_MAX / 3);
    uint32_t i_bytes = 0;
    block_t *p_newblock;

    /* always need at least 10 bytes (atom size+header+1pair)*/
    if ( i_remaining < 10 ||
         !(i_bytes = GetDWBE(p_block->p_buffer)) ||
         (i_bytes > i_remaining) ||
         memcmp("cdat", &p_block->p_buffer[4], 4) ||
         !(p_newblock = block_Alloc( i_remaining * 3 - 8 )) )
    {
        p_block->i_buffer = 0;
        return p_block;
    }

    uint8_t *p_write = p_newblock->p_buffer;
    uint8_t *p_read = &p_block->p_buffer[8];
    i_bytes -= 8;
    i_remaining -= 8;

    do
    {
        p_write[i_copied++] = CC_PKT_BYTE0(0); /* cc1 == field 0 */
        p_write[i_copied++] = p_read[0];
        p_write[i_copied++] = p_read[1];
        p_read += 2;
        i_bytes -= 2;
        i_remaining -= 2;
    } while( i_bytes >= 2 );

    /* cdt2 is optional */
    if ( i_remaining >= 10 &&
         (i_bytes = GetDWBE(p_read)) &&
         (i_bytes <= i_remaining) &&
         !memcmp("cdt2", &p_read[4], 4) )
    {
        p_read += 8;
        i_bytes -= 8;
        i_remaining -= 8;
        do
        {
            p_write[i_copied++] = CC_PKT_BYTE0(0); /* cc1 == field 0 */
            p_write[i_copied++] = p_read[0];
            p_write[i_copied++] = p_read[1];
            p_read += 2;
            i_bytes -= 2;
        } while( i_bytes >= 2 );
    }

    p_newblock->i_pts = p_block->i_dts;
    p_newblock->i_buffer = i_copied;
    p_newblock->i_flags = BLOCK_FLAG_TYPE_P;
    block_Release( p_block );

    return p_newblock;
}

static uint32_t MP4_TrackGetRunSeq( mp4_track_t *p_track )
{
    if( p_track->i_chunk_count > 0 )
        return p_track->chunk[p_track->i_chunk].i_virtual_run_number;
    return 0;
}

/* Analyzes chunks to find max interleave length
 * sets flat flag if no interleaving is in use */
static void MP4_GetInterleaving( demux_t *p_demux, vlc_tick_t *pi_max_contiguous, bool *pb_flat )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    *pi_max_contiguous = 0;
    *pb_flat = true;

    /* Find first recorded chunk */
    mp4_track_t *tk = NULL;
    uint64_t i_duration = 0;
    for( unsigned i=0; i < p_sys->i_tracks; i++ )
    {
        mp4_track_t *cur = &p_sys->track[i];
        if( !cur->i_chunk_count )
            continue;

        if( tk == NULL || cur->chunk[0].i_offset < tk->chunk[0].i_offset )
            tk = cur;
    }

    for( ; tk != NULL; )
    {
        i_duration += tk->chunk[tk->i_chunk].i_duration;
        tk->i_chunk++;

        /* Find next chunk in data order */
        mp4_track_t *nexttk = NULL;
        for( unsigned i=0; i < p_sys->i_tracks; i++ )
        {
            mp4_track_t *cur = &p_sys->track[i];
            if( cur->i_chunk == cur->i_chunk_count )
                continue;

            if( nexttk == NULL ||
                cur->chunk[cur->i_chunk].i_offset < nexttk->chunk[nexttk->i_chunk].i_offset )
                nexttk = cur;
        }

        /* copy previous run */
        if( nexttk && nexttk->i_chunk > 0 )
            nexttk->chunk[nexttk->i_chunk].i_virtual_run_number =
                    nexttk->chunk[nexttk->i_chunk - 1].i_virtual_run_number;

        if( tk != nexttk )
        {
            vlc_tick_t i_dur = MP4_rescale_mtime( i_duration, tk->i_timescale );
            if( i_dur > *pi_max_contiguous )
                *pi_max_contiguous = i_dur;
            i_duration = 0;

            if( tk->i_chunk != tk->i_chunk_count )
                *pb_flat = false;

            if( nexttk && nexttk->i_chunk > 0 ) /* new run number */
                nexttk->chunk[nexttk->i_chunk].i_virtual_run_number++;
        }

        tk = nexttk;
    }

    /* reset */
    for( unsigned i=0; i < p_sys->i_tracks; i++ )
        p_sys->track[i].i_chunk = 0;
}

static block_t * MP4_Block_Convert( demux_t *p_demux, const mp4_track_t *p_track, block_t *p_block )
{
    /* might have some encap */
    if( p_track->fmt.i_cat == SPU_ES )
    {
        switch( p_track->fmt.i_codec )
        {
            case VLC_CODEC_WEBVTT:
            case VLC_CODEC_TTML:
            case VLC_CODEC_TX3G:
            case VLC_CODEC_SPU:
            case VLC_CODEC_SUBT:
            /* accept as-is */
            break;
            case VLC_CODEC_CEA608:
                p_block = MP4_EIA608_Convert( p_block );
            break;
        default:
            p_block->i_buffer = 0;
            break;
        }
    }
    else if( p_track->fmt.i_codec == VLC_CODEC_AV1 )
    {
        p_block = AV1_Unpack_Sample( p_block );
    }
    else if( p_track->fmt.i_original_fourcc == ATOM_rrtp )
    {
        p_block = MP4_RTPHint_Convert( p_demux, p_block, p_track->fmt.i_codec );
    }

    return p_block;
}

static void MP4_Block_Send( demux_t *p_demux, mp4_track_t *p_track, block_t *p_block )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    p_block = MP4_Block_Convert( p_demux, p_track, p_block );
    if( p_block == NULL )
        return;

    if ( p_track->b_chans_reorder )
    {
        aout_ChannelReorder( p_block->p_buffer, p_block->i_buffer,
                             p_track->fmt.audio.i_channels,
                             p_track->rgi_chans_reordering,
                             p_track->fmt.i_codec );
    }

    p_block->i_flags |= p_track->i_block_flags;
    if( p_track->i_next_block_flags )
    {
        p_block->i_flags |= p_track->i_next_block_flags;
        p_track->i_next_block_flags = 0;
    }

    /* ASF packets in mov */
    if( p_track->p_asf )
    {
        /* Fake a new stream from MP4 block */
        stream_t *p_stream = p_sys->s_frag;
        p_sys->s_frag = vlc_stream_MemoryNew( p_demux, p_block->p_buffer, p_block->i_buffer, true );
        if ( p_sys->s_frag )
        {
            p_track->i_dts_backup = p_block->i_dts;
            p_track->i_pts_backup = p_block->i_pts;
            /* And demux it as ASF packet */
            DemuxASFPacket( &p_sys->asfpacketsys, p_block->i_buffer, p_block->i_buffer );
            vlc_stream_Delete(p_sys->s_frag);
        }
        block_Release(p_block);
        p_sys->s_frag = p_stream;
    }
    else
        es_out_Send( p_demux->out, p_track->p_es, p_block );
}

int  OpenHEIF ( vlc_object_t * );
void CloseHEIF( vlc_object_t * );


const unsigned int SAMPLEHEADERSIZE = 4;
const unsigned int RTPPACKETSIZE = 12;
const unsigned int CONSTRUCTORSIZE = 16;

/*******************************************************************************
 * MP4_RTPHintToFrame: converts RTP Reception Hint Track sample to H.264 frame
 *******************************************************************************/
static block_t * MP4_RTPHintToFrame( demux_t *p_demux, block_t *p_block, uint32_t packetcount )
{
    uint8_t *p_slice = p_block->p_buffer + SAMPLEHEADERSIZE;
    block_t *p_newblock = NULL;
    size_t i_payload = 0;

    if( p_block->i_buffer < SAMPLEHEADERSIZE + RTPPACKETSIZE + CONSTRUCTORSIZE )
    {
        msg_Err( p_demux, "Sample not large enough for necessary structs");
        block_Release( p_block );
        return NULL;
    }

    for( uint32_t i = 0; i < packetcount; ++i )
    {
        if( (size_t)(p_slice - p_block->p_buffer) + RTPPACKETSIZE + CONSTRUCTORSIZE > p_block->i_buffer )
            goto error;

        /* skip RTP header in sample. Could be used to detect packet losses */
        p_slice += RTPPACKETSIZE;

        mp4_rtpsampleconstructor_t sample_cons;

        sample_cons.type =                      p_slice[0];
        sample_cons.trackrefindex =             p_slice[1];
        sample_cons.length =          GetWBE(  &p_slice[2] );
        sample_cons.samplenumber =    GetDWBE( &p_slice[4] );
        sample_cons.sampleoffset =    GetDWBE( &p_slice[8] );
        sample_cons.bytesperblock =   GetWBE(  &p_slice[12] );
        sample_cons.samplesperblock = GetWBE(  &p_slice[14] );

        /* skip packet constructor */
        p_slice += CONSTRUCTORSIZE;

        /* check that is RTPsampleconstructor, referencing itself and no weird audio stuff */
        if( sample_cons.type != 2||sample_cons.trackrefindex != -1
            ||sample_cons.samplesperblock != 1||sample_cons.bytesperblock != 1 )
        {
            msg_Err(p_demux, "Unhandled constructor in RTP Reception Hint Track. Type:%u", sample_cons.type);
            goto error;
        }

        /* slice doesn't fit in buffer */
        if( sample_cons.sampleoffset + sample_cons.length > p_block->i_buffer)
        {
            msg_Err(p_demux, "Sample buffer is smaller than sample" );
            goto error;
        }

        block_t *p_realloc = ( p_newblock ) ?
                             block_Realloc( p_newblock, 0, i_payload + sample_cons.length + 4 ):
                             block_Alloc( i_payload + sample_cons.length + 4 );
        if( !p_realloc )
            goto error;

        p_newblock = p_realloc;
        uint8_t *p_dst = &p_newblock->p_buffer[i_payload];

        const uint8_t* p_src = p_block->p_buffer + sample_cons.sampleoffset;
        uint8_t i_type = (*p_src) & ((1<<5)-1);

        const uint8_t synccode[4] = { 0, 0, 0, 1 };
        if( memcmp( p_src, synccode, 4 ) )
        {
            if( i_type == 7 || i_type == 8 )
                *p_dst++=0;

            p_dst[0] = 0;
            p_dst[1] = 0;
            p_dst[2] = 1;
            p_dst += 3;
        }

        memcpy( p_dst, p_src, sample_cons.length );
        p_dst += sample_cons.length;

        i_payload = p_dst - p_newblock->p_buffer;
    }

    block_Release( p_block );
    if( p_newblock )
        p_newblock->i_buffer = i_payload;
    return p_newblock;

error:
    block_Release( p_block );
    if( p_newblock )
        block_Release( p_newblock );
    return NULL;
}

/* RTP Reception Hint Track */
static block_t * MP4_RTPHint_Convert( demux_t *p_demux, block_t *p_block, vlc_fourcc_t i_codec )
{
    block_t *p_converted = NULL;
    if( p_block->i_buffer < 2 )
    {
        block_Release( p_block );
        return NULL;
    }

    /* number of RTP packets contained in this sample */
    const uint16_t i_packets = GetWBE( p_block->p_buffer );
    if( i_packets <= 1 || i_codec != VLC_CODEC_H264 )
    {
        const size_t i_skip = SAMPLEHEADERSIZE + i_packets * ( RTPPACKETSIZE + CONSTRUCTORSIZE );
        if( i_packets == 1 && i_skip < p_block->i_buffer )
        {
            p_block->p_buffer += i_skip;
            p_converted = p_block;
        }
        else
        {
            block_Release( p_block );
        }
    }
    else
    {
        p_converted = MP4_RTPHintToFrame( p_demux, p_block, i_packets );
    }

    return p_converted;
}

/*****************************************************************************
 * Demux: read packet and send them to decoders
 *****************************************************************************
 * TODO check for newly selected track (ie audio upt to now )
 *****************************************************************************/
static int DemuxTrack( demux_t *p_demux, mp4_track_t *tk, uint64_t i_readpos,
                       vlc_tick_t i_max_preload )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    uint32_t i_nb_samples = 0;
    uint32_t i_samplessize = 0;

    if( !tk->b_ok || tk->i_sample >= tk->i_sample_count )
        return VLC_DEMUXER_EOS;

    if( tk->b_chapters_source )
        return VLC_DEMUXER_SUCCESS;

    uint32_t i_run_seq = MP4_TrackGetRunSeq( tk );
    vlc_tick_t i_current_nzdts = MP4_TrackGetDTS( p_demux, tk );
    const vlc_tick_t i_demux_max_nzdts =i_max_preload < INVALID_PRELOAD
                                    ? i_current_nzdts + i_max_preload
                                    : INT64_MAX;

    for( ; i_demux_max_nzdts >= i_current_nzdts; )
    {
        if( tk->i_sample >= tk->i_sample_count )
            return VLC_DEMUXER_EOS;

#if 0
        msg_Dbg( p_demux, "tk(%i)=%"PRId64" mv=%"PRId64" pos=%"PRIu64, tk->i_track_ID,
                 MP4_TrackGetDTS( p_demux, tk ),
                 MP4_GetMoviePTS( p_demux->p_sys ), i_readpos );
#endif

        i_samplessize = MP4_TrackGetReadSize( tk, &i_nb_samples );
        if( i_samplessize > 0 )
        {
            block_t *p_block;
            vlc_tick_t i_delta;

            if( vlc_stream_Tell( p_sys->s_frag ) != i_readpos )
            {
                if( MP4_Seek( p_sys->s_frag, i_readpos ) != VLC_SUCCESS )
                {
                    msg_Warn( p_demux, "track[0x%x] will be disabled (eof?)"
                                       ": Failed to seek to %"PRIu64,
                              tk->i_track_ID, i_readpos );
                    MP4_TrackSelect( p_demux, tk, false );
                    goto end;
                }
            }

            /* now read pes */
            if( !(p_block = vlc_stream_Block( p_sys->s_frag, i_samplessize )) )
            {
                msg_Warn( p_demux, "track[0x%x] will be disabled (eof?)"
                                   ": Failed to read %d bytes sample at %"PRIu64,
                          tk->i_track_ID, i_samplessize, i_readpos );
                MP4_TrackSelect( p_demux, tk, false );
                goto end;
            }

            /* !important! Ensure clock is set before sending data */
            if( p_sys->i_pcr == VLC_TICK_INVALID )
            {
                es_out_SetPCR( p_demux->out, VLC_TICK_0 + i_current_nzdts );
                p_sys->i_pcr = VLC_TICK_0 + i_current_nzdts;
            }

            /* dts */
            p_block->i_dts = VLC_TICK_0 + i_current_nzdts;
            /* pts */
            if( MP4_TrackGetPTSDelta( p_demux, tk, &i_delta ) )
                p_block->i_pts = p_block->i_dts + i_delta;
            else if( tk->fmt.i_cat != VIDEO_ES )
                p_block->i_pts = p_block->i_dts;
            else
                p_block->i_pts = VLC_TICK_INVALID;

            p_block->i_length = MP4_GetSamplesDuration( p_demux, tk, i_nb_samples );

            MP4_Block_Send( p_demux, tk, p_block );
        }

        /* Next sample */
        if ( i_nb_samples ) /* sample size could be 0, need to go fwd. see return */
            MP4_TrackNextSample( p_demux, tk, i_nb_samples );

        uint32_t i_next_run_seq = MP4_TrackGetRunSeq( tk );
        if( i_next_run_seq != i_run_seq )
            break;

        i_current_nzdts = MP4_TrackGetDTS( p_demux, tk );
        i_readpos = MP4_TrackGetPos( tk );
    }

    return VLC_DEMUXER_SUCCESS;

end:
    return VLC_DEMUXER_EGENERIC;
}

static int DemuxMoov( demux_t *p_demux )
{
	demux_sys_t *p_sys = p_demux->p_sys;

    unsigned int i_track;

    msg_Info(p_demux, "%d:DemuxMoov, track count: %d", __LINE__, p_sys->i_tracks);

    /* check for newly selected/unselected track */
    for( i_track = 0; i_track < p_sys->i_tracks; i_track++ )
    {
        mp4_track_t *tk = &p_sys->track[i_track];
        bool b = true;

        if( !tk->b_ok || tk->b_chapters_source ||
            ( tk->b_selected && tk->i_sample >= tk->i_sample_count ) )
        {
            continue;
        }

        if( p_sys->b_seekable )
            es_out_Control( p_demux->out, ES_OUT_GET_ES_STATE, tk->p_es, &b );

        if( tk->b_selected && !b )
        {
            msg_Info(p_demux, "%d:DemuxMoov, before MP4_TrackSelect with track_id: %d", __LINE__, i_track);

            MP4_TrackSelect( p_demux, tk, false );
        }
        else if( !tk->b_selected && b)
        {
            msg_Info(p_demux, "%d:DemuxMoov, before MP4_TrackSeek with track_id: %d, no seeking", __LINE__, i_track);

           // MP4_TrackSeek( p_demux, tk, MP4_GetMoviePTS( p_sys ) );
        }
    }

    const vlc_tick_t i_nztime = MP4_GetMoviePTS( p_sys );

    /* We demux/set pcr, even without selected tracks, (empty edits, ...) */
    if( p_sys->i_pcr != VLC_TICK_INVALID /* not after a seek */ )
    {
        bool b_eof = true;
        for( i_track = 0; i_track < p_sys->i_tracks; i_track++ )
        {
            mp4_track_t *tk = &p_sys->track[i_track];
            if( !tk->b_ok || tk->b_chapters_source || tk->i_sample >= tk->i_sample_count )
                continue;
            /* Test for EOF on each track (samples count, edit list) */
            b_eof &= ( i_nztime > MP4_TrackGetDTS( p_demux, tk ) );
        }
        if( b_eof ) {
            msg_Info(p_demux, "%d:DemuxMoov, got b_eof track_id: %d", __LINE__, i_track);

            return VLC_DEMUXER_EOS;
        }
    }

    const vlc_tick_t i_max_preload = ( p_sys->b_fastseekable ) ? 0 : ( p_sys->b_seekable ) ? DEMUX_TRACK_MAX_PRELOAD : INVALID_PRELOAD;
    int i_status;
    /* demux up to increment amount of data on every track, or just set pcr if empty data */
    for( ;; )
    {
        mp4_track_t *tk = NULL;
        i_status = VLC_DEMUXER_EOS;

        /* First pass, find any track within our target increment, ordered by position */
        for( i_track = 0; i_track < p_sys->i_tracks; i_track++ )
        {
            mp4_track_t *tk_tmp = &p_sys->track[i_track];
            if( !tk_tmp->b_ok || tk_tmp->b_chapters_source ||
                tk_tmp->i_sample >= tk_tmp->i_sample_count ||
                (!tk_tmp->b_selected && p_sys->b_seekable) )
                continue;

            /* At least still have data to demux on this or next turns */
            i_status = VLC_DEMUXER_SUCCESS;

            if ( MP4_TrackGetDTS( p_demux, tk_tmp ) <= i_nztime + DEMUX_INCREMENT )
            {
                if( tk == NULL || MP4_TrackGetPos( tk_tmp ) < MP4_TrackGetPos( tk ) )
                    tk = tk_tmp;
            }
        }

        if( tk )
        {
            /* Second pass, refine and find any best candidate having a chunk pos closer than
             * current candidate (avoids seeks when increment falls between the 2) from
             * current position, but within extended interleave time */
            for( i_track = 0; i_max_preload != 0 && i_track < p_sys->i_tracks; i_track++ )
            {
                mp4_track_t *tk_tmp = &p_sys->track[i_track];
                if( tk_tmp == tk ||
                    !tk_tmp->b_ok || tk_tmp->b_chapters_source ||
                   (!tk_tmp->b_selected && p_sys->b_seekable) ||
                    tk_tmp->i_sample >= tk_tmp->i_sample_count )
                    continue;

                vlc_tick_t i_nzdts = MP4_TrackGetDTS( p_demux, tk_tmp );
                if ( i_nzdts <= i_nztime + DEMUX_TRACK_MAX_PRELOAD )
                {
                    /* Found a better candidate to avoid seeking */
                    if( MP4_TrackGetPos( tk_tmp ) < MP4_TrackGetPos( tk ) )
                        tk = tk_tmp;
                    /* Note: previous candidate will be repicked on next loop */
                }
            }

            uint64_t i_pos = MP4_TrackGetPos( tk );
            msg_Info(p_demux, "%d:DemuxMoov, before DemuxTrack: i_pos: %d", __LINE__, i_pos);

            int i_ret = DemuxTrack( p_demux, tk, i_pos, i_max_preload );

            if( i_ret == VLC_DEMUXER_SUCCESS )
                i_status = VLC_DEMUXER_SUCCESS;
        }

        if( i_status != VLC_DEMUXER_SUCCESS || !tk )
            break;
    }

    p_sys->i_nztime += DEMUX_INCREMENT;
    if( p_sys->i_pcr != VLC_TICK_INVALID )
    {
        p_sys->i_pcr = VLC_TICK_0 + p_sys->i_nztime;
        es_out_SetPCR( p_demux->out, p_sys->i_pcr );
    }

    /* */
    MP4_UpdateSeekpoint( p_demux, i_nztime + DEMUX_INCREMENT );

    return i_status;
}


static int __mp4_Demux( demux_t *p_demux )
{
    msg_Info(p_demux, "__mp4_Demux:()");

    demux_sys_t *p_sys = p_demux->p_sys;
//jdj-2018-12-31 - dont care you're de-fragmented as a MPU
//    assert( ! p_sys->b_fragmented );

    msg_Info(p_demux, "__mp4_Demux:before DemuxMoov");

    int i_status = DemuxMoov( p_demux );
    msg_Info(p_demux, "__mp4_Demux:after DemuxMoov, i_status is: %d", i_status);

    if( i_status == VLC_DEMUXER_EOS )
        i_status = VLC_DEMUXER_EOF;

    return i_status;
}

static void MP4_UpdateSeekpoint( demux_t *p_demux, vlc_tick_t i_time )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    int i;
    if( !p_sys->p_title )
        return;
    for( i = 0; i < p_sys->p_title->i_seekpoint; i++ )
    {
        if( i_time < p_sys->p_title->seekpoint[i]->i_time_offset )
            break;
    }
    i--;

    if( i != p_sys->i_seekpoint && i >= 0 )
    {
        p_sys->i_seekpoint = i;
        p_sys->seekpoint_changed = true;
    }
}
/*****************************************************************************
 * Seek: Go to i_date
******************************************************************************/
static int Seek( demux_t *p_demux, vlc_tick_t i_date, bool b_accurate )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    unsigned int i_track;

    /* Now for each stream try to go to this time */
    vlc_tick_t i_start = i_date;
    for( i_track = 0; i_track < p_sys->i_tracks; i_track++ )
    {
        mp4_track_t *tk = &p_sys->track[i_track];
        /* FIXME: we should find the lowest time from tracks with indexes.
           considering only video for now */
        if( tk->fmt.i_cat != VIDEO_ES )
            continue;
        if( MP4_TrackSeek( p_demux, tk, i_date ) == VLC_SUCCESS )
        {
            vlc_tick_t i_seeked = MP4_TrackGetDTS( p_demux, tk );
            if( i_seeked < i_start )
                i_start = i_seeked;
        }
    }

    msg_Dbg( p_demux, "seeking with %"PRId64 "ms %s", MS_FROM_VLC_TICK(i_date - i_start),
            !b_accurate ? "alignment" : "preroll (use input-fast-seek to avoid)" );

    for( i_track = 0; i_track < p_sys->i_tracks; i_track++ )
    {
        mp4_track_t *tk = &p_sys->track[i_track];
        tk->i_next_block_flags |= BLOCK_FLAG_DISCONTINUITY;
        if( tk->fmt.i_cat == VIDEO_ES )
            continue;
        MP4_TrackSeek( p_demux, tk, i_start );
    }

    MP4_UpdateSeekpoint( p_demux, i_date );
    MP4ASF_ResetFrames( p_sys );
    /* update global time */
    p_sys->i_nztime = i_start;
    p_sys->i_pcr  = VLC_TICK_INVALID;

    if( b_accurate )
        es_out_Control( p_demux->out, ES_OUT_SET_NEXT_DISPLAY_TIME, i_date );

    return VLC_SUCCESS;
}

static int FragPrepareChunk( demux_t *p_demux, MP4_Box_t *p_moof,
                             MP4_Box_t *p_sidx, stime_t i_moof_time, bool b_discontinuity )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if( b_discontinuity )
    {
        for( unsigned i=0; i<p_sys->i_tracks; i++ )
            p_sys->track[i].context.b_resync_time_offset = true;
    }

    if( FragCreateTrunIndex( p_demux, p_moof, p_sidx, i_moof_time ) == VLC_SUCCESS )
    {
        for( unsigned i=0; i<p_sys->i_tracks; i++ )
        {
            mp4_track_t *p_track = &p_sys->track[i];
            if( p_track->context.runs.i_count )
            {
                const mp4_run_t *p_run = &p_track->context.runs.p_array[0];
                p_track->context.i_trun_sample_pos = p_run->i_offset;
                p_track->context.i_trun_sample = 0;
                p_track->i_time = p_run->i_first_dts;
            }
        }
        return VLC_SUCCESS;
    }

    return VLC_EGENERIC;
}

static vlc_tick_t FragGetDemuxTimeFromTracksTime( demux_sys_t *p_sys )
{
    vlc_tick_t i_time = INT64_MAX;
    for( unsigned int i = 0; i < p_sys->i_tracks; i++ )
    {
        if( p_sys->track[i].context.runs.i_count == 0 )
            continue;
        vlc_tick_t i_ttime = MP4_rescale_mtime( p_sys->track[i].i_time,
                                             p_sys->track[i].i_timescale );
        i_time = __MIN( i_time, i_ttime );
    }
    return i_time;
}

static uint32_t FragGetMoofSequenceNumber( MP4_Box_t *p_moof )
{
    const MP4_Box_t *p_mfhd = MP4_BoxGet( p_moof, "mfhd" );
    if( p_mfhd && BOXDATA(p_mfhd) )
        return BOXDATA(p_mfhd)->i_sequence_number;
    return 0;
}

static int FragSeekLoadFragment( demux_t *p_demux, uint32_t i_moox, stime_t i_moox_time )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    MP4_Box_t *p_moox;

    if( i_moox == ATOM_moov )
    {
        p_moox = p_sys->p_moov;
    }
    else
    {
        const uint8_t *p_peek;
        if( vlc_stream_Peek( p_sys->s_frag, &p_peek, 8 ) != 8 )
            return VLC_EGENERIC;

        if( ATOM_moof != VLC_FOURCC( p_peek[4], p_peek[5], p_peek[6], p_peek[7] ) )
            return VLC_EGENERIC;

        MP4_Box_t *p_vroot = MP4_BoxGetNextChunk( p_sys->s_frag );
        if(!p_vroot)
            return VLC_EGENERIC;
        p_moox = MP4_BoxExtract( &p_vroot->p_first, ATOM_moof );
        MP4_BoxFree( p_vroot );

        if(!p_moox)
            return VLC_EGENERIC;
    }

    FragResetContext( p_sys );

    /* map context */
    p_sys->context.p_fragment_atom = p_moox;
    p_sys->context.i_current_box_type = i_moox;

    if( i_moox == ATOM_moof )
    {
        FragPrepareChunk( p_demux, p_moox, NULL, i_moox_time, true );
        p_sys->context.i_lastseqnumber = FragGetMoofSequenceNumber( p_moox );

        p_sys->i_nztime = FragGetDemuxTimeFromTracksTime( p_sys );
        p_sys->i_pcr = VLC_TICK_INVALID;
    }

    msg_Dbg( p_demux, "seeked to %4.4s at pos %" PRIu64, (char *) &i_moox, p_moox->i_pos );
    return VLC_SUCCESS;
}

static unsigned GetSeekTrackIndex( demux_sys_t *p_sys )
{
    unsigned cand = 0;
    for( unsigned i=0; i<p_sys->i_tracks; i++ )
    {
        if( p_sys->track[i].fmt.i_cat == VIDEO_ES ||
            p_sys->track[i].fmt.i_cat == AUDIO_ES )
        {
            if( cand != i && !p_sys->track[cand].b_selected )
                cand = i;
        }
    }
    return cand;
}

static void FragTrunSeekToTime( mp4_track_t *p_track, stime_t i_target_time )
{
    if( !p_track->b_ok || p_track->context.runs.i_count < 1 )
        return;

    unsigned i_run = 0;
    unsigned i_sample = 0;
    uint64_t i_pos = p_track->context.runs.p_array[0].i_offset;
    stime_t  i_time = p_track->context.runs.p_array[0].i_first_dts;

    for( unsigned r = 0; r < p_track->context.runs.i_count; r++ )
    {
        const mp4_run_t *p_run = &p_track->context.runs.p_array[r];
        const MP4_Box_data_trun_t *p_data =
                    p_track->context.runs.p_array[r].p_trun->data.p_trun;
        if( i_time > i_target_time )
            break;

        i_run = r;
        i_time = p_run->i_first_dts;
        i_pos = p_run->i_offset;
        i_sample = 0;

        uint32_t dur = p_track->context.i_default_sample_duration;
        uint32_t len = p_track->context.i_default_sample_size;
        for ( unsigned i=0; i<p_data->i_sample_count; i++ )
        {
            if( p_data->i_flags & MP4_TRUN_SAMPLE_DURATION )
                dur = p_data->p_samples[i].i_duration;

            /* check condition */
            if( i_time + dur > i_target_time )
                break;

            if( p_data->i_flags & MP4_TRUN_SAMPLE_SIZE )
                len = p_data->p_samples[i].i_size;

            i_time += dur;
            i_pos += len;
        }
    }

    p_track->context.i_trun_sample = i_sample;
    p_track->context.i_trun_sample_pos = i_pos;
    p_track->context.runs.i_current = i_run;
}

#define INVALID_SEGMENT_TIME  INT64_MAX

static int FragSeekToTime( demux_t *p_demux, vlc_tick_t i_nztime, bool b_accurate )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    uint64_t i64 = UINT64_MAX;
    uint32_t i_segment_type = ATOM_moof;
    stime_t  i_segment_time = INVALID_SEGMENT_TIME;
    vlc_tick_t i_sync_time = i_nztime;
    bool b_iframesync = false;

    const uint64_t i_duration = __MAX(p_sys->i_duration, p_sys->i_cumulated_duration);
    if ( !p_sys->i_timescale || !i_duration || !p_sys->b_seekable )
         return VLC_EGENERIC;

    uint64_t i_backup_pos = vlc_stream_Tell( p_sys->s_frag );

    if ( !p_sys->b_fragments_probed && !p_sys->b_index_probed && p_sys->b_seekable )
    {
        ProbeIndex( p_demux );
        p_sys->b_index_probed = true;
    }

    const unsigned i_seek_track_index = GetSeekTrackIndex( p_sys );
    const unsigned i_seek_track_ID = p_sys->track[i_seek_track_index].i_track_ID;

    if( MP4_rescale_qtime( i_nztime, p_sys->i_timescale )
                     < GetMoovTrackDuration( p_sys, i_seek_track_ID ) )
    {
        i64 = p_sys->p_moov->i_pos;
        i_segment_type = ATOM_moov;
    }
    else if( FragGetMoofBySidxIndex( p_demux, i_nztime, &i64, &i_sync_time ) == VLC_SUCCESS )
    {
        /* provides base offset */
        i_segment_time = i_sync_time;
        msg_Dbg( p_demux, "seeking to sidx moof pos %" PRId64 " %" PRId64, i64, i_sync_time );
    }
    else
    {
        bool b_buildindex = false;

        if( FragGetMoofByTfraIndex( p_demux, i_nztime, i_seek_track_ID, &i64, &i_sync_time ) == VLC_SUCCESS )
        {
            /* Does only provide segment position and a sync sample time */
            msg_Dbg( p_demux, "seeking to sync point %" PRId64, i_sync_time );
            b_iframesync = true;
        }
        else if( !p_sys->b_fragments_probed && !p_sys->b_fastseekable )
        {
            const char *psz_msg = _(
                "Because this file index is broken or missing, "
                "seeking will not work correctly.\n"
                "VLC won't repair your file but can temporary fix this "
                "problem by building an index in memory.\n"
                "This step might take a long time on a large file.\n"
                "What do you want to do?");
            b_buildindex = vlc_dialog_wait_question( p_demux,
                                                     VLC_DIALOG_QUESTION_NORMAL,
                                                     _("Do not seek"),
                                                     _("Build index"),
                                                     NULL,
                                                     _("Broken or missing Index"),
                                                     "%s", psz_msg );
        }

        if( !p_sys->b_fragments_probed && ( p_sys->b_fastseekable || b_buildindex ) )
        {
            bool foo;
            int i_ret = vlc_stream_Seek( p_sys->s_frag, p_sys->p_moov->i_pos + p_sys->p_moov->i_size );
            if( i_ret == VLC_SUCCESS )
            {
                i_ret = ProbeFragments( p_demux, true, &foo );
                p_sys->b_fragments_probed = true;
            }
            if( i_ret != VLC_SUCCESS )
            {
                p_sys->b_error = (vlc_stream_Seek( p_sys->s_frag, i_backup_pos ) != VLC_SUCCESS);
                return i_ret;
            }
        }

        if( p_sys->b_fragments_probed && p_sys->p_fragsindex )
        {
            stime_t i_basetime = MP4_rescale_qtime( i_sync_time, p_sys->i_timescale );
            if( !MP4_Fragments_Index_Lookup( p_sys->p_fragsindex, &i_basetime, &i64, i_seek_track_index ) )
            {
                p_sys->b_error = (vlc_stream_Seek( p_sys->s_frag, i_backup_pos ) != VLC_SUCCESS);
                return VLC_EGENERIC;
            }
            msg_Dbg( p_demux, "seeking to fragment index pos %" PRId64 " %" PRId64, i64,
                     MP4_rescale_mtime( i_basetime, p_sys->i_timescale ) );
        }
    }

    if( i64 == UINT64_MAX )
    {
        msg_Warn( p_demux, "seek by index failed" );
        p_sys->b_error = (vlc_stream_Seek( p_sys->s_frag, i_backup_pos ) != VLC_SUCCESS);
        return VLC_EGENERIC;
    }

    msg_Dbg( p_demux, "final seek to fragment at %"PRId64, i64 );
    if( vlc_stream_Seek( p_sys->s_frag, i64 ) )
    {
        msg_Err( p_demux, "seek failed to %"PRId64, i64 );
        p_sys->b_error = (vlc_stream_Seek( p_sys->s_frag, i_backup_pos ) != VLC_SUCCESS);
        return VLC_EGENERIC;
    }

    /* Context is killed on success */
    if( FragSeekLoadFragment( p_demux, i_segment_type, i_segment_time ) != VLC_SUCCESS )
    {
        p_sys->b_error = (vlc_stream_Seek( p_sys->s_frag, i_backup_pos ) != VLC_SUCCESS);
        return VLC_EGENERIC;
    }

    p_sys->i_pcr  = VLC_TICK_INVALID;

    for( unsigned i=0; i<p_sys->i_tracks; i++ )
    {
        if( i_segment_type == ATOM_moov )
        {
            MP4_TrackSeek( p_demux, &p_sys->track[i], i_sync_time );
            p_sys->i_nztime = i_sync_time;
            p_sys->i_pcr  = VLC_TICK_INVALID;
        }
        else if( b_iframesync )
        {
            stime_t i_tst = MP4_rescale_qtime( i_sync_time, p_sys->track[i].i_timescale );
            FragTrunSeekToTime( &p_sys->track[i], i_tst );
            p_sys->track[i].i_next_block_flags |= BLOCK_FLAG_DISCONTINUITY;
        }
    }

    MP4ASF_ResetFrames( p_sys );
    /* And set next display time in that trun/fragment */
    if( b_iframesync && b_accurate )
        es_out_Control( p_demux->out, ES_OUT_SET_NEXT_DISPLAY_TIME, VLC_TICK_0 + i_nztime );
    return VLC_SUCCESS;
}

static int FragSeekToPos( demux_t *p_demux, double f, bool b_accurate )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const uint64_t i_duration = __MAX(p_sys->i_duration, p_sys->i_cumulated_duration);

    if ( !p_sys->b_seekable || !p_sys->i_timescale || !i_duration )
        return VLC_EGENERIC;

    return FragSeekToTime( p_demux, (vlc_tick_t)( f *
                           MP4_rescale_mtime( i_duration, p_sys->i_timescale ) ), b_accurate );
}

static bool imageTypeCompatible( const MP4_Box_data_data_t *p_data )
{
    return p_data && (
    p_data->e_wellknowntype == DATA_WKT_PNG ||
    p_data->e_wellknowntype == DATA_WKT_JPEG ||
    p_data->e_wellknowntype == DATA_WKT_BMP );
}

static int MP4_LoadMeta( demux_sys_t *p_sys, vlc_meta_t *p_meta )
{
    MP4_Box_t *p_data = NULL;
    MP4_Box_t *p_udta = NULL;
    bool b_attachment_set = false;

    if( !p_meta )
        return VLC_EGENERIC;

    for( int i_index = 0; psz_meta_roots[i_index] && !p_udta; i_index++ )
    {
        p_udta = MP4_BoxGet( p_sys->p_root, psz_meta_roots[i_index] );
        if ( p_udta )
        {
            p_data = MP4_BoxGet( p_udta, "covr/data" );
            if ( p_data && imageTypeCompatible( BOXDATA(p_data) ) )
            {
                char *psz_attachment;
                if ( -1 != asprintf( &psz_attachment, "attachment://%s/covr/data[0]",
                                     psz_meta_roots[i_index] ) )
                {
                    vlc_meta_SetArtURL( p_meta, psz_attachment );
                    b_attachment_set = true;
                    free( psz_attachment );
                }
            }
        }
    }

    const MP4_Box_t *p_pnot;
    if ( !b_attachment_set && (p_pnot = MP4_BoxGet( p_sys->p_root, "pnot" )) )
    {
        for ( size_t i=0; i< ARRAY_SIZE(rgi_pict_atoms) && !b_attachment_set; i++ )
        {
            if ( rgi_pict_atoms[i] == BOXDATA(p_pnot)->i_type )
            {
                char rgsz_path[26];
                snprintf( rgsz_path, 26, "attachment://%4.4s[%"PRIu16"]",
                          (char*)&rgi_pict_atoms[i], BOXDATA(p_pnot)->i_index - 1 );
                vlc_meta_SetArtURL( p_meta, rgsz_path );
                b_attachment_set = true;
            }
        }
    }

    if( p_udta == NULL )
    {
        if( !b_attachment_set )
            return VLC_EGENERIC;
    }
    else SetupMeta( p_meta, p_udta );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Control:
 *****************************************************************************/
static int __mp4_Control( demux_t *p_demux, int i_query, va_list args )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    double f, *pf;
    vlc_tick_t i64;
    bool b;

    const uint64_t i_duration = __MAX(p_sys->i_duration, p_sys->i_cumulated_duration);

    switch( i_query )
    {
        case DEMUX_CAN_SEEK:
            *va_arg( args, bool * ) = p_sys->b_seekable;
            return VLC_SUCCESS;

        case DEMUX_GET_POSITION:
            pf = va_arg( args, double * );
            if( i_duration > 0 )
            {
                *pf = (double)p_sys->i_nztime /
                      MP4_rescale_mtime( i_duration, p_sys->i_timescale );
            }
            else
            {
                *pf = 0.0;
            }
            return VLC_SUCCESS;

        case DEMUX_GET_SEEKPOINT:
            *va_arg( args, int * ) = p_sys->i_seekpoint;
            return VLC_SUCCESS;

        case DEMUX_SET_POSITION:
            f = va_arg( args, double );
            b = va_arg( args, int );
            if ( p_demux->pf_demux == DemuxFrag )
                return FragSeekToPos( p_demux, f, b );
            else if( p_sys->i_timescale > 0 )
            {
                i64 = (vlc_tick_t)( f * MP4_rescale_mtime( p_sys->i_duration,
                                                        p_sys->i_timescale ) );
                return Seek( p_demux, i64, b );
            }
            else return VLC_EGENERIC;

        case DEMUX_GET_TIME:
            *va_arg( args, vlc_tick_t * ) = p_sys->i_timescale > 0 ? p_sys->i_nztime : 0;
            return VLC_SUCCESS;

        case DEMUX_SET_TIME:
            i64 = va_arg( args, vlc_tick_t );
            b = va_arg( args, int );
            if ( p_demux->pf_demux == DemuxFrag )
                return FragSeekToTime( p_demux, i64, b );
            else
                return Seek( p_demux, i64, b );

        case DEMUX_GET_LENGTH:
            if( p_sys->i_timescale > 0 )
            {
                *va_arg( args, vlc_tick_t * ) = MP4_rescale_mtime( i_duration,
                                                           p_sys->i_timescale );
            }
            else *va_arg( args, vlc_tick_t * ) = 0;
            return VLC_SUCCESS;

        case DEMUX_GET_FPS:
            pf = va_arg( args, double * );
            *pf = p_sys->f_fps;
            return VLC_SUCCESS;

        case DEMUX_GET_ATTACHMENTS:
        {
            input_attachment_t ***ppp_attach = va_arg( args, input_attachment_t*** );
            int *pi_int = va_arg( args, int * );

            MP4_Box_t *p_udta = NULL;
            size_t i_count = 0;
            int i_index = 0;

            /* Count number of total attachments */
            for( ; psz_meta_roots[i_index] && !p_udta; i_index++ )
            {
                p_udta = MP4_BoxGet( p_sys->p_root, psz_meta_roots[i_index] );
                if ( p_udta )
                    i_count += MP4_BoxCount( p_udta, "covr/data" );
            }

            for ( size_t i=0; i< ARRAY_SIZE(rgi_pict_atoms); i++ )
            {
                char rgsz_path[5];
                snprintf( rgsz_path, 5, "%4.4s", (char*)&rgi_pict_atoms[i] );
                i_count += MP4_BoxCount( p_sys->p_root, rgsz_path );
            }

            if ( i_count == 0 )
                return VLC_EGENERIC;

            *ppp_attach = (input_attachment_t**)
                    vlc_alloc( i_count, sizeof(input_attachment_t*) );
            if( !(*ppp_attach) ) return VLC_ENOMEM;

            /* First add cover attachments */
            i_count = 0;
            size_t i_box_count = 0;
            if ( p_udta )
            {
                const MP4_Box_t *p_data = MP4_BoxGet( p_udta, "covr/data" );
                for( ; p_data; p_data = p_data->p_next )
                {
                    char *psz_mime;
                    char *psz_filename;
                    i_box_count++;

                    if ( p_data->i_type != ATOM_data || !imageTypeCompatible( BOXDATA(p_data) ) )
                        continue;

                    switch( BOXDATA(p_data)->e_wellknowntype )
                    {
                    case DATA_WKT_PNG:
                        psz_mime = strdup( "image/png" );
                        break;
                    case DATA_WKT_JPEG:
                        psz_mime = strdup( "image/jpeg" );
                        break;
                    case DATA_WKT_BMP:
                        psz_mime = strdup( "image/bmp" );
                        break;
                    default:
                        continue;
                    }

                    if ( asprintf( &psz_filename, "%s/covr/data[%"PRIu64"]", psz_meta_roots[i_index - 1],
                                   (uint64_t) i_box_count - 1 ) >= 0 )
                    {
                        (*ppp_attach)[i_count++] =
                            vlc_input_attachment_New( psz_filename, psz_mime, "Cover picture",
                                BOXDATA(p_data)->p_blob, BOXDATA(p_data)->i_blob );
                        msg_Dbg( p_demux, "adding attachment %s", psz_filename );
                        free( psz_filename );
                    }

                    free( psz_mime );
                }
            }

            /* Then quickdraw pict ones */
            for ( size_t i=0; i< ARRAY_SIZE(rgi_pict_atoms); i++ )
            {
                char rgsz_path[5];
                snprintf( rgsz_path, 5, "%4.4s", (char*)&rgi_pict_atoms[i] );
                const MP4_Box_t *p_pict = MP4_BoxGet( p_sys->p_root, rgsz_path );
                i_box_count = 0;
                for( ; p_pict; p_pict = p_pict->p_next )
                {
                    if ( i_box_count++ == UINT16_MAX ) /* pnot only handles 2^16 */
                        break;
                    if ( p_pict->i_type != rgi_pict_atoms[i] )
                        continue;
                    char rgsz_location[12];
                    snprintf( rgsz_location, 12, "%4.4s[%"PRIu16"]", (char*)&rgi_pict_atoms[i],
                              (uint16_t) i_box_count - 1 );
                    (*ppp_attach)[i_count] = vlc_input_attachment_New( rgsz_location, "image/x-pict",
                        "Quickdraw image", p_pict->data.p_binary->p_blob, p_pict->data.p_binary->i_blob );
                    if ( !(*ppp_attach)[i_count] )
                    {
                        i_count = 0;
                        break;
                    }
                    i_count++;
                    msg_Dbg( p_demux, "adding attachment %s", rgsz_location );
                }
            }

            if ( i_count == 0 )
            {
                free( *ppp_attach );
                return VLC_EGENERIC;
            }

            *pi_int = i_count;

            return VLC_SUCCESS;
        }

        case DEMUX_GET_META:
        {
            vlc_meta_t *p_meta = va_arg( args, vlc_meta_t *);

            if( !p_sys->p_meta )
                return VLC_EGENERIC;

            vlc_meta_Merge( p_meta, p_sys->p_meta );

            return VLC_SUCCESS;
        }

        case DEMUX_GET_TITLE_INFO:
        {
            input_title_t ***ppp_title = va_arg( args, input_title_t *** );
            int *pi_int = va_arg( args, int* );
            int *pi_title_offset = va_arg( args, int* );
            int *pi_seekpoint_offset = va_arg( args, int* );

            if( !p_sys->p_title )
                return VLC_EGENERIC;

            *pi_int = 1;
            *ppp_title = malloc( sizeof( input_title_t*) );
            (*ppp_title)[0] = vlc_input_title_Duplicate( p_sys->p_title );
            *pi_title_offset = 0;
            *pi_seekpoint_offset = 0;
            return VLC_SUCCESS;
        }
        case DEMUX_SET_TITLE:
        {
            const int i_title = va_arg( args, int );
            if( !p_sys->p_title || i_title != 0 )
                return VLC_EGENERIC;
            return VLC_SUCCESS;
        }
        case DEMUX_SET_SEEKPOINT:
        {
            const int i_seekpoint = va_arg( args, int );
            if( !p_sys->p_title )
                return VLC_EGENERIC;
            return Seek( p_demux, p_sys->p_title->seekpoint[i_seekpoint]->i_time_offset, true );
        }
        case DEMUX_TEST_AND_CLEAR_FLAGS:
        {
            unsigned *restrict flags = va_arg( args, unsigned * );

            if ((*flags & INPUT_UPDATE_SEEKPOINT) && p_sys->seekpoint_changed)
            {
                *flags = INPUT_UPDATE_SEEKPOINT;
                p_sys->seekpoint_changed = false;
            }
            else
                *flags = 0;
            return VLC_SUCCESS;
        }

        case DEMUX_GET_PTS_DELAY:
        {
            for( unsigned int i = 0; i < p_sys->i_tracks; i++ )
            {
                const MP4_Box_t *p_load;
                if ( (p_load = MP4_BoxGet( p_sys->track[i].p_track, "load" )) &&
                     BOXDATA(p_load)->i_duration > 0 )
                {
                    *va_arg(args, vlc_tick_t *) =
                            MP4_rescale_mtime( BOXDATA(p_load)->i_duration,
                                               p_sys->track[i].i_timescale );
                    return VLC_SUCCESS;
                }
            }
            return demux_vaControlHelper( p_sys->s_frag, 0, -1, 0, 1, i_query, args );
        }
        case DEMUX_SET_NEXT_DEMUX_TIME:
        case DEMUX_SET_GROUP_DEFAULT:
        case DEMUX_SET_GROUP_ALL:
        case DEMUX_SET_GROUP_LIST:
        case DEMUX_HAS_UNSUPPORTED_META:
        case DEMUX_CAN_RECORD:
            return VLC_EGENERIC;

        case DEMUX_CAN_PAUSE:
        case DEMUX_SET_PAUSE_STATE:
        case DEMUX_CAN_CONTROL_PACE:
            return demux_vaControlHelper( p_sys->s_frag, 0, -1, 0, 1, i_query, args );

        default:
            return VLC_EGENERIC;
    }
}

/*****************************************************************************
 * Close: frees unused data
 *****************************************************************************/
static void __mp4_Close ( vlc_object_t * p_this )
{
    demux_t *  p_demux = (demux_t *)p_this;
    demux_sys_t *p_sys = p_demux->p_sys;
    unsigned int i_track;

    msg_Dbg( p_demux, "freeing all memory" );

    FragResetContext( p_sys );

    MP4_BoxFree( p_sys->p_root );

    if( p_sys->p_title )
        vlc_input_title_Delete( p_sys->p_title );

    if( p_sys->p_meta )
        vlc_meta_Delete( p_sys->p_meta );

    MP4_Fragments_Index_Delete( p_sys->p_fragsindex );

    for( i_track = 0; i_track < p_sys->i_tracks; i_track++ )
        MP4_TrackClean( p_demux->out, &p_sys->track[i_track] );
    free( p_sys->track );

    free( p_sys );
}



/****************************************************************************
 * Local functions, specific to vlc
 ****************************************************************************/
/* Chapters */
static void LoadChapterGpac( demux_t  *p_demux, MP4_Box_t *p_chpl )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if( BOXDATA(p_chpl)->i_chapter == 0 )
        return;

    p_sys->p_title = vlc_input_title_New();
    for( int i = 0; i < BOXDATA(p_chpl)->i_chapter && p_sys->p_title; i++ )
    {
        seekpoint_t *s = vlc_seekpoint_New();
        if( s == NULL) continue;

        s->psz_name = strdup( BOXDATA(p_chpl)->chapter[i].psz_name );
        if( s->psz_name == NULL)
        {
            vlc_seekpoint_Delete( s );;
            continue;
        }

        EnsureUTF8( s->psz_name );
        msftime_t offset = BOXDATA(p_chpl)->chapter[i].i_start;
        s->i_time_offset = VLC_TICK_FROM_MSFTIME(offset);
        TAB_APPEND( p_sys->p_title->i_seekpoint, p_sys->p_title->seekpoint, s );
    }
}
static void LoadChapterGoPro( demux_t *p_demux, MP4_Box_t *p_hmmt )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    p_sys->p_title = vlc_input_title_New();
    if( p_sys->p_title )
        for( unsigned i = 0; i < BOXDATA(p_hmmt)->i_chapter_count; i++ )
        {
            seekpoint_t *s = vlc_seekpoint_New();
            if( s )
            {
                if( asprintf( &s->psz_name, "HiLight tag #%u", i+1 ) != -1 )
                    EnsureUTF8( s->psz_name );

                /* HiLights are stored in ms so we convert them to µs */
                s->i_time_offset = VLC_TICK_FROM_MS( BOXDATA(p_hmmt)->pi_chapter_start[i] );
                TAB_APPEND( p_sys->p_title->i_seekpoint, p_sys->p_title->seekpoint, s );
            }
        }
}
static void LoadChapterApple( demux_t  *p_demux, mp4_track_t *tk )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    for( tk->i_sample = 0; tk->i_sample < tk->i_sample_count; tk->i_sample++ )
    {
        const vlc_tick_t i_dts = MP4_TrackGetDTS( p_demux, tk );
        vlc_tick_t i_pts_delta;
        if ( !MP4_TrackGetPTSDelta( p_demux, tk, &i_pts_delta ) )
            i_pts_delta = 0;
        uint32_t i_nb_samples = 0;
        const uint32_t i_size = MP4_TrackGetReadSize( tk, &i_nb_samples );

        if( i_size > 0 && !vlc_stream_Seek( p_sys->s_frag, MP4_TrackGetPos( tk ) ) )
        {
            char p_buffer[256];
            const uint32_t i_read = stream_ReadU32( p_sys->s_frag, p_buffer,
                                                    __MIN( sizeof(p_buffer), i_size ) );
            if( i_read > 2 )
            {
                const uint32_t i_string = __MIN( GetWBE(p_buffer), i_read-2 );
                const char *psnz_string = &p_buffer[2];

                seekpoint_t *s = vlc_seekpoint_New();
                if( s == NULL ) continue;

                if( i_string > 1 && !memcmp( psnz_string, "\xFF\xFE", 2 ) )
                    s->psz_name = FromCharset( "UTF-16LE", psnz_string, i_string );
                else
                    s->psz_name = strndup( psnz_string, i_string );

                if( s->psz_name == NULL )
                {
                    vlc_seekpoint_Delete( s );
                    continue;
                }

                EnsureUTF8( s->psz_name );
                s->i_time_offset = i_dts + __MAX( i_pts_delta, 0 );

                if( !p_sys->p_title )
                    p_sys->p_title = vlc_input_title_New();
                TAB_APPEND( p_sys->p_title->i_seekpoint, p_sys->p_title->seekpoint, s );
            }
        }
        if( tk->i_sample+1 >= tk->chunk[tk->i_chunk].i_sample_first +
                              tk->chunk[tk->i_chunk].i_sample_count )
            tk->i_chunk++;
    }
}
static void LoadChapter( demux_t  *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    MP4_Box_t *p_chpl;
    MP4_Box_t *p_hmmt;

    if( ( p_chpl = MP4_BoxGet( p_sys->p_root, "/moov/udta/chpl" ) ) &&
          BOXDATA(p_chpl) && BOXDATA(p_chpl)->i_chapter > 0 )
    {
        LoadChapterGpac( p_demux, p_chpl );
    }
    else if( ( p_hmmt = MP4_BoxGet( p_sys->p_root, "/moov/udta/HMMT" ) ) &&
             BOXDATA(p_hmmt) && BOXDATA(p_hmmt)->pi_chapter_start && BOXDATA(p_hmmt)->i_chapter_count > 0 )
    {
        LoadChapterGoPro( p_demux, p_hmmt );
    }
    else if( p_sys->p_tref_chap )
    {
        MP4_Box_data_tref_generic_t *p_chap = p_sys->p_tref_chap->data.p_tref_generic;
        unsigned int i, j;

        /* Load the first subtitle track like quicktime */
        for( i = 0; i < p_chap->i_entry_count; i++ )
        {
            for( j = 0; j < p_sys->i_tracks; j++ )
            {
                mp4_track_t *tk = &p_sys->track[j];
                if( tk->b_ok && tk->i_track_ID == p_chap->i_track_ID[i] &&
                    tk->fmt.i_cat == SPU_ES && tk->fmt.i_codec == VLC_CODEC_TX3G )
                    break;
            }
            if( j < p_sys->i_tracks )
            {
                LoadChapterApple( p_demux, &p_sys->track[j] );
                break;
            }
        }
    }

    /* Add duration if titles are enabled */
    if( p_sys->p_title )
    {
        const uint64_t i_duration = __MAX(p_sys->i_duration, p_sys->i_cumulated_duration);
        p_sys->p_title->i_length =
                MP4_rescale_mtime( i_duration, p_sys->i_timescale );
    }
}

/* now create basic chunk data, the rest will be filled by MP4_CreateSamplesIndex */
static int TrackCreateChunksIndex( demux_t *p_demux,
                                   mp4_track_t *p_demux_track )
{
    MP4_Box_t *p_co64; /* give offset for each chunk, same for stco and co64 */
    MP4_Box_t *p_stsc;

    unsigned int i_chunk;
    unsigned int i_index, i_last;

    if( ( !(p_co64 = MP4_BoxGet( p_demux_track->p_stbl, "stco" ) )&&
          !(p_co64 = MP4_BoxGet( p_demux_track->p_stbl, "co64" ) ) )||
        ( !(p_stsc = MP4_BoxGet( p_demux_track->p_stbl, "stsc" ) ) ))
    {
        return( VLC_EGENERIC );
    }

    p_demux_track->i_chunk_count = BOXDATA(p_co64)->i_entry_count;
    if( !p_demux_track->i_chunk_count )
    {
        msg_Warn( p_demux, "no chunk defined" );
    }
    p_demux_track->chunk = calloc( p_demux_track->i_chunk_count,
                                   sizeof( mp4_chunk_t ) );
    if( p_demux_track->chunk == NULL )
    {
        return VLC_ENOMEM;
    }

    /* first we read chunk offset */
    for( i_chunk = 0; i_chunk < p_demux_track->i_chunk_count; i_chunk++ )
    {
        mp4_chunk_t *ck = &p_demux_track->chunk[i_chunk];

        ck->i_offset = BOXDATA(p_co64)->i_chunk_offset[i_chunk];

        ck->i_first_dts = 0;
        ck->i_entries_dts = 0;
        ck->p_sample_count_dts = NULL;
        ck->p_sample_delta_dts = NULL;
        ck->i_entries_pts = 0;
        ck->p_sample_count_pts = NULL;
        ck->p_sample_offset_pts = NULL;
    }

    /* now we read index for SampleEntry( soun vide mp4a mp4v ...)
        to be used for the sample XXX begin to 1
        We construct it begining at the end */
    i_last = p_demux_track->i_chunk_count; /* last chunk proceded */
    i_index = BOXDATA(p_stsc)->i_entry_count;

    while( i_index-- > 0 )
    {
        for( i_chunk = BOXDATA(p_stsc)->i_first_chunk[i_index] - 1;
             i_chunk < i_last; i_chunk++ )
        {
            if( i_chunk >= p_demux_track->i_chunk_count )
            {
                msg_Warn( p_demux, "corrupted chunk table" );
                return VLC_EGENERIC;
            }

            p_demux_track->chunk[i_chunk].i_sample_description_index =
                    BOXDATA(p_stsc)->i_sample_description_index[i_index];
            p_demux_track->chunk[i_chunk].i_sample_count =
                    BOXDATA(p_stsc)->i_samples_per_chunk[i_index];
        }
        i_last = BOXDATA(p_stsc)->i_first_chunk[i_index] - 1;
    }

    p_demux_track->i_sample_count = 0;
    bool b_broken = false;
    if ( p_demux_track->i_chunk_count )
    {
        p_demux_track->chunk[0].i_sample_first = 0;
        p_demux_track->i_sample_count += p_demux_track->chunk[0].i_sample_count;

        const mp4_chunk_t *prev = &p_demux_track->chunk[0];
        for( i_chunk = 1; i_chunk < p_demux_track->i_chunk_count; i_chunk++ )
        {
            mp4_chunk_t *cur = &p_demux_track->chunk[i_chunk];
            if( unlikely(UINT32_MAX - cur->i_sample_count < p_demux_track->i_sample_count) )
            {
                b_broken = true;
                break;
            }
            p_demux_track->i_sample_count += cur->i_sample_count;
            cur->i_sample_first = prev->i_sample_first + prev->i_sample_count;
            prev = cur;
        }
    }

    if( unlikely(b_broken) )
    {
        msg_Err( p_demux, "Overflow in chunks total samples count" );
        return VLC_EGENERIC;
    }

    msg_Dbg( p_demux, "track[Id 0x%x] read %d chunk",
             p_demux_track->i_track_ID, p_demux_track->i_chunk_count );

    return VLC_SUCCESS;
}

static int xTTS_CountEntries( demux_t *p_demux, uint32_t *pi_entry /* out */,
                              const uint32_t i_index,
                              uint32_t i_index_samples_left,
                              uint32_t i_sample_count,
                              const uint32_t *pi_index_sample_count,
                              const uint32_t i_table_count )
{
    uint32_t i_array_offset;
    while( i_sample_count > 0 )
    {
        if ( likely((UINT32_MAX - i_index) >= *pi_entry) )
            i_array_offset = i_index + *pi_entry;
        else
            return VLC_EGENERIC;

        if ( i_array_offset >= i_table_count )
        {
            msg_Err( p_demux, "invalid index counting total samples %u %u", i_array_offset,  i_table_count );
            return VLC_ENOVAR;
        }

        if ( i_index_samples_left )
        {
            if ( i_index_samples_left > i_sample_count )
            {
                i_index_samples_left -= i_sample_count;
                i_sample_count = 0;
                *pi_entry +=1; /* No samples left, go copy */
                break;
            }
            else
            {
                i_sample_count -= i_index_samples_left;
                i_index_samples_left = 0;
                *pi_entry += 1;
                continue;
            }
        }
        else
        {
            i_sample_count -= __MIN( i_sample_count, pi_index_sample_count[i_array_offset] );
            *pi_entry += 1;
        }
    }

    return VLC_SUCCESS;
}

static int TrackCreateSamplesIndex( demux_t *p_demux,
                                    mp4_track_t *p_demux_track )
{
    MP4_Box_t *p_box;
    MP4_Box_data_stsz_t *stsz;
    /* TODO use also stss and stsh table for seeking */
    /* FIXME use edit table */

    /* Find stsz
     *  Gives the sample size for each samples. There is also a stz2 table
     *  (compressed form) that we need to implement TODO */
    p_box = MP4_BoxGet( p_demux_track->p_stbl, "stsz" );
    if( !p_box )
    {
        /* FIXME and stz2 */
        msg_Warn( p_demux, "cannot find STSZ box" );
        return VLC_EGENERIC;
    }
    stsz = p_box->data.p_stsz;

    /* Use stsz table to create a sample number -> sample size table */
    if( p_demux_track->i_sample_count != stsz->i_sample_count )
    {
        msg_Warn( p_demux, "Incorrect total samples stsc %" PRIu32 " <> stsz %"PRIu32 ", "
                           " expect truncated media playback",
                           p_demux_track->i_sample_count, stsz->i_sample_count );
        p_demux_track->i_sample_count = __MIN(p_demux_track->i_sample_count, stsz->i_sample_count);
    }

    if( stsz->i_sample_size )
    {
        /* 1: all sample have the same size, so no need to construct a table */
        p_demux_track->i_sample_size = stsz->i_sample_size;
        p_demux_track->p_sample_size = NULL;
    }
    else
    {
        /* 2: each sample can have a different size */
        p_demux_track->i_sample_size = 0;
        p_demux_track->p_sample_size =
            calloc( p_demux_track->i_sample_count, sizeof( uint32_t ) );
        if( p_demux_track->p_sample_size == NULL )
            return VLC_ENOMEM;

        for( uint32_t i_sample = 0; i_sample < p_demux_track->i_sample_count; i_sample++ )
        {
            p_demux_track->p_sample_size[i_sample] =
                    stsz->i_entry_size[i_sample];
        }
    }

    if ( p_demux_track->i_chunk_count && p_demux_track->i_sample_size == 0 )
    {
        const mp4_chunk_t *lastchunk = &p_demux_track->chunk[p_demux_track->i_chunk_count - 1];
        if( (uint64_t)lastchunk->i_sample_count + p_demux_track->i_chunk_count - 1 > stsz->i_sample_count )
        {
            msg_Err( p_demux, "invalid samples table: stsz table is too small" );
            return VLC_EGENERIC;
        }
    }

    /* Use stts table to create a sample number -> dts table.
     * XXX: if we don't want to waste too much memory, we can't expand
     *  the box! so each chunk will contain an "extract" of this table
     *  for fast research (problem with raw stream where a sample is sometime
     *  just channels*bits_per_sample/8 */

     /* FIXME: refactor STTS & CTTS, STTS having now only few extra lines and
      *        differing in 2/2 fields and 1 signedness */

    int64_t i_next_dts = 0;
    /* Find stts
     *  Gives mapping between sample and decoding time
     */
    p_box = MP4_BoxGet( p_demux_track->p_stbl, "stts" );
    if( !p_box )
    {
        msg_Warn( p_demux, "cannot find STTS box" );
        return VLC_EGENERIC;
    }
    else
    {
        MP4_Box_data_stts_t *stts = p_box->data.p_stts;

        msg_Warn( p_demux, "STTS table of %"PRIu32" entries", stts->i_entry_count );

        /* Create sample -> dts table per chunk */
        uint32_t i_index = 0;
        uint32_t i_current_index_samples_left = 0;

        for( uint32_t i_chunk = 0; i_chunk < p_demux_track->i_chunk_count; i_chunk++ )
        {
            mp4_chunk_t *ck = &p_demux_track->chunk[i_chunk];
            uint32_t i_sample_count;

            /* save first dts */
            ck->i_first_dts = i_next_dts;

            /* count how many entries are needed for this chunk
             * for p_sample_delta_dts and p_sample_count_dts */
            ck->i_entries_dts = 0;

            int i_ret = xTTS_CountEntries( p_demux, &ck->i_entries_dts, i_index,
                                           i_current_index_samples_left,
                                           ck->i_sample_count,
                                           stts->pi_sample_count,
                                           stts->i_entry_count );
            if ( i_ret == VLC_EGENERIC )
                return i_ret;

            /* allocate them */
            ck->p_sample_count_dts = calloc( ck->i_entries_dts, sizeof( uint32_t ) );
            ck->p_sample_delta_dts = calloc( ck->i_entries_dts, sizeof( uint32_t ) );
            if( !ck->p_sample_count_dts || !ck->p_sample_delta_dts )
            {
                free( ck->p_sample_count_dts );
                free( ck->p_sample_delta_dts );
                msg_Err( p_demux, "can't allocate memory for i_entry=%"PRIu32, ck->i_entries_dts );
                ck->i_entries_dts = 0;
                return VLC_ENOMEM;
            }

            /* now copy */
            i_sample_count = ck->i_sample_count;

            for( uint32_t i = 0; i < ck->i_entries_dts; i++ )
            {
                if ( i_current_index_samples_left )
                {
                    if ( i_current_index_samples_left > i_sample_count )
                    {
                        ck->p_sample_count_dts[i] = i_sample_count;
                        ck->p_sample_delta_dts[i] = stts->pi_sample_delta[i_index];
                        i_next_dts += ck->p_sample_count_dts[i] * stts->pi_sample_delta[i_index];
                        if ( i_sample_count ) ck->i_duration = i_next_dts - ck->i_first_dts;
                        i_current_index_samples_left -= i_sample_count;
                        i_sample_count = 0;
                        assert( i == ck->i_entries_dts - 1 );
                        break;
                    }
                    else
                    {
                        ck->p_sample_count_dts[i] = i_current_index_samples_left;
                        ck->p_sample_delta_dts[i] = stts->pi_sample_delta[i_index];
                        i_next_dts += ck->p_sample_count_dts[i] * stts->pi_sample_delta[i_index];
                        if ( i_current_index_samples_left ) ck->i_duration = i_next_dts - ck->i_first_dts;
                        i_sample_count -= i_current_index_samples_left;
                        i_current_index_samples_left = 0;
                        i_index++;
                    }
                }
                else
                {
                    if ( stts->pi_sample_count[i_index] > i_sample_count )
                    {
                        ck->p_sample_count_dts[i] = i_sample_count;
                        ck->p_sample_delta_dts[i] = stts->pi_sample_delta[i_index];
                        i_next_dts += ck->p_sample_count_dts[i] * stts->pi_sample_delta[i_index];
                        if ( i_sample_count ) ck->i_duration = i_next_dts - ck->i_first_dts;
                        i_current_index_samples_left = stts->pi_sample_count[i_index] - i_sample_count;
                        i_sample_count = 0;
                        assert( i == ck->i_entries_dts - 1 );
                        // keep building from same index
                    }
                    else
                    {
                        ck->p_sample_count_dts[i] = stts->pi_sample_count[i_index];
                        ck->p_sample_delta_dts[i] = stts->pi_sample_delta[i_index];
                        i_next_dts += ck->p_sample_count_dts[i] * stts->pi_sample_delta[i_index];
                        if ( stts->pi_sample_count[i_index] ) ck->i_duration = i_next_dts - ck->i_first_dts;
                        i_sample_count -= stts->pi_sample_count[i_index];
                        i_index++;
                    }
                }

            }
        }
    }


    /* Find ctts
     *  Gives the delta between decoding time (dts) and composition table (pts)
     */
    p_box = MP4_BoxGet( p_demux_track->p_stbl, "ctts" );
    if( p_box && p_box->data.p_ctts )
    {
        MP4_Box_data_ctts_t *ctts = p_box->data.p_ctts;

        msg_Warn( p_demux, "CTTS table of %"PRIu32" entries", ctts->i_entry_count );

        int64_t i_cts_shift = 0;
        const MP4_Box_t *p_cslg = MP4_BoxGet( p_demux_track->p_stbl, "cslg" );
        if( p_cslg && BOXDATA(p_cslg) )
            i_cts_shift = BOXDATA(p_cslg)->ct_to_dts_shift;

        /* Create pts-dts table per chunk */
        uint32_t i_index = 0;
        uint32_t i_current_index_samples_left = 0;

        for( uint32_t i_chunk = 0; i_chunk < p_demux_track->i_chunk_count; i_chunk++ )
        {
            mp4_chunk_t *ck = &p_demux_track->chunk[i_chunk];
            uint32_t i_sample_count;

            /* count how many entries are needed for this chunk
             * for p_sample_offset_pts and p_sample_count_pts */
            ck->i_entries_pts = 0;
            int i_ret = xTTS_CountEntries( p_demux, &ck->i_entries_pts, i_index,
                                           i_current_index_samples_left,
                                           ck->i_sample_count,
                                           ctts->pi_sample_count,
                                           ctts->i_entry_count );
            if ( i_ret == VLC_EGENERIC )
                return i_ret;

            /* allocate them */
            ck->p_sample_count_pts = calloc( ck->i_entries_pts, sizeof( uint32_t ) );
            ck->p_sample_offset_pts = calloc( ck->i_entries_pts, sizeof( int32_t ) );
            if( !ck->p_sample_count_pts || !ck->p_sample_offset_pts )
            {
                free( ck->p_sample_count_pts );
                free( ck->p_sample_offset_pts );
                msg_Err( p_demux, "can't allocate memory for i_entry=%"PRIu32, ck->i_entries_pts );
                ck->i_entries_pts = 0;
                return VLC_ENOMEM;
            }

            /* now copy */
            i_sample_count = ck->i_sample_count;

            for( uint32_t i = 0; i < ck->i_entries_pts; i++ )
            {
                if ( i_current_index_samples_left )
                {
                    if ( i_current_index_samples_left > i_sample_count )
                    {
                        ck->p_sample_count_pts[i] = i_sample_count;
                        ck->p_sample_offset_pts[i] = ctts->pi_sample_offset[i_index] + i_cts_shift;
                        i_current_index_samples_left -= i_sample_count;
                        i_sample_count = 0;
                        assert( i == ck->i_entries_pts - 1 );
                        break;
                    }
                    else
                    {
                        ck->p_sample_count_pts[i] = i_current_index_samples_left;
                        ck->p_sample_offset_pts[i] = ctts->pi_sample_offset[i_index] + i_cts_shift;
                        i_sample_count -= i_current_index_samples_left;
                        i_current_index_samples_left = 0;
                        i_index++;
                    }
                }
                else
                {
                    if ( ctts->pi_sample_count[i_index] > i_sample_count )
                    {
                        ck->p_sample_count_pts[i] = i_sample_count;
                        ck->p_sample_offset_pts[i] = ctts->pi_sample_offset[i_index] + i_cts_shift;
                        i_current_index_samples_left = ctts->pi_sample_count[i_index] - i_sample_count;
                        i_sample_count = 0;
                        assert( i == ck->i_entries_pts - 1 );
                        // keep building from same index
                    }
                    else
                    {
                        ck->p_sample_count_pts[i] = ctts->pi_sample_count[i_index];
                        ck->p_sample_offset_pts[i] = ctts->pi_sample_offset[i_index] + i_cts_shift;
                        i_sample_count -= ctts->pi_sample_count[i_index];
                        i_index++;
                    }
                }


            }
        }
    }

    msg_Dbg( p_demux, "track[Id 0x%x] read %"PRIu32" samples length:%"PRId64"s",
             p_demux_track->i_track_ID, p_demux_track->i_sample_count,
             i_next_dts / p_demux_track->i_timescale );

    return VLC_SUCCESS;
}


/**
 * It computes the sample rate for a video track using the given sample
 * description index
 */
static void TrackGetESSampleRate( demux_t *p_demux,
                                  unsigned *pi_num, unsigned *pi_den,
                                  const mp4_track_t *p_track,
                                  unsigned i_sd_index,
                                  unsigned i_chunk )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    *pi_num = 0;
    *pi_den = 0;

    MP4_Box_t *p_trak = MP4_GetTrakByTrackID( MP4_BoxGet( p_sys->p_root,
                                                          "/moov" ),
                                              p_track->i_track_ID );
    MP4_Box_t *p_mdhd = MP4_BoxGet( p_trak, "mdia/mdhd" );
    if ( p_mdhd && BOXDATA(p_mdhd) )
    {
        vlc_ureduce( pi_num, pi_den,
                     (uint64_t) BOXDATA(p_mdhd)->i_timescale * p_track->i_sample_count,
                     (uint64_t) BOXDATA(p_mdhd)->i_duration,
                     UINT16_MAX );
        return;
    }

    if( p_track->i_chunk_count == 0 )
        return;

    /* */
    const mp4_chunk_t *p_chunk = &p_track->chunk[i_chunk];
    while( p_chunk > &p_track->chunk[0] &&
           p_chunk[-1].i_sample_description_index == i_sd_index )
    {
        p_chunk--;
    }

    uint64_t i_sample = 0;
    uint64_t i_total_duration = 0;
    do
    {
        i_sample += p_chunk->i_sample_count;
        i_total_duration += p_chunk->i_duration;
        p_chunk++;
    }
    while( p_chunk < &p_track->chunk[p_track->i_chunk_count] &&
           p_chunk->i_sample_description_index == i_sd_index );

    if( i_sample > 0 && i_total_duration )
        vlc_ureduce( pi_num, pi_den,
                     i_sample * p_track->i_timescale,
                     i_total_duration,
                     UINT16_MAX);
}

/*
 * TrackCreateES:
 * Create ES and PES to init decoder if needed, for a track starting at i_chunk
 */
static int TrackCreateES( demux_t *p_demux, mp4_track_t *p_track,
                          unsigned int i_chunk, es_out_id_t **pp_es )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    unsigned int i_sample_description_index;

    if( p_sys->b_fragmented || p_track->i_chunk_count == 0 )
        i_sample_description_index = 1; /* XXX */
    else
        i_sample_description_index =
                p_track->chunk[i_chunk].i_sample_description_index;

    if( pp_es )
        *pp_es = NULL;

    if( !i_sample_description_index )
    {
        msg_Warn( p_demux, "invalid SampleEntry index (track[Id 0x%x])",
                  p_track->i_track_ID );
        return VLC_EGENERIC;
    }

    MP4_Box_t *p_sample = MP4_BoxGet(  p_track->p_stsd, "[%d]",
                            i_sample_description_index - 1 );

    if( !p_sample ||
        ( !p_sample->data.p_payload && p_track->fmt.i_cat != SPU_ES ) )
    {
        msg_Warn( p_demux, "cannot find SampleEntry (track[Id 0x%x])",
                  p_track->i_track_ID );
        return VLC_EGENERIC;
    }

    p_track->p_sample = p_sample;

    MP4_Box_t   *p_frma;
    if( ( p_frma = MP4_BoxGet( p_track->p_sample, "sinf/frma" ) ) && p_frma->data.p_frma )
    {
        msg_Warn( p_demux, "Original Format Box: %4.4s", (char *)&p_frma->data.p_frma->i_type );

        p_sample->i_type = p_frma->data.p_frma->i_type;
    }

    /* */
    switch( p_track->fmt.i_cat )
    {
    case VIDEO_ES:
        if ( p_sample->i_handler != ATOM_vide ||
             !SetupVideoES( p_demux, p_track, p_sample ) )
            return VLC_EGENERIC;

        /* Set frame rate */
        TrackGetESSampleRate( p_demux,
                              &p_track->fmt.video.i_frame_rate,
                              &p_track->fmt.video.i_frame_rate_base,
                              p_track, i_sample_description_index, i_chunk );

        p_sys->f_fps = (float)p_track->fmt.video.i_frame_rate /
                       (float)p_track->fmt.video.i_frame_rate_base;

        break;

    case AUDIO_ES:
        if ( p_sample->i_handler != ATOM_soun ||
             !SetupAudioES( p_demux, p_track, p_sample ) )
            return VLC_EGENERIC;
        if( p_sys->p_meta )
        {
            audio_replay_gain_t *p_arg = &p_track->fmt.audio_replay_gain;
            const char *psz_meta = vlc_meta_GetExtra( p_sys->p_meta, "replaygain_track_gain" );
            if( psz_meta )
            {
                double f_gain = us_atof( psz_meta );
                p_arg->pf_gain[AUDIO_REPLAY_GAIN_TRACK] = f_gain;
                p_arg->pb_gain[AUDIO_REPLAY_GAIN_TRACK] = f_gain != 0;
            }
            psz_meta = vlc_meta_GetExtra( p_sys->p_meta, "replaygain_track_peak" );
            if( psz_meta )
            {
                double f_gain = us_atof( psz_meta );
                p_arg->pf_peak[AUDIO_REPLAY_GAIN_TRACK] = f_gain;
                p_arg->pb_peak[AUDIO_REPLAY_GAIN_TRACK] = f_gain > 0;
            }
        }
        break;

    case SPU_ES:
        if ( ( p_sample->i_handler != ATOM_text &&
               p_sample->i_handler != ATOM_subt &&
               p_sample->i_handler != ATOM_sbtl ) ||
             !SetupSpuES( p_demux, p_track, p_sample ) )
           return VLC_EGENERIC;
        break;

    default:
        break;
    }

    if( pp_es )
        *pp_es = MP4_AddTrackES( p_demux->out, p_track );

    return ( !pp_es || *pp_es ) ? VLC_SUCCESS : VLC_EGENERIC;
}

/* *** Try to find nearest sync points *** */
static int TrackGetNearestSeekPoint( demux_t *p_demux, mp4_track_t *p_track,
                                     uint32_t i_sample, uint32_t *pi_sync_sample )
{
    int i_ret = VLC_EGENERIC;
    *pi_sync_sample = 0;

    const MP4_Box_t *p_stss;
    if( ( p_stss = MP4_BoxGet( p_track->p_stbl, "stss" ) ) )
    {
        const MP4_Box_data_stss_t *p_stss_data = BOXDATA(p_stss);
        msg_Dbg( p_demux, "track[Id 0x%x] using Sync Sample Box (stss)",
                 p_track->i_track_ID );
        for( unsigned i_index = 0; i_index < p_stss_data->i_entry_count; i_index++ )
        {
            if( i_index >= p_stss_data->i_entry_count - 1 ||
                i_sample < p_stss_data->i_sample_number[i_index+1] )
            {
                *pi_sync_sample = p_stss_data->i_sample_number[i_index];
                msg_Dbg( p_demux, "stss gives %d --> %" PRIu32 " (sample number)",
                         i_sample, *pi_sync_sample );
                i_ret = VLC_SUCCESS;
                break;
            }
        }
    }

    /* try rap samples groups */
    const MP4_Box_t *p_sbgp = MP4_BoxGet( p_track->p_stbl, "sbgp" );
    for( ; p_sbgp; p_sbgp = p_sbgp->p_next )
    {
        const MP4_Box_data_sbgp_t *p_sbgp_data = BOXDATA(p_sbgp);
        if( p_sbgp->i_type != ATOM_sbgp || !p_sbgp_data )
            continue;

        if( p_sbgp_data->i_grouping_type == SAMPLEGROUP_rap )
        {
            uint32_t i_group_sample = 0;
            for ( uint32_t i=0; i<p_sbgp_data->i_entry_count; i++ )
            {
                /* Sample belongs to rap group ? */
                if( p_sbgp_data->entries.pi_group_description_index[i] != 0 )
                {
                    if( i_sample < i_group_sample )
                    {
                        msg_Dbg( p_demux, "sbgp lookup failed %" PRIu32 " (sample number)",
                                 i_sample );
                        break;
                    }
                    else if ( i_sample >= i_group_sample &&
                              *pi_sync_sample < i_group_sample )
                    {
                        *pi_sync_sample = i_group_sample;
                        i_ret = VLC_SUCCESS;
                    }
                }
                i_group_sample += p_sbgp_data->entries.pi_sample_count[i];
            }

            if( i_ret == VLC_SUCCESS && *pi_sync_sample )
            {
                msg_Dbg( p_demux, "sbgp gives %d --> %" PRIu32 " (sample number)",
                         i_sample, *pi_sync_sample );
            }
        }
    }

    return i_ret;
}

/* given a time it return sample/chunk
 * it also update elst field of the track
 */
static int TrackTimeToSampleChunk( demux_t *p_demux, mp4_track_t *p_track,
                                   vlc_tick_t start, uint32_t *pi_chunk,
                                   uint32_t *pi_sample )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    uint64_t     i_dts;
    unsigned int i_sample;
    unsigned int i_chunk;
    int          i_index;
    stime_t      i_start;

    /* FIXME see if it's needed to check p_track->i_chunk_count */
    if( p_track->i_chunk_count == 0 )
        return( VLC_EGENERIC );

    /* handle elst (find the correct one) */
    MP4_TrackSetELST( p_demux, p_track, start );
    if( p_track->p_elst && p_track->BOXDATA(p_elst)->i_entry_count > 0 )
    {
        MP4_Box_data_elst_t *elst = p_track->BOXDATA(p_elst);
        int64_t i_mvt= MP4_rescale_qtime( start, p_sys->i_timescale );

        /* now calculate i_start for this elst */
        /* offset */
        if( start < MP4_rescale_mtime( p_track->i_elst_time, p_sys->i_timescale ) )
        {
            *pi_chunk = 0;
            *pi_sample= 0;

            return VLC_SUCCESS;
        }
        /* to track time scale */
        i_start  = MP4_rescale_qtime( start, p_track->i_timescale );
        /* add elst offset */
        if( ( elst->i_media_rate_integer[p_track->i_elst] > 0 ||
             elst->i_media_rate_fraction[p_track->i_elst] > 0 ) &&
            elst->i_media_time[p_track->i_elst] > 0 )
        {
            i_start += elst->i_media_time[p_track->i_elst];
        }

        msg_Dbg( p_demux, "elst (%d) gives %"PRId64"ms (movie)-> %"PRId64
                 "ms (track)", p_track->i_elst,
                 MP4_rescale( i_mvt, p_sys->i_timescale, 1000 ),
                 MP4_rescale( i_start, p_track->i_timescale, 1000 ) );
    }
    else
    {
        /* convert absolute time to in timescale unit */
        i_start = MP4_rescale_qtime( start, p_track->i_timescale );
    }

    /* we start from sample 0/chunk 0, hope it won't take too much time */
    /* *** find good chunk *** */
    for( i_chunk = 0; ; i_chunk++ )
    {
        if( i_chunk + 1 >= p_track->i_chunk_count )
        {
            /* at the end and can't check if i_start in this chunk,
               it will be check while searching i_sample */
            i_chunk = p_track->i_chunk_count - 1;
            break;
        }

        if( (uint64_t)i_start >= p_track->chunk[i_chunk].i_first_dts &&
            (uint64_t)i_start <  p_track->chunk[i_chunk + 1].i_first_dts )
        {
            break;
        }
    }

    /* *** find sample in the chunk *** */
    i_sample = p_track->chunk[i_chunk].i_sample_first;
    i_dts    = p_track->chunk[i_chunk].i_first_dts;
    for( i_index = 0; i_sample < p_track->chunk[i_chunk].i_sample_count; )
    {
        if( i_dts +
            p_track->chunk[i_chunk].p_sample_count_dts[i_index] *
            p_track->chunk[i_chunk].p_sample_delta_dts[i_index] < (uint64_t)i_start )
        {
            i_dts    +=
                p_track->chunk[i_chunk].p_sample_count_dts[i_index] *
                p_track->chunk[i_chunk].p_sample_delta_dts[i_index];

            i_sample += p_track->chunk[i_chunk].p_sample_count_dts[i_index];
            i_index++;
        }
        else
        {
            if( p_track->chunk[i_chunk].p_sample_delta_dts[i_index] <= 0 )
            {
                break;
            }
            i_sample += ( i_start - i_dts ) /
                p_track->chunk[i_chunk].p_sample_delta_dts[i_index];
            break;
        }
    }

    if( i_sample >= p_track->i_sample_count )
    {
        msg_Warn( p_demux, "track[Id 0x%x] will be disabled "
                  "(seeking too far) chunk=%d sample=%d",
                  p_track->i_track_ID, i_chunk, i_sample );
        return( VLC_EGENERIC );
    }


    /* *** Try to find nearest sync points *** */
    uint32_t i_sync_sample;
    if( VLC_SUCCESS ==
        TrackGetNearestSeekPoint( p_demux, p_track, i_sample, &i_sync_sample ) )
    {
        /* Go to chunk */
        if( i_sync_sample <= i_sample )
        {
            while( i_chunk > 0 &&
                   i_sync_sample < p_track->chunk[i_chunk].i_sample_first )
                i_chunk--;
        }
        else
        {
            while( i_chunk < p_track->i_chunk_count - 1 &&
                   i_sync_sample >= p_track->chunk[i_chunk].i_sample_first +
                                    p_track->chunk[i_chunk].i_sample_count )
                i_chunk++;
        }
        i_sample = i_sync_sample;
    }

    *pi_chunk  = i_chunk;
    *pi_sample = i_sample;

    return VLC_SUCCESS;
}

static int TrackGotoChunkSample( demux_t *p_demux, mp4_track_t *p_track,
                                 unsigned int i_chunk, unsigned int i_sample )
{
    bool b_reselect = false;

    /* now see if actual es is ok */
    if( p_track->i_chunk >= p_track->i_chunk_count ||
        p_track->chunk[p_track->i_chunk].i_sample_description_index !=
            p_track->chunk[i_chunk].i_sample_description_index )
    {
        msg_Warn( p_demux, "recreate ES for track[Id 0x%x]",
                  p_track->i_track_ID );

        es_out_Control( p_demux->out, ES_OUT_GET_ES_STATE,
                        p_track->p_es, &b_reselect );

        es_out_Del( p_demux->out, p_track->p_es );

        p_track->p_es = NULL;

        if( TrackCreateES( p_demux, p_track, i_chunk, &p_track->p_es ) )
        {
            msg_Err( p_demux, "cannot create es for track[Id 0x%x]",
                     p_track->i_track_ID );

            p_track->b_ok       = false;
            p_track->b_selected = false;
            return VLC_EGENERIC;
        }
    }

    /* select again the new decoder */
    if( b_reselect )
    {
        es_out_Control( p_demux->out, ES_OUT_SET_ES, p_track->p_es );
    }

    p_track->i_chunk    = i_chunk;
    p_track->chunk[i_chunk].i_sample = i_sample - p_track->chunk[i_chunk].i_sample_first;
    p_track->i_sample   = i_sample;

    return p_track->b_selected ? VLC_SUCCESS : VLC_EGENERIC;
}

static void MP4_TrackRestart( demux_t *p_demux, mp4_track_t *p_track,
                              MP4_Box_t *p_params_box )
{
    bool b_reselect = false;
    if( p_track->p_es )
    {
        es_out_Control( p_demux->out, ES_OUT_GET_ES_STATE,
                        p_track->p_es, &b_reselect );
    }

    /* Save previous fragmented pos */
    uint32_t i_sample_pos_backup = p_track->i_sample;
    vlc_tick_t time_backup = p_track->i_time;
    uint32_t timescale_backup = p_track->i_timescale;

    /* Save previous format and ES */
    es_format_t fmtbackup;
    es_out_id_t *p_es_backup = p_track->p_es;
    p_track->p_es = NULL;
    es_format_Copy( &fmtbackup, &p_track->fmt );
    es_format_Clean( &p_track->fmt );


    /* do the cleanup and recycle track / restart */
    //MP4_TrackDestroy( p_demux, p_track );
    //memset( p_track, 0, sizeof(*p_track) );

    assert(p_params_box->i_type == ATOM_trak);
   //? MP4_TrackCreate( p_demux, p_track, p_params_box, false, true );

    if( p_track->b_ok )
    {
        if( !es_format_IsSimilar( &fmtbackup, &p_track->fmt ) ||
            fmtbackup.i_extra != p_track->fmt.i_extra ||
            memcmp( fmtbackup.p_extra, p_track->fmt.p_extra, fmtbackup.i_extra ) )
        {
            if( p_es_backup )
                es_out_Del( p_demux->out, p_es_backup );

            if( !p_track->b_chapters_source )
            {
                p_track->p_es = MP4_AddTrackES( p_demux->out, p_track );
                p_track->b_ok = !!p_track->p_es;
            }
        }
        else
        {
            p_track->p_es = p_es_backup;
        }
    }
    else if( p_es_backup )
    {
        es_out_Del( p_demux->out, p_es_backup );
    }

    /* select again the new decoder */
    if( b_reselect && p_track->p_es )
        es_out_Control( p_demux->out, ES_OUT_SET_ES, p_track->p_es );

    es_format_Clean( &fmtbackup );

    /* Restore fragmented pos */
    p_track->i_sample = i_sample_pos_backup;
    p_track->i_time = MP4_rescale( time_backup, timescale_backup, p_track->i_timescale );
}

/****************************************************************************
 * MP4_TrackSetup:
 ****************************************************************************
 * Parse track information and create all needed data to run a track
 * If it succeed b_ok is set to 1 else to 0
 ****************************************************************************/
static void MP4_TrackSetup( demux_t *p_demux, mp4_track_t *p_track,
                             MP4_Box_t *p_box_trak,
                             bool b_create_es, bool b_force_enable )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    p_track->p_track = p_box_trak;

    char language[4] = { '\0' };
    char sdp_media_type[8] = { '\0' };

    const MP4_Box_t *p_tkhd = MP4_BoxGet( p_box_trak, "tkhd" );
    if( !p_tkhd )
    {
        return;
    }

    /* do we launch this track by default ? */
    p_track->b_enable =
        ( ( BOXDATA(p_tkhd)->i_flags&MP4_TRACK_ENABLED ) != 0 );

    p_track->i_track_ID = BOXDATA(p_tkhd)->i_track_ID;

    p_track->i_width = BOXDATA(p_tkhd)->i_width / BLOCK16x16;
    p_track->i_height = BOXDATA(p_tkhd)->i_height / BLOCK16x16;
    p_track->f_rotation = BOXDATA(p_tkhd)->f_rotation;

    /* FIXME: unhandled box: tref */

    const MP4_Box_t *p_mdhd = MP4_BoxGet( p_box_trak, "mdia/mdhd" );
    const MP4_Box_t *p_hdlr = MP4_BoxGet( p_box_trak, "mdia/hdlr" );

    if( ( !p_mdhd )||( !p_hdlr ) )
    {
        return;
    }

    if( BOXDATA(p_mdhd)->i_timescale == 0 )
    {
        msg_Warn( p_demux, "Invalid track timescale " );
        return;
    }
    p_track->i_timescale = BOXDATA(p_mdhd)->i_timescale;

    memcpy( &language, BOXDATA(p_mdhd)->rgs_language, 3 );
    p_track->b_mac_encoding = BOXDATA(p_mdhd)->b_mac_encoding;

    switch( p_hdlr->data.p_hdlr->i_handler_type )
    {
        case( ATOM_soun ):
            if( !MP4_BoxGet( p_box_trak, "mdia/minf/smhd" ) )
            {
                return;
            }
            es_format_Change( &p_track->fmt, AUDIO_ES, 0 );
            break;

        case( ATOM_pict ): /* heif */
            es_format_Change( &p_track->fmt, VIDEO_ES, 0 );
            break;

        case( ATOM_vide ):
            if( !MP4_BoxGet( p_box_trak, "mdia/minf/vmhd") )
            {
                return;
            }
            es_format_Change( &p_track->fmt, VIDEO_ES, 0 );
            break;

        case( ATOM_hint ):
            /* RTP Reception Hint tracks */
            if( !MP4_BoxGet( p_box_trak, "mdia/minf/hmhd" ) ||
                !MP4_BoxGet( p_box_trak, "mdia/minf/stbl/stsd/rrtp" ) )
            {
                break;
            }
            MP4_Box_t *p_sdp;

            /* parse the sdp message to find out whether the RTP stream contained audio or video */
            if( !( p_sdp  = MP4_BoxGet( p_box_trak, "udta/hnti/sdp " ) ) )
            {
                msg_Warn( p_demux, "Didn't find sdp box to determine stream type" );
                return;
            }

            memcpy( sdp_media_type, BOXDATA(p_sdp)->psz_text, 7 );
            if( !strcmp(sdp_media_type, "m=audio") )
            {
                msg_Dbg( p_demux, "Found audio Rtp: %s", sdp_media_type );
                es_format_Change( &p_track->fmt, AUDIO_ES, 0 );
            }
            else if( !strcmp(sdp_media_type, "m=video") )
            {
                msg_Dbg( p_demux, "Found video Rtp: %s", sdp_media_type );
                es_format_Change( &p_track->fmt, VIDEO_ES, 0 );
            }
            else
            {
                msg_Warn( p_demux, "Malformed track SDP message: %s", sdp_media_type );
                return;
            }
            p_track->p_sdp = p_sdp;
            break;

        case( ATOM_tx3g ):
        case( ATOM_text ):
        case( ATOM_subp ):
        case( ATOM_subt ): /* ttml */
        case( ATOM_sbtl ):
        case( ATOM_clcp ): /* closed captions */
            es_format_Change( &p_track->fmt, SPU_ES, 0 );
            break;

        default:
            return;
    }

    p_track->asfinfo.i_cat = p_track->fmt.i_cat;

    const MP4_Box_t *p_elst;
    p_track->i_elst = 0;
    p_track->i_elst_time = 0;
    if( ( p_track->p_elst = p_elst = MP4_BoxGet( p_box_trak, "edts/elst" ) ) )
    {
        MP4_Box_data_elst_t *elst = BOXDATA(p_elst);
        unsigned int i;

        msg_Warn( p_demux, "elst box found" );
        for( i = 0; i < elst->i_entry_count; i++ )
        {
            msg_Dbg( p_demux, "   - [%d] duration=%"PRId64"ms media time=%"PRId64
                     "ms) rate=%d.%d", i,
                     MP4_rescale( elst->i_segment_duration[i], p_sys->i_timescale, 1000 ),
                     elst->i_media_time[i] >= 0 ?
                        MP4_rescale( elst->i_media_time[i], p_track->i_timescale, 1000 ) :
                        INT64_C(-1),
                     elst->i_media_rate_integer[i],
                     elst->i_media_rate_fraction[i] );
        }
    }


/*  TODO
    add support for:
    p_dinf = MP4_BoxGet( p_minf, "dinf" );
*/
    if( !( p_track->p_stbl = MP4_BoxGet( p_box_trak,"mdia/minf/stbl" ) ) ||
        !( p_track->p_stsd = MP4_BoxGet( p_box_trak,"mdia/minf/stbl/stsd") ) )
    {
        return;
    }

    /* Set language */
    if( *language && strcmp( language, "```" ) && strcmp( language, "und" ) )
    {
        p_track->fmt.psz_language = strdup( language );
    }

    const MP4_Box_t *p_udta = MP4_BoxGet( p_box_trak, "udta" );
    if( p_udta )
    {
        const MP4_Box_t *p_box_iter;
        for( p_box_iter = p_udta->p_first; p_box_iter != NULL;
                 p_box_iter = p_box_iter->p_next )
        {
            switch( p_box_iter->i_type )
            {
                case ATOM_0xa9nam:
                case ATOM_name:
                    p_track->fmt.psz_description =
                        strndup( p_box_iter->data.p_binary->p_blob,
                                 p_box_iter->data.p_binary->i_blob );
                default:
                    break;
            }
        }
    }

    /* Create chunk index table and sample index table */
//    if( TrackCreateChunksIndex( p_demux,p_track  ) ||
//        TrackCreateSamplesIndex( p_demux, p_track ) )
//    {
//        msg_Err( p_demux, "cannot create chunks index" );
//        return; /* cannot create chunks index */
//    }

    p_track->i_chunk  = 0;
    p_track->i_sample = 0;

    /* Mark chapter only track */
    if( p_sys->p_tref_chap )
    {
        MP4_Box_data_tref_generic_t *p_chap = p_sys->p_tref_chap->data.p_tref_generic;
        unsigned int i;

        for( i = 0; i < p_chap->i_entry_count; i++ )
        {
            if( p_track->i_track_ID == p_chap->i_track_ID[i] &&
                p_track->fmt.i_cat == UNKNOWN_ES )
            {
                p_track->b_chapters_source = true;
                p_track->b_enable = false;
                break;
            }
        }
    }

    const MP4_Box_t *p_tsel;
    /* now create es */
    if( b_force_enable &&
        ( p_track->fmt.i_cat == VIDEO_ES || p_track->fmt.i_cat == AUDIO_ES ) )
    {
        msg_Warn( p_demux, "Enabling track[Id 0x%x] (buggy file without enabled track)",
                  p_track->i_track_ID );
        p_track->b_enable = true;
        p_track->b_selected = true;
        p_track->fmt.i_priority = ES_PRIORITY_SELECTABLE_MIN;
    }
    else if ( (p_tsel = MP4_BoxGet( p_box_trak, "udta/tsel" )) )
    {
        if ( BOXDATA(p_tsel) && BOXDATA(p_tsel)->i_switch_group )
        {
            p_track->i_switch_group = BOXDATA(p_tsel)->i_switch_group;
            int i_priority = ES_PRIORITY_SELECTABLE_MIN;
            for ( unsigned int i = 0; i < p_sys->i_tracks; i++ )
            {
                const mp4_track_t *p_other = &p_sys->track[i];
                if( p_other && p_other != p_track &&
                    p_other->fmt.i_cat == p_track->fmt.i_cat &&
                    p_track->i_switch_group == p_other->i_switch_group )
                        i_priority = __MAX( i_priority, p_other->fmt.i_priority + 1 );
            }
            /* VLC only support ES priority for AUDIO_ES and SPU_ES.
               If there's another VIDEO_ES in the same group, we need to unselect it then */
            if ( p_track->fmt.i_cat == VIDEO_ES && i_priority > ES_PRIORITY_SELECTABLE_MIN )
                p_track->fmt.i_priority = ES_PRIORITY_NOT_DEFAULTABLE;
            else
                p_track->fmt.i_priority = i_priority;
        }
    }
    /* If there's no tsel, try to enable the track coming first in edit list */
    else if ( p_track->p_elst && p_track->fmt.i_priority == ES_PRIORITY_SELECTABLE_MIN )
    {
#define MAX_SELECTABLE (INT_MAX - ES_PRIORITY_SELECTABLE_MIN)
        for ( uint32_t i=0; i<p_track->BOXDATA(p_elst)->i_entry_count; i++ )
        {
            if ( p_track->BOXDATA(p_elst)->i_media_time[i] >= 0 &&
                 p_track->BOXDATA(p_elst)->i_segment_duration[i] )
            {
                /* We do selection by inverting start time into priority.
                   The track with earliest edit will have the highest prio */
                const int i_time = __MIN( MAX_SELECTABLE, p_track->BOXDATA(p_elst)->i_media_time[i] );
                p_track->fmt.i_priority = ES_PRIORITY_SELECTABLE_MIN + MAX_SELECTABLE - i_time;
                break;
            }
        }
    }

    if( p_sys->hacks.es_cat_filters && (p_sys->hacks.es_cat_filters & p_track->fmt.i_cat) == 0 )
    {
        p_track->fmt.i_priority = ES_PRIORITY_NOT_DEFAULTABLE;
    }

    if( !p_track->b_enable )
        p_track->fmt.i_priority = ES_PRIORITY_NOT_DEFAULTABLE;

    if( TrackCreateES( p_demux,
                       p_track, p_track->i_chunk,
                      (p_track->b_chapters_source || !b_create_es) ? NULL : &p_track->p_es ) )
    {
        msg_Err( p_demux, "cannot create es for track[Id 0x%x]",
                 p_track->i_track_ID );
        return;
    }

    p_track->b_ok = true;
}

static void DestroyChunk( mp4_chunk_t *ck )
{
    free( ck->p_sample_count_dts );
    free( ck->p_sample_delta_dts );
    free( ck->p_sample_count_pts );
    free( ck->p_sample_offset_pts );
    free( ck->p_sample_size );
}

/****************************************************************************
 * MP4_TrackClean:
 ****************************************************************************
 * Cleans a track created by MP4_TrackCreate.
 ****************************************************************************/
static void MP4_TrackClean( es_out_t *out, mp4_track_t *p_track )
{
    es_format_Clean( &p_track->fmt );

    if( p_track->p_es )
        es_out_Del( out, p_track->p_es );

    if( p_track->chunk )
    {
        for( unsigned int i_chunk = 0; i_chunk < p_track->i_chunk_count; i_chunk++ )
            DestroyChunk( &p_track->chunk[i_chunk] );
    }
    free( p_track->chunk );

    if( !p_track->i_sample_size )
        free( p_track->p_sample_size );

    if ( p_track->asfinfo.p_frame )
        block_ChainRelease( p_track->asfinfo.p_frame );

    free( p_track->context.runs.p_array );
}

static void MP4_TrackInit( mp4_track_t *p_track )
{
    memset( p_track, 0, sizeof(mp4_track_t) );
    es_format_Init( &p_track->fmt, UNKNOWN_ES, 0 );
    p_track->i_timescale = 1;
}

static void MP4_TrackSelect( demux_t *p_demux, mp4_track_t *p_track, bool b_select )
{
    if( !p_track->b_ok || p_track->b_chapters_source )
        return;

    if( b_select == p_track->b_selected )
        return;

    if( !b_select && p_track->p_es )
    {
        es_out_Control( p_demux->out, ES_OUT_SET_ES_STATE,
                        p_track->p_es, false );
    }

    p_track->b_selected = b_select;
}

static int MP4_TrackSeek( demux_t *p_demux, mp4_track_t *p_track,
                          vlc_tick_t i_start )
{

	return VLC_SUCCESS;

	uint32_t i_chunk;
    uint32_t i_sample;

    if( !p_track->b_ok || p_track->b_chapters_source )
        return VLC_EGENERIC;

    p_track->b_selected = false;

    if( TrackTimeToSampleChunk( p_demux, p_track, i_start,
                                &i_chunk, &i_sample ) )
    {
        msg_Warn( p_demux, "cannot select track[Id 0x%x]",
                  p_track->i_track_ID );
        return VLC_EGENERIC;
    }

    p_track->b_selected = true;
    if( !TrackGotoChunkSample( p_demux, p_track, i_chunk, i_sample ) )
        p_track->b_selected = true;

    return p_track->b_selected ? VLC_SUCCESS : VLC_EGENERIC;
}


/*
 * 3 types: for audio
 *
 */
static inline uint32_t MP4_GetFixedSampleSize( const mp4_track_t *p_track,
                                               const MP4_Box_data_sample_soun_t *p_soun )
{
    uint32_t i_size = p_track->i_sample_size;

    assert( p_track->i_sample_size != 0 );

     /* QuickTime "built-in" support case fixups */
    if( p_track->fmt.i_cat == AUDIO_ES &&
        p_soun->i_compressionid == 0 && p_track->i_sample_size <= 2 )
    {
        switch( p_track->fmt.i_codec )
        {
        case VLC_CODEC_GSM:
            i_size = p_soun->i_channelcount;
            break;
        case VLC_FOURCC( 'N', 'O', 'N', 'E' ):
        case ATOM_twos:
        case ATOM_sowt:
        case ATOM_raw:
        case VLC_CODEC_S24L:
        case VLC_CODEC_S24B:
        case VLC_CODEC_S32L:
        case VLC_CODEC_S32B:
        case VLC_CODEC_F32L:
        case VLC_CODEC_F32B:
        case VLC_CODEC_F64L:
        case VLC_CODEC_F64B:
            if( p_track->i_sample_size < ((p_soun->i_samplesize+7U)/8U) * p_soun->i_channelcount )
                i_size = ((p_soun->i_samplesize+7)/8) * p_soun->i_channelcount;
            break;
        case VLC_CODEC_ALAW:
        case VLC_FOURCC( 'u', 'l', 'a', 'w' ):
            i_size = p_soun->i_channelcount;
            break;
        default:
            break;
        }
    }

    return i_size;
}

static uint32_t MP4_TrackGetReadSize( mp4_track_t *p_track, uint32_t *pi_nb_samples )
{
    uint32_t i_size = 0;
    *pi_nb_samples = 0;

    if ( p_track->i_sample == p_track->i_sample_count )
        return 0;

    if ( p_track->fmt.i_cat != AUDIO_ES )
    {
        *pi_nb_samples = 1;

        if( p_track->i_sample_size == 0 ) /* all sizes are different */
            return p_track->p_sample_size[p_track->i_sample];
        else
            return p_track->i_sample_size;
    }
    else
    {
        const MP4_Box_data_sample_soun_t *p_soun = p_track->p_sample->data.p_sample_soun;
        const mp4_chunk_t *p_chunk = &p_track->chunk[p_track->i_chunk];
        uint32_t i_max_samples = p_chunk->i_sample_count - p_chunk->i_sample;

        /* Group audio packets so we don't call demux for single sample unit */
        if( p_track->fmt.i_original_fourcc == VLC_CODEC_DVD_LPCM &&
            p_soun->i_constLPCMframesperaudiopacket &&
            p_soun->i_constbytesperaudiopacket )
        {
            /* uncompressed case */
            uint32_t i_packets = i_max_samples / p_soun->i_constLPCMframesperaudiopacket;
            if ( UINT32_MAX / p_soun->i_constbytesperaudiopacket < i_packets )
                i_packets = UINT32_MAX / p_soun->i_constbytesperaudiopacket;
            *pi_nb_samples = i_packets * p_soun->i_constLPCMframesperaudiopacket;
            return i_packets * p_soun->i_constbytesperaudiopacket;
        }

        if( p_track->fmt.i_original_fourcc == VLC_FOURCC('r','r','t','p') )
        {
            *pi_nb_samples = 1;
            return p_track->i_sample_size;
        }

        /* all samples have a different size */
        if( p_track->i_sample_size == 0 )
        {
            *pi_nb_samples = 1;
            return p_track->p_sample_size[p_track->i_sample];
        }

        if( p_soun->i_qt_version == 1 )
        {
            if ( p_soun->i_compressionid == 0xFFFE )
            {
                *pi_nb_samples = 1; /* != number of audio samples */
                if ( p_track->i_sample_size )
                    return p_track->i_sample_size;
                else
                    return p_track->p_sample_size[p_track->i_sample];
            }
            else if ( p_soun->i_compressionid != 0 || p_soun->i_bytes_per_sample > 1 ) /* compressed */
            {
                /* in this case we are dealing with compressed data
                   -2 in V1: additional fields are meaningless (VBR and such) */
                *pi_nb_samples = i_max_samples;//p_track->chunk[p_track->i_chunk].i_sample_count;
                if( p_track->fmt.audio.i_blockalign > 1 )
                    *pi_nb_samples = p_soun->i_sample_per_packet;
                i_size = *pi_nb_samples / p_soun->i_sample_per_packet * p_soun->i_bytes_per_frame;
                return i_size;
            }
            else /* uncompressed case */
            {
                uint32_t i_packets;
                if( p_track->fmt.audio.i_blockalign > 1 )
                    i_packets = 1;
                else
                    i_packets = i_max_samples / p_soun->i_sample_per_packet;

                if ( UINT32_MAX / p_soun->i_bytes_per_frame < i_packets )
                    i_packets = UINT32_MAX / p_soun->i_bytes_per_frame;

                *pi_nb_samples = i_packets * p_soun->i_sample_per_packet;
                i_size = i_packets * p_soun->i_bytes_per_frame;
                return i_size;
            }
        }

        /* uncompressed v0 (qt) or... not (ISO) */

        /* Quicktime built-in support handling */
        if( p_soun->i_compressionid == 0 && p_track->i_sample_size == 1 )
        {
            switch( p_track->fmt.i_codec )
            {
                /* sample size is not integer */
                case VLC_CODEC_GSM:
                    *pi_nb_samples = 160 * p_track->fmt.audio.i_channels;
                    return 33 * p_track->fmt.audio.i_channels;
                case VLC_CODEC_ADPCM_IMA_QT:
                    *pi_nb_samples = 64 * p_track->fmt.audio.i_channels;
                    return 34 * p_track->fmt.audio.i_channels;
                default:
                    break;
            }
        }

        /* More regular V0 cases */
        uint32_t i_max_v0_samples;
        switch( p_track->fmt.i_codec )
        {
            /* Compressed samples in V0 */
            case VLC_CODEC_AMR_NB:
            case VLC_CODEC_AMR_WB:
                i_max_v0_samples = 16;
                break;
            case VLC_CODEC_MPGA:
            case VLC_CODEC_MP2:
            case VLC_CODEC_MP3:
            case VLC_CODEC_DTS:
            case VLC_CODEC_MP4A:
            case VLC_CODEC_A52:
                i_max_v0_samples = 1;
                break;
                /* fixme, reverse using a list of uncompressed codecs */
            default:
                /* Read 25ms of samples (uncompressed) */
                i_max_v0_samples = p_track->fmt.audio.i_rate / 40 *
                                   p_track->fmt.audio.i_channels;
                if( i_max_v0_samples < 1 )
                    i_max_v0_samples = 1;
                break;
        }

        *pi_nb_samples = 0;
        for( uint32_t i=p_track->i_sample;
             i<p_chunk->i_sample_first+p_chunk->i_sample_count &&
             i<p_track->i_sample_count;
             i++ )
        {
            (*pi_nb_samples)++;
            if ( p_track->i_sample_size == 0 )
                i_size += p_track->p_sample_size[i];
            else
                i_size += MP4_GetFixedSampleSize( p_track, p_soun );

            /* Try to detect compression in ISO */
            if(p_soun->i_compressionid != 0)
            {
                /* Return only 1 sample */
                break;
            }

            if ( *pi_nb_samples == i_max_v0_samples )
                break;
        }
    }

    //fprintf( stderr, "size=%d\n", i_size );
    return i_size;
}

static uint64_t MP4_TrackGetPos( mp4_track_t *p_track )
{
    unsigned int i_sample;
    uint64_t i_pos;

    i_pos = p_track->chunk[p_track->i_chunk].i_offset;

    if( p_track->i_sample_size )
    {
        MP4_Box_data_sample_soun_t *p_soun =
            p_track->p_sample->data.p_sample_soun;

        /* Quicktime builtin support, _must_ ignore sample tables */
        if( p_track->fmt.i_cat == AUDIO_ES && p_soun->i_compressionid == 0 &&
            p_track->i_sample_size == 1 )
        {
            switch( p_track->fmt.i_codec )
            {
            case VLC_CODEC_GSM: /* # Samples > data size */
                i_pos += ( p_track->i_sample -
                           p_track->chunk[p_track->i_chunk].i_sample_first ) / 160 * 33;
                return i_pos;
            case VLC_CODEC_ADPCM_IMA_QT: /* # Samples > data size */
                i_pos += ( p_track->i_sample -
                           p_track->chunk[p_track->i_chunk].i_sample_first ) / 64 * 34;
                return i_pos;
            default:
                break;
            }
        }

        if( p_track->fmt.i_cat != AUDIO_ES || p_soun->i_qt_version == 0 ||
            p_track->fmt.audio.i_blockalign <= 1 ||
            p_soun->i_sample_per_packet * p_soun->i_bytes_per_frame == 0 )
        {
            i_pos += ( p_track->i_sample -
                       p_track->chunk[p_track->i_chunk].i_sample_first ) *
                     MP4_GetFixedSampleSize( p_track, p_soun );
        }
        else
        {
            /* we read chunk by chunk unless a blockalign is requested */
            i_pos += ( p_track->i_sample - p_track->chunk[p_track->i_chunk].i_sample_first ) /
                        p_soun->i_sample_per_packet * p_soun->i_bytes_per_frame;
        }
    }
    else
    {
        for( i_sample = p_track->chunk[p_track->i_chunk].i_sample_first;
             i_sample < p_track->i_sample; i_sample++ )
        {
            i_pos += p_track->p_sample_size[i_sample];
        }
    }

    return i_pos;
}

static int MP4_TrackNextSample( demux_t *p_demux, mp4_track_t *p_track, uint32_t i_samples )
{
    if ( UINT32_MAX - p_track->i_sample < i_samples )
    {
        p_track->i_sample = UINT32_MAX;
        return VLC_EGENERIC;
    }

    p_track->i_sample += i_samples;

    if( p_track->i_sample >= p_track->i_sample_count )
        return VLC_EGENERIC;

    /* Have we changed chunk ? */
    if( p_track->i_sample >=
            p_track->chunk[p_track->i_chunk].i_sample_first +
            p_track->chunk[p_track->i_chunk].i_sample_count )
    {
        if( TrackGotoChunkSample( p_demux, p_track, p_track->i_chunk + 1,
                                  p_track->i_sample ) )
        {
            msg_Warn( p_demux, "track[0x%x] will be disabled "
                      "(cannot restart decoder)", p_track->i_track_ID );
            MP4_TrackSelect( p_demux, p_track, false );
            return VLC_EGENERIC;
        }
    }

    /* Have we changed elst */
    if( p_track->p_elst && p_track->BOXDATA(p_elst)->i_entry_count > 0 )
    {
        demux_sys_t *p_sys = p_demux->p_sys;
        MP4_Box_data_elst_t *elst = p_track->BOXDATA(p_elst);
        uint64_t i_mvt = MP4_rescale_qtime( MP4_TrackGetDTS( p_demux, p_track ),
                                            p_sys->i_timescale );
        if( (unsigned int)p_track->i_elst < elst->i_entry_count &&
            i_mvt >= p_track->i_elst_time +
                     elst->i_segment_duration[p_track->i_elst] )
        {
            MP4_TrackSetELST( p_demux, p_track,
                              MP4_TrackGetDTS( p_demux, p_track ) );
        }
    }

    return VLC_SUCCESS;
}

static void MP4_TrackSetELST( demux_t *p_demux, mp4_track_t *tk,
                              vlc_tick_t i_time )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    int         i_elst_last = tk->i_elst;

    /* handle elst (find the correct one) */
    tk->i_elst      = 0;
    tk->i_elst_time = 0;
    if( tk->p_elst && tk->BOXDATA(p_elst)->i_entry_count > 0 )
    {
        MP4_Box_data_elst_t *elst = tk->BOXDATA(p_elst);
        int64_t i_mvt= MP4_rescale_qtime( i_time, p_sys->i_timescale );

        for( tk->i_elst = 0; (unsigned int)tk->i_elst < elst->i_entry_count; tk->i_elst++ )
        {
            uint64_t i_dur = elst->i_segment_duration[tk->i_elst];

            if( tk->i_elst_time <= i_mvt && i_mvt < tk->i_elst_time + i_dur )
            {
                break;
            }
            tk->i_elst_time += i_dur;
        }

        if( (unsigned int)tk->i_elst >= elst->i_entry_count )
        {
            /* msg_Dbg( p_demux, "invalid number of entry in elst" ); */
            tk->i_elst = elst->i_entry_count - 1;
            tk->i_elst_time -= elst->i_segment_duration[tk->i_elst];
        }

        if( elst->i_media_time[tk->i_elst] < 0 )
        {
            /* track offset */
            tk->i_elst_time += elst->i_segment_duration[tk->i_elst];
        }
    }
    if( i_elst_last != tk->i_elst )
    {
        msg_Warn( p_demux, "elst old=%d new=%d", i_elst_last, tk->i_elst );
        tk->i_next_block_flags |= BLOCK_FLAG_DISCONTINUITY;
    }
}

/******************************************************************************
 *     Here are the functions used for fragmented MP4
 *****************************************************************************/
/**
 * Re-init decoder.
 * \Note If we call that function too soon,
 * before the track has been selected by MP4_TrackSelect
 * (during the first execution of Demux), then the track gets disabled
 */
static int ReInitDecoder( demux_t *p_demux, const MP4_Box_t *p_root,
                          mp4_track_t *p_track )
{
    MP4_Box_t *p_paramsbox = MP4_BoxGet( p_root, "/moov/trak[0]" );
    if( !p_paramsbox )
        return VLC_EGENERIC;

    MP4_TrackRestart( p_demux, p_track, p_paramsbox );

    /* Temporary hack until we support track selection */
    p_track->b_selected = true;
    p_track->b_enable = true;

    return VLC_SUCCESS;
}

static stime_t GetCumulatedDuration( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    stime_t i_max_duration = 0;

    for ( unsigned int i=0; i<p_sys->i_tracks; i++ )
    {
        stime_t i_track_duration = 0;
        MP4_Box_t *p_trak = MP4_GetTrakByTrackID( p_sys->p_moov, p_sys->track[i].i_track_ID );
        const MP4_Box_t *p_stsz;
        const MP4_Box_t *p_tkhd;
        if ( (p_tkhd = MP4_BoxGet( p_trak, "tkhd" )) &&
             (p_stsz = MP4_BoxGet( p_trak, "mdia/minf/stbl/stsz" )) &&
             /* duration might be wrong an be set to whole duration :/ */
             BOXDATA(p_stsz)->i_sample_count > 0 )
        {
            i_max_duration = __MAX( (uint64_t)i_max_duration, BOXDATA(p_tkhd)->i_duration );
        }

        if( p_sys->p_fragsindex )
        {
            i_track_duration += MP4_Fragment_Index_GetTrackDuration( p_sys->p_fragsindex, i );
        }

        i_max_duration = __MAX( i_max_duration, i_track_duration );
    }

    return i_max_duration;
}

static int ProbeIndex( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    uint64_t i_stream_size;
    uint8_t mfro[MP4_MFRO_BOXSIZE];
    assert( p_sys->b_seekable );

    if ( MP4_BoxCount( p_sys->p_root, "/mfra" ) )
        return VLC_EGENERIC;

    i_stream_size = stream_Size( p_sys->s_frag );
    if ( ( i_stream_size >> 62 ) ||
         ( i_stream_size < MP4_MFRO_BOXSIZE ) ||
         ( vlc_stream_Seek( p_sys->s_frag, i_stream_size - MP4_MFRO_BOXSIZE ) != VLC_SUCCESS )
       )
    {
        msg_Dbg( p_demux, "Probing tail for mfro has failed" );
        return VLC_EGENERIC;
    }

    if ( vlc_stream_Read( p_sys->s_frag, &mfro, MP4_MFRO_BOXSIZE ) == MP4_MFRO_BOXSIZE &&
         VLC_FOURCC(mfro[4],mfro[5],mfro[6],mfro[7]) == ATOM_mfro &&
         GetDWBE( &mfro ) == MP4_MFRO_BOXSIZE )
    {
        uint32_t i_offset = GetDWBE( &mfro[12] );
        msg_Dbg( p_demux, "will read mfra index at %"PRIu64, i_stream_size - i_offset );
        if ( i_stream_size > i_offset &&
             vlc_stream_Seek( p_sys->s_frag, i_stream_size - i_offset ) == VLC_SUCCESS )
        {
            msg_Dbg( p_demux, "reading mfra index at %"PRIu64, i_stream_size - i_offset );
            const uint32_t stoplist[] = { ATOM_mfra, 0 };
            MP4_ReadBoxContainerChildren( p_sys->s_frag, p_sys->p_root, stoplist );
        }
        return VLC_SUCCESS;
    }
    return VLC_EGENERIC;
}

static stime_t GetMoovTrackDuration( demux_sys_t *p_sys, unsigned i_track_ID )
{
    MP4_Box_t *p_trak = MP4_GetTrakByTrackID( p_sys->p_moov, i_track_ID );
    const MP4_Box_t *p_stsz;
    const MP4_Box_t *p_tkhd;
    if ( (p_tkhd = MP4_BoxGet( p_trak, "tkhd" )) &&
         (p_stsz = MP4_BoxGet( p_trak, "mdia/minf/stbl/stsz" )) &&
         /* duration might be wrong an be set to whole duration :/ */
         BOXDATA(p_stsz)->i_sample_count > 0 )
    {
        if( BOXDATA(p_tkhd)->i_duration <= p_sys->i_moov_duration )
            return BOXDATA(p_tkhd)->i_duration; /* In movie / mvhd scale */
        else
            return p_sys->i_moov_duration;
    }
    return 0;
}

static bool GetMoofTrackDuration( MP4_Box_t *p_moov, MP4_Box_t *p_moof,
                                  unsigned i_track_ID, stime_t *p_duration )
{
    if ( !p_moof || !p_moov )
        return false;

    MP4_Box_t *p_traf = MP4_BoxGet( p_moof, "traf" );
    while ( p_traf )
    {
        if ( p_traf->i_type != ATOM_traf )
        {
           p_traf = p_traf->p_next;
           continue;
        }

        const MP4_Box_t *p_tfhd = MP4_BoxGet( p_traf, "tfhd" );
        const MP4_Box_t *p_trun = MP4_BoxGet( p_traf, "trun" );
        if ( !p_tfhd || !p_trun || i_track_ID != BOXDATA(p_tfhd)->i_track_ID )
        {
           p_traf = p_traf->p_next;
           continue;
        }

        uint32_t i_track_timescale = 0;
        uint32_t i_track_defaultsampleduration = 0;

        /* set trex for defaults */
        MP4_Box_t *p_trex = MP4_GetTrexByTrackID( p_moov, BOXDATA(p_tfhd)->i_track_ID );
        if ( p_trex )
        {
            i_track_defaultsampleduration = BOXDATA(p_trex)->i_default_sample_duration;
        }

        MP4_Box_t *p_trak = MP4_GetTrakByTrackID( p_moov, BOXDATA(p_tfhd)->i_track_ID );
        if ( p_trak )
        {
            MP4_Box_t *p_mdhd = MP4_BoxGet( p_trak, "mdia/mdhd" );
            if ( p_mdhd )
                i_track_timescale = BOXDATA(p_mdhd)->i_timescale;
        }

        if ( !i_track_timescale )
        {
           p_traf = p_traf->p_next;
           continue;
        }

        uint64_t i_traf_duration = 0;
        while ( p_trun && p_tfhd )
        {
            if ( p_trun->i_type != ATOM_trun )
            {
               p_trun = p_trun->p_next;
               continue;
            }
            const MP4_Box_data_trun_t *p_trundata = p_trun->data.p_trun;

            /* Sum total time */
            if ( p_trundata->i_flags & MP4_TRUN_SAMPLE_DURATION )
            {
                for( uint32_t i=0; i< p_trundata->i_sample_count; i++ )
                    i_traf_duration += p_trundata->p_samples[i].i_duration;
            }
            else if ( BOXDATA(p_tfhd)->i_flags & MP4_TFHD_DFLT_SAMPLE_DURATION )
            {
                i_traf_duration += p_trundata->i_sample_count *
                        BOXDATA(p_tfhd)->i_default_sample_duration;
            }
            else
            {
                i_traf_duration += p_trundata->i_sample_count *
                        i_track_defaultsampleduration;
            }

            p_trun = p_trun->p_next;
        }

        *p_duration = i_traf_duration;
        break;
    }

    return true;
}

static int ProbeFragments( demux_t *p_demux, bool b_force, bool *pb_fragmented )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    msg_Dbg( p_demux, "probing fragments from %"PRId64, vlc_stream_Tell( p_sys->s_frag ) );

    assert( p_sys->p_root );

    MP4_Box_t *p_vroot = MP4_BoxNew(ATOM_root);
    if( !p_vroot )
        return VLC_EGENERIC;

    if( p_sys->b_seekable && (p_sys->b_fastseekable || b_force) )
    {
        MP4_ReadBoxContainerChildren( p_sys->s_frag, p_vroot, NULL ); /* Get the rest of the file */
        p_sys->b_fragments_probed = true;

        const unsigned i_moof = MP4_BoxCount( p_vroot, "/moof" );
        if( i_moof )
        {
            *pb_fragmented = true;
            p_sys->p_fragsindex = MP4_Fragments_Index_New( p_sys->i_tracks, i_moof );
            if( !p_sys->p_fragsindex )
            {
                MP4_BoxFree( p_vroot );
                return VLC_EGENERIC;
            }

            stime_t *pi_track_times = calloc( p_sys->i_tracks, sizeof(*pi_track_times) );
            if( !pi_track_times )
            {
                MP4_Fragments_Index_Delete( p_sys->p_fragsindex );
                p_sys->p_fragsindex = NULL;
                MP4_BoxFree( p_vroot );
                return VLC_EGENERIC;
            }

            unsigned index = 0;

            for( MP4_Box_t *p_moof = p_vroot->p_first; p_moof; p_moof = p_moof->p_next )
            {
                if( p_moof->i_type != ATOM_moof )
                    continue;

                for( unsigned i=0; i<p_sys->i_tracks; i++ )
                {
                    MP4_Box_t *p_tfdt = NULL;
                    MP4_Box_t *p_traf = MP4_GetTrafByTrackID( p_moof, p_sys->track[i].i_track_ID );
                    if( p_traf )
                        p_tfdt = MP4_BoxGet( p_traf, "tfdt" );

                    if( p_tfdt && BOXDATA(p_tfdt) )
                    {
                        pi_track_times[i] = p_tfdt->data.p_tfdt->i_base_media_decode_time;
                    }
                    else if( index == 0 ) /* Set first fragment time offset from moov */
                    {
                        stime_t i_duration = GetMoovTrackDuration( p_sys, p_sys->track[i].i_track_ID );
                        pi_track_times[i] = MP4_rescale( i_duration, p_sys->i_timescale, p_sys->track[i].i_timescale );
                    }

                    stime_t i_movietime = MP4_rescale( pi_track_times[i], p_sys->track[i].i_timescale, p_sys->i_timescale );
                    p_sys->p_fragsindex->p_times[index * p_sys->i_tracks + i] = i_movietime;

                    stime_t i_duration = 0;
                    if( GetMoofTrackDuration( p_sys->p_moov, p_moof, p_sys->track[i].i_track_ID, &i_duration ) )
                        pi_track_times[i] += i_duration;
                }

                p_sys->p_fragsindex->pi_pos[index++] = p_moof->i_pos;
            }

            for( unsigned i=0; i<p_sys->i_tracks; i++ )
            {
                stime_t i_movietime = MP4_rescale( pi_track_times[i], p_sys->track[i].i_timescale, p_sys->i_timescale );
                if( p_sys->p_fragsindex->i_last_time < i_movietime )
                    p_sys->p_fragsindex->i_last_time = i_movietime;
            }

            free( pi_track_times );
#ifdef MP4_VERBOSE
            MP4_Fragments_Index_Dump( VLC_OBJECT(p_demux), p_sys->p_fragsindex, p_sys->i_timescale );
#endif
        }
    }
    else
    {
        /* We stop at first moof, which validates our fragmentation condition
         * and we'll find others while reading. */
        const uint32_t excllist[] = { ATOM_moof, 0 };
        MP4_ReadBoxContainerRestricted( p_sys->s_frag, p_vroot, NULL, excllist );
        /* Peek since we stopped before restriction */
        const uint8_t *p_peek;
        if ( vlc_stream_Peek( p_sys->s_frag, &p_peek, 8 ) == 8 )
            *pb_fragmented = (VLC_FOURCC( p_peek[4], p_peek[5], p_peek[6], p_peek[7] ) == ATOM_moof);
        else
            *pb_fragmented = false;
    }

    MP4_BoxFree( p_vroot );

    MP4_Box_t *p_mehd = MP4_BoxGet( p_sys->p_moov, "mvex/mehd");
    if ( !p_mehd )
           p_sys->i_cumulated_duration = GetCumulatedDuration( p_demux );

    return VLC_SUCCESS;
}

static void FragResetContext( demux_sys_t *p_sys )
{
    if( p_sys->context.p_fragment_atom )
    {
        if( p_sys->context.p_fragment_atom != p_sys->p_moov )
            MP4_BoxFree( p_sys->context.p_fragment_atom );
        p_sys->context.p_fragment_atom = NULL;
    }
    p_sys->context.i_current_box_type = 0;

    for ( uint32_t i=0; i<p_sys->i_tracks; i++ )
    {
        mp4_track_t *p_track = &p_sys->track[i];
        p_track->context.i_default_sample_size = 0;
        p_track->context.i_default_sample_duration = 0;
    }
}

static int FragDemuxTrack( demux_t *p_demux, mp4_track_t *p_track,
                           vlc_tick_t i_max_preload )
{

    demux_sys_t *p_sys = p_demux->p_sys;

    if( !p_track->b_ok ||
         p_track->context.runs.i_current >= p_track->context.runs.i_count )
        return VLC_DEMUXER_EOS;

    const MP4_Box_data_trun_t *p_trun =
            p_track->context.runs.p_array[p_track->context.runs.i_current].p_trun->data.p_trun;

    if( p_track->context.i_trun_sample >= p_trun->i_sample_count )
        return VLC_DEMUXER_EOS;

    uint32_t dur = p_track->context.i_default_sample_duration,
             len = p_track->context.i_default_sample_size;

    if( vlc_stream_Tell(p_sys->s_frag) != p_track->context.i_trun_sample_pos &&
        MP4_Seek( p_sys->s_frag, p_track->context.i_trun_sample_pos ) != VLC_SUCCESS ) {
        msg_Info(p_demux, "%d:FragDemuxTrack, p_sys->s_frag should be eof: %d, pos: %llu", __LINE__, vlc_stream_Eof(p_sys->s_frag), vlc_stream_Tell(p_sys->s_frag));

        return VLC_DEMUXER_FATAL; //VLC_DEMUXER_EOF;
    }

    const stime_t i_demux_max_dts = (i_max_preload < INVALID_PRELOAD) ?
                p_track->i_time + MP4_rescale_qtime( i_max_preload, p_track->i_timescale ) :
                INT64_MAX;

    for( uint32_t i = p_track->context.i_trun_sample; i < p_trun->i_sample_count; i++ )
    {
        const stime_t i_dts = p_track->i_time;
        stime_t i_pts = i_dts;

        if( p_trun->i_flags & MP4_TRUN_SAMPLE_DURATION )
            dur = p_trun->p_samples[i].i_duration;

        if( i_dts > i_demux_max_dts ) {
            msg_Info(p_demux, "%d:FragDemuxTrack, i_dts:%llu > i_demux_max_pts:%llu", __LINE__, i_dts, i_demux_max_dts);

            return VLC_DEMUXER_SUCCESS;
        }

        p_track->i_time += dur;
        p_track->context.i_trun_sample = i + 1;

        if( p_trun->i_flags & MP4_TRUN_SAMPLE_TIME_OFFSET )
        {
            if ( p_trun->i_version == 1 )
                i_pts += p_trun->p_samples[i].i_composition_time_offset.v1;
            else if( p_trun->p_samples[i].i_composition_time_offset.v0 < 0xFF000000 )
                i_pts += p_trun->p_samples[i].i_composition_time_offset.v0;
            else /* version 0 with negative */
                i_pts += p_trun->p_samples[i].i_composition_time_offset.v1;
        }

        if( p_trun->i_flags & MP4_TRUN_SAMPLE_SIZE )
            len = p_trun->p_samples[i].i_size;

        if( !dur )
            msg_Warn(p_demux, "Zero duration sample in trun.");

        if( !len )
            msg_Warn(p_demux, "Zero length sample in trun.");

        block_t *p_block = vlc_stream_Block( p_sys->s_frag, len );
        uint32_t i_read = ( p_block ) ? p_block->i_buffer : 0;
        p_track->context.i_trun_sample_pos += i_read;
        if( i_read < len || p_block == NULL )
        {
            if( p_block )
                block_Release( p_block );
            return VLC_DEMUXER_FATAL;
        }

#if 0
        msg_Dbg( p_demux, "tk(%i)=%"PRId64" mv=%"PRId64" pos=%"PRIu64, p_track->i_track_ID,
                 VLC_TICK_0 + MP4_rescale_mtime( i_dts, p_track->i_timescale ),
                 VLC_TICK_0 + MP4_rescale_mtime( i_pts, p_track->i_timescale ),
                 p_track->context.i_trun_sample_pos );
#endif
        if ( p_track->p_es )
        {
            p_block->i_dts = VLC_TICK_0 + MP4_rescale_mtime( i_dts, p_track->i_timescale );
            if( p_track->fmt.i_cat == VIDEO_ES && !( p_trun->i_flags & MP4_TRUN_SAMPLE_TIME_OFFSET ) )
                p_block->i_pts = VLC_TICK_INVALID;
            else
                p_block->i_pts = VLC_TICK_0 + MP4_rescale_mtime( i_pts, p_track->i_timescale );
            p_block->i_length = MP4_rescale_mtime( dur, p_track->i_timescale );
            MP4_Block_Send( p_demux, p_track, p_block );
        }
        else block_Release( p_block );
    }

    if( p_track->context.i_trun_sample == p_trun->i_sample_count )
    {
        p_track->context.i_trun_sample = 0;
        if( ++p_track->context.runs.i_current < p_track->context.runs.i_count )
        {
            p_track->i_time = p_track->context.runs.p_array[p_track->context.runs.i_current].i_first_dts;
            p_track->context.i_trun_sample_pos = p_track->context.runs.p_array[p_track->context.runs.i_current].i_offset;
        }
    }

    return VLC_DEMUXER_SUCCESS;
}

static int DemuxMoof( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    int i_status;

    const vlc_tick_t i_max_preload = ( p_sys->b_fastseekable ) ? 0 : ( p_sys->b_seekable ) ? DEMUX_TRACK_MAX_PRELOAD : INVALID_PRELOAD;

    const vlc_tick_t i_nztime = MP4_GetMoviePTS( p_sys );

    /* !important! Ensure clock is set before sending data */
    if( p_sys->i_pcr == VLC_TICK_INVALID )
        es_out_SetPCR( p_demux->out, VLC_TICK_0 + i_nztime );

    /* demux up to increment amount of data on every track, or just set pcr if empty data */
    for( ;; )
    {
        msg_Info(p_demux, "%d:DemuxMoof", __LINE__);

        mp4_track_t *tk = NULL;
        i_status = VLC_DEMUXER_EOS;

        /* First pass, find any track within our target increment, ordered by position */
        for( unsigned i = 0; i < p_sys->i_tracks; i++ )
        {
            mp4_track_t *tk_tmp = &p_sys->track[i];

            if( !tk_tmp->b_ok || tk_tmp->b_chapters_source ||
               (!tk_tmp->b_selected && !p_sys->b_seekable) ||
                tk_tmp->context.runs.i_current >= tk_tmp->context.runs.i_count )
                continue;

            /* At least still have data to demux on this or next turns */
            i_status = VLC_DEMUXER_SUCCESS;
            msg_Info(p_demux, "%d:DemuxMoof", __LINE__);

            if( MP4_rescale_mtime( tk_tmp->i_time, tk_tmp->i_timescale ) <= i_nztime + DEMUX_INCREMENT )
            {
                if( tk == NULL || tk_tmp->context.i_trun_sample_pos < tk->context.i_trun_sample_pos )
                    tk = tk_tmp;
            }
        }

        msg_Info(p_demux, "%d:DemuxFrag", __LINE__);

        if( tk )
        {
            /* Second pass, refine and find any best candidate having a chunk pos closer than
             * current candidate (avoids seeks when increment falls between the 2) from
             * current position, but within extended interleave time */
            for( unsigned i = 0; i_max_preload != 0 && i < p_sys->i_tracks; i++ )
            {
                mp4_track_t *tk_tmp = &p_sys->track[i];
                if( tk_tmp == tk ||
                    !tk_tmp->b_ok || tk_tmp->b_chapters_source ||
                   (!tk_tmp->b_selected && !p_sys->b_seekable) ||
                    tk_tmp->context.runs.i_current >= tk_tmp->context.runs.i_count )
                    continue;

                vlc_tick_t i_nzdts = MP4_rescale_mtime( tk_tmp->i_time, tk_tmp->i_timescale );
                if ( i_nzdts <= i_nztime + DEMUX_TRACK_MAX_PRELOAD )
                {
                    /* Found a better candidate to avoid seeking */
                    if( tk_tmp->context.i_trun_sample_pos < tk->context.i_trun_sample_pos )
                        tk = tk_tmp;
                    /* Note: previous candidate will be repicked on next loop */
                }
            }
            msg_Info(p_demux, "%d:DemuxMoof", __LINE__);

            int i_ret = FragDemuxTrack( p_demux, tk, i_max_preload );

            msg_Info(p_demux, "%d:DemuxMoof, i_ret from FragDemuxTrack is: %d", __LINE__, i_ret);

            if( i_ret == VLC_DEMUXER_SUCCESS )
                i_status = VLC_DEMUXER_SUCCESS;
            else if( i_ret == VLC_DEMUXER_FATAL )
                i_status = VLC_DEMUXER_EOF;
        }
        msg_Info(p_demux, "%d:DemuxMoof, i_status is: %d", __LINE__, i_status);

        if( i_status != VLC_DEMUXER_SUCCESS || !tk )
            break;
    }
    msg_Info(p_demux, "%d:DemuxMoof", __LINE__);

    if( i_status != VLC_DEMUXER_EOS )
    {
        p_sys->i_nztime += DEMUX_INCREMENT;
        p_sys->i_pcr = VLC_TICK_0 + p_sys->i_nztime;
        es_out_SetPCR( p_demux->out, p_sys->i_pcr );
    }
    else
    {
        vlc_tick_t i_segment_end = INT64_MAX;
        for( unsigned i = 0; i < p_sys->i_tracks; i++ )
        {
            msg_Info(p_demux, "%d:DemuxMoof", __LINE__);

            mp4_track_t *tk = &p_sys->track[i];
            if( tk->b_ok || tk->b_chapters_source ||
               (!tk->b_selected && !p_sys->b_seekable) )
                continue;

            msg_Info(p_demux, "%d:DemuxMoof", __LINE__);

            vlc_tick_t i_track_end = MP4_rescale_mtime( tk->i_time, tk->i_timescale );
            msg_Info(p_demux, "%d:DemuxMoof", __LINE__);

            if( i_track_end < i_segment_end  )
                i_segment_end = i_track_end;
        }
        if( i_segment_end != INT64_MAX )
        {
            p_sys->i_nztime = i_segment_end;
            p_sys->i_pcr = VLC_TICK_0 + p_sys->i_nztime;
            es_out_SetPCR( p_demux->out, p_sys->i_pcr );
        }
    }

    return i_status;
}

static int FragCreateTrunIndex( demux_t *p_demux, MP4_Box_t *p_moof,
                                MP4_Box_t *p_chunksidx, stime_t i_moof_time )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    uint64_t i_traf_base_data_offset = p_moof->i_pos;
    uint32_t i_traf = 0;
    uint64_t i_prev_traf_end = 0;

    for( unsigned i=0; i<p_sys->i_tracks; i++ )
    {
        mp4_track_t *p_track = &p_sys->track[i];
        if( p_track->context.runs.p_array )
            free( p_track->context.runs.p_array );
        p_track->context.runs.p_array = NULL;
        p_track->context.runs.i_count = 0;
        p_track->context.runs.i_current = 0;
    }

    for( MP4_Box_t *p_traf = MP4_BoxGet( p_moof, "traf" );
                    p_traf ; p_traf = p_traf->p_next )
    {
        if ( p_traf->i_type != ATOM_traf )
            continue;

        const MP4_Box_t *p_tfhd = MP4_BoxGet( p_traf, "tfhd" );
        const uint32_t i_trun_count = MP4_BoxCount( p_traf, "trun" );
        if ( !p_tfhd || !i_trun_count )
            continue;

        mp4_track_t *p_track = MP4_GetTrackByTrackID( p_demux, BOXDATA(p_tfhd)->i_track_ID );
        if( !p_track )
            continue;

        p_track->context.runs.p_array = calloc(i_trun_count, sizeof(mp4_run_t));
        if(!p_track->context.runs.p_array)
            continue;

        /* Get defaults for this/these RUN */
        uint32_t i_track_defaultsamplesize = 0;
        uint32_t i_track_defaultsampleduration = 0;
        MP4_GetDefaultSizeAndDuration( p_sys->p_moov, BOXDATA(p_tfhd),
                                       &i_track_defaultsamplesize,
                                       &i_track_defaultsampleduration );
        p_track->context.i_default_sample_size = i_track_defaultsamplesize;
        p_track->context.i_default_sample_duration = i_track_defaultsampleduration;

        stime_t  i_traf_start_time = p_track->i_time;
        bool     b_has_base_media_decode_time = false;

        if( p_track->context.b_resync_time_offset ) /* We NEED start time offset for each track */
        {
            p_track->context.b_resync_time_offset = false;

            /* Find start time */
            const MP4_Box_t *p_tfdt = MP4_BoxGet( p_traf, "tfdt" );
            if( p_tfdt )
            {
                i_traf_start_time = BOXDATA(p_tfdt)->i_base_media_decode_time;
                b_has_base_media_decode_time = true;
            }

            /* Try using Tfxd for base offset (Smooth) */
            if( !b_has_base_media_decode_time && p_sys->i_tracks == 1 )
            {
                const MP4_Box_t *p_uuid = MP4_BoxGet( p_traf, "uuid" );
                for( ; p_uuid; p_uuid = p_uuid->p_next )
                {
                    if( p_uuid->i_type == ATOM_uuid &&
                       !CmpUUID( &p_uuid->i_uuid, &TfxdBoxUUID ) && p_uuid->data.p_tfxd )
                    {
                        i_traf_start_time = p_uuid->data.p_tfxd->i_fragment_abs_time;
                        b_has_base_media_decode_time = true;
                        break;
                    }
                }
            }

            /* After seek we should have probed fragments */
            if( !b_has_base_media_decode_time && p_sys->p_fragsindex )
            {
                unsigned i_track_index = (p_track - p_sys->track);
                assert(&p_sys->track[i_track_index] == p_track);
                i_traf_start_time = MP4_Fragment_Index_GetTrackStartTime( p_sys->p_fragsindex,
                                                                          i_track_index, p_moof->i_pos );
                i_traf_start_time = MP4_rescale( i_traf_start_time,
                                                 p_sys->i_timescale, p_track->i_timescale );
                b_has_base_media_decode_time = true;
            }

            if( !b_has_base_media_decode_time && p_chunksidx )
            {
                /* Try using SIDX as base offset.
                 * This can not work for global sidx but only when sent within each fragment (dash) */
                const MP4_Box_data_sidx_t *p_data = p_chunksidx->data.p_sidx;
                if( p_data && p_data->i_timescale && p_data->i_reference_count == 1 )
                {
                    i_traf_start_time = MP4_rescale( p_data->i_earliest_presentation_time,
                                                     p_data->i_timescale, p_track->i_timescale );
                    b_has_base_media_decode_time = true;
                }
            }

            /* First contiguous segment (moov->moof) and there's no tfdt not probed index (yet) */
            if( !b_has_base_media_decode_time && FragGetMoofSequenceNumber( p_moof ) == 1 )
            {
                i_traf_start_time = MP4_rescale( GetMoovTrackDuration( p_sys, p_track->i_track_ID ),
                                                 p_sys->i_timescale, p_track->i_timescale );
                b_has_base_media_decode_time = true;
            }

            /* Use global sidx moof time, in case moof does not carry tfdt */
            if( !b_has_base_media_decode_time && i_moof_time != INVALID_SEGMENT_TIME )
                i_traf_start_time = MP4_rescale( i_moof_time, p_sys->i_timescale, p_track->i_timescale );

            /* That should not happen */
            if( !b_has_base_media_decode_time )
                i_traf_start_time = MP4_rescale_qtime( p_sys->i_nztime, p_track->i_timescale );
        }

        /* Parse TRUN data */

        if ( BOXDATA(p_tfhd)->i_flags & MP4_TFHD_BASE_DATA_OFFSET )
        {
            i_traf_base_data_offset = BOXDATA(p_tfhd)->i_base_data_offset;
        }
        /* ignored if MP4_TFHD_BASE_DATA_OFFSET */
        else if ( BOXDATA(p_tfhd)->i_flags & MP4_TFHD_DEFAULT_BASE_IS_MOOF )
        {
            i_traf_base_data_offset = p_moof->i_pos /* + 8*/;
        }
        else
        {
            if ( i_traf == 0 )
                i_traf_base_data_offset = p_moof->i_pos /*+ 8*/;
            else
                i_traf_base_data_offset = i_prev_traf_end;
        }

        uint64_t i_trun_dts = i_traf_start_time;
        uint64_t i_trun_data_offset = i_traf_base_data_offset;
        uint32_t i_trun_size = 0;

        for( const MP4_Box_t *p_trun = MP4_BoxGet( p_traf, "trun" );
                              p_trun && p_tfhd;  p_trun = p_trun->p_next )
        {
            if ( p_trun->i_type != ATOM_trun )
               continue;

            const MP4_Box_data_trun_t *p_trundata = p_trun->data.p_trun;

            /* Get data offset */
            if ( p_trundata->i_flags & MP4_TRUN_DATA_OFFSET )
            {
                /* Fix for broken Trun data offset relative to tfhd instead of moof, as seen in smooth */
                if( (BOXDATA(p_tfhd)->i_flags & MP4_TFHD_BASE_DATA_OFFSET) == 0 &&
                    i_traf == 0 &&
                    i_traf_base_data_offset + p_trundata->i_data_offset < p_moof->i_pos + p_moof->i_size + 8 )
                {
                    i_trun_data_offset += p_moof->i_size + 8;
                }
                else if( (BOXDATA(p_tfhd)->i_flags & MP4_TFHD_BASE_DATA_OFFSET) )
                {
                    i_trun_data_offset = BOXDATA(p_tfhd)->i_base_data_offset + p_trundata->i_data_offset;
                }
                /* ignored if MP4_TFHD_BASE_DATA_OFFSET */
                else if ( BOXDATA(p_tfhd)->i_flags & MP4_TFHD_DEFAULT_BASE_IS_MOOF )
                {
                    i_trun_data_offset = p_moof->i_pos + p_trundata->i_data_offset;
                }
                else
                {
                    i_trun_data_offset += p_trundata->i_data_offset;
                }
            }
            else
            {
                i_trun_data_offset += i_trun_size;
            }

            i_trun_size = 0;
#ifndef NDEBUG
            msg_Dbg( p_demux,
                     "tk %u run %" PRIu32 " dflt dur %"PRIu32" size %"PRIu32" firstdts %"PRId64" offset %"PRIu64,
                     p_track->i_track_ID,
                     p_track->context.runs.i_count,
                     i_track_defaultsampleduration,
                     i_track_defaultsamplesize,
                     MP4_rescale_mtime( i_trun_dts, p_track->i_timescale ), i_trun_data_offset );
#endif
            //************
            mp4_run_t *p_run = &p_track->context.runs.p_array[p_track->context.runs.i_count++];
            p_run->i_first_dts = i_trun_dts;
            p_run->i_offset = i_trun_data_offset;
            p_run->p_trun = p_trun;

            //************
            /* Sum total time */
            if ( p_trundata->i_flags & MP4_TRUN_SAMPLE_DURATION )
            {
                for( uint32_t i=0; i< p_trundata->i_sample_count; i++ )
                    i_trun_dts += p_trundata->p_samples[i].i_duration;
            }
            else
            {
                i_trun_dts += p_trundata->i_sample_count *
                        i_track_defaultsampleduration;
            }

            /* Get total traf size */
            if ( p_trundata->i_flags & MP4_TRUN_SAMPLE_SIZE )
            {
                for( uint32_t i=0; i< p_trundata->i_sample_count; i++ )
                    i_trun_size += p_trundata->p_samples[i].i_size;
            }
            else
            {
                i_trun_size += p_trundata->i_sample_count *
                        i_track_defaultsamplesize;
            }

            i_prev_traf_end = i_trun_data_offset + i_trun_size;
        }

        i_traf++;
    }

    return VLC_SUCCESS;
}

static int FragGetMoofBySidxIndex( demux_t *p_demux, vlc_tick_t target_time,
                                   uint64_t *pi_moof_pos, vlc_tick_t *pi_sampletime )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    const MP4_Box_t *p_sidx = MP4_BoxGet( p_sys->p_root, "sidx" );
    const MP4_Box_data_sidx_t *p_data;
    if( !p_sidx || !((p_data = BOXDATA(p_sidx))) || !p_data->i_timescale )
        return VLC_EGENERIC;

    stime_t i_target_time = MP4_rescale_qtime( target_time, p_data->i_timescale );

    /* sidx refers to offsets from end of sidx pos in the file + first offset */
    uint64_t i_pos = p_data->i_first_offset + p_sidx->i_pos + p_sidx->i_size;
    stime_t i_time = 0;
    for( uint16_t i=0; i<p_data->i_reference_count; i++ )
    {
        if( i_time + p_data->p_items[i].i_subsegment_duration > i_target_time )
        {
            *pi_sampletime = MP4_rescale_mtime( i_time, p_data->i_timescale );
            *pi_moof_pos = i_pos;
            return VLC_SUCCESS;
        }
        i_pos += p_data->p_items[i].i_referenced_size;
        i_time += p_data->p_items[i].i_subsegment_duration;
    }

    return VLC_EGENERIC;
}

static int FragGetMoofByTfraIndex( demux_t *p_demux, const vlc_tick_t i_target_time, unsigned i_track_ID,
                                   uint64_t *pi_moof_pos, vlc_tick_t *pi_sampletime )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    MP4_Box_t *p_tfra = MP4_BoxGet( p_sys->p_root, "mfra/tfra" );
    for( ; p_tfra; p_tfra = p_tfra->p_next )
    {
        if ( p_tfra->i_type == ATOM_tfra )
        {
            const MP4_Box_data_tfra_t *p_data = BOXDATA(p_tfra);
            if( !p_data || p_data->i_track_ID != i_track_ID )
                continue;

            uint64_t i_pos = 0;
            mp4_track_t *p_track = MP4_GetTrackByTrackID( p_demux, p_data->i_track_ID );
            if ( p_track )
            {
                stime_t i_track_target_time = MP4_rescale_qtime( i_target_time, p_track->i_timescale );
                for ( uint32_t i = 0; i<p_data->i_number_of_entries; i += ( p_data->i_version == 1 ) ? 2 : 1 )
                {
                    stime_t i_time;
                    uint64_t i_offset;
                    if ( p_data->i_version == 1 )
                    {
                        i_time = *((uint64_t *)(p_data->p_time + i));
                        i_offset = *((uint64_t *)(p_data->p_moof_offset + i));
                    }
                    else
                    {
                        i_time = p_data->p_time[i];
                        i_offset = p_data->p_moof_offset[i];
                    }

                    if ( i_time >= i_track_target_time )
                    {
                        if ( i_pos == 0 ) /* Not in this traf */
                            break;

                        *pi_moof_pos = i_pos;
                        *pi_sampletime = MP4_rescale_mtime( i_time, p_track->i_timescale );
                        return VLC_SUCCESS;
                    }
                    else
                        i_pos = i_offset;
                }
            }
        }
    }
    return VLC_EGENERIC;
}

static void MP4_GetDefaultSizeAndDuration( MP4_Box_t *p_moov,
                                           const MP4_Box_data_tfhd_t *p_tfhd_data,
                                           uint32_t *pi_default_size,
                                           uint32_t *pi_default_duration )
{
    if( p_tfhd_data->i_flags & MP4_TFHD_DFLT_SAMPLE_DURATION )
        *pi_default_duration = p_tfhd_data->i_default_sample_duration;

    if( p_tfhd_data->i_flags & MP4_TFHD_DFLT_SAMPLE_SIZE )
        *pi_default_size = p_tfhd_data->i_default_sample_size;

    if( !*pi_default_duration || !*pi_default_size )
    {
        const MP4_Box_t *p_trex = MP4_GetTrexByTrackID( p_moov, p_tfhd_data->i_track_ID );
        if ( p_trex )
        {
            if ( !*pi_default_duration )
                *pi_default_duration = BOXDATA(p_trex)->i_default_sample_duration;
            if ( !*pi_default_size )
                *pi_default_size = BOXDATA(p_trex)->i_default_sample_size;
        }
    }
}

static int DemuxFrag( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    //if we haven't processed the inital ftyp/moov then return until we have completed this predicate
    if(!p_sys->has_processed_ftype_moov) {
    	return VLC_DEMUXER_SUCCESS;
    }

    unsigned i_track_selected = 0;
    int i_status = VLC_DEMUXER_SUCCESS;

    if( unlikely(p_sys->b_error) )
    {
        msg_Warn( p_demux, "unrecoverable error" );
        i_status = VLC_DEMUXER_EOF;
        goto end;
    }

    msg_Info(p_demux, "%d:DemuxFrag, track count is: %d", __LINE__, p_sys->i_tracks);

    /* check for newly selected/unselected track */
    for( unsigned i_track = 0; i_track < p_sys->i_tracks; i_track++ )
    {
        mp4_track_t *tk = &p_sys->track[i_track];
        bool b = true;

        if( !tk->b_ok || tk->b_chapters_source )
            continue;

        if( p_sys->b_seekable )
            es_out_Control( p_demux->out, ES_OUT_GET_ES_STATE, tk->p_es, &b );

        if(tk->b_selected != b)
        {
            msg_Dbg( p_demux, "track %u %s!", tk->i_track_ID, b ? "enabled" : "disabled" );
            MP4_TrackSelect( p_demux, tk, b );
        }

        if( tk->b_selected )
            i_track_selected++;
    }

    if( i_track_selected <= 0 )
    {
        msg_Warn( p_demux, "no track selected, exiting..." );
        i_status = VLC_DEMUXER_EOF;
        goto end;
    }

    msg_Info(p_demux, "%d:DemuxFrag", __LINE__);

    if ( p_sys->context.i_current_box_type != ATOM_mdat )
    {
        /* Othewise mdat is skipped. FIXME: mdat reading ! */
        const uint8_t *p_peek;
        if( vlc_stream_Peek( p_sys->s_frag, &p_peek, 8 ) != 8 )
        {
            msg_Info(p_demux, "%d:DemuxFrag, EOF??", __LINE__);

            i_status = VLC_DEMUXER_EOF;
            goto end;
        }

        p_sys->context.i_current_box_type = VLC_FOURCC( p_peek[4], p_peek[5], p_peek[6], p_peek[7] );
        if( p_sys->context.i_current_box_type != ATOM_moof &&
            p_sys->context.i_current_box_type != ATOM_moov )
        {
            uint64_t i_pos = vlc_stream_Tell( p_sys->s_frag );
            uint64_t i_size = GetDWBE( p_peek );
            if ( i_size == 1 )
            {
                if( vlc_stream_Peek( p_sys->s_frag, &p_peek, 16 ) != 16 )
                {
                    msg_Info(p_demux, "%d:DemuxFrag, EOF??", __LINE__);

                    i_status = VLC_DEMUXER_EOF;
                    goto end;
                }
                i_size = GetQWBE( p_peek + 8 );
            }

            if( UINT64_MAX - i_pos < i_size )
            {
                i_status = VLC_DEMUXER_EOF;
                msg_Info(p_demux, "%d:DemuxFrag, EOF??", __LINE__);

                goto end;
            }

            if( p_sys->context.i_current_box_type == ATOM_mdat )
            {
                /* We'll now read mdat using context atom,
                 * but we'll need post mdat offset, as we'll never seek backward */
                p_sys->context.i_post_mdat_offset = i_pos + i_size;
            }
            else if( MP4_Seek( p_sys->s_frag, i_pos + i_size ) != VLC_SUCCESS ) /* skip other atoms */
            {
                i_status = VLC_DEMUXER_EOF;
                msg_Info(p_demux, "%d:DemuxFrag, EOF??", __LINE__);

                goto end;
            }
        }
        else
        {
            MP4_Box_t *p_vroot = MP4_BoxGetNextChunk( p_sys->s_frag );
            if(!p_vroot)
            {
                i_status = VLC_DEMUXER_EOF;
                goto end;
            }

            MP4_Box_t *p_box = NULL;
            for( p_box = p_vroot->p_first; p_box; p_box = p_box->p_next )
            {
                if( p_box->i_type == ATOM_moof ||
                    p_box->i_type == ATOM_moov )
                    break;
            }

            if( p_box )
            {
                FragResetContext( p_sys );

                if( p_box->i_type == ATOM_moov )
                {
                    p_sys->context.p_fragment_atom = p_sys->p_moov;
                }
                else
                {
                    p_sys->context.p_fragment_atom = MP4_BoxExtract( &p_vroot->p_first, p_box->i_type );

                    /* Detect and Handle Passive Seek */
                    const uint32_t i_sequence_number = FragGetMoofSequenceNumber( p_sys->context.p_fragment_atom );
                    const bool b_discontinuity = ( i_sequence_number != p_sys->context.i_lastseqnumber + 1 );
                    if( b_discontinuity )
                        msg_Info( p_demux, "Fragment sequence discontinuity detected %"PRIu32" != %"PRIu32,
                                            i_sequence_number, p_sys->context.i_lastseqnumber + 1 );
                    p_sys->context.i_lastseqnumber = i_sequence_number;

                    /* Prepare chunk */
                    if( FragPrepareChunk( p_demux, p_sys->context.p_fragment_atom,
                                          MP4_BoxGet( p_vroot, "sidx"), INVALID_SEGMENT_TIME,
                                          b_discontinuity ) != VLC_SUCCESS )
                    {
                        msg_Info(p_demux, "%d:DemuxFrag, FragPrepareChunk failed", __LINE__);

                        MP4_BoxFree( p_vroot );
                        i_status = VLC_DEMUXER_EOF;
                        goto end;
                    }

                    if( b_discontinuity )
                    {
                        p_sys->i_nztime = FragGetDemuxTimeFromTracksTime( p_sys );
                        p_sys->i_pcr = VLC_TICK_INVALID;
                    }
                    /* !Prepare chunk */
                }

                p_sys->context.i_current_box_type = p_box->i_type;
            }

            MP4_BoxFree( p_vroot );

            if( p_sys->context.p_fragment_atom == NULL )
            {
                msg_Info(p_demux, "no moof or moov in current chunk");
                return VLC_DEMUXER_SUCCESS;
            }
        }
    }

    msg_Info(p_demux, "%d:DemuxFrag", __LINE__);

    if ( p_sys->context.i_current_box_type == ATOM_mdat )
    {
        assert(p_sys->context.p_fragment_atom);

        if ( p_sys->context.p_fragment_atom )
        switch( p_sys->context.p_fragment_atom->i_type )
        {
            case ATOM_moov://[ftyp/moov, mdat]+ -> [moof, mdat]+
                msg_Info(p_demux, "%d:DemuxFrag", __LINE__);

                i_status = DemuxMoov( p_demux );
                msg_Info(p_demux, "%d:DemuxFrag", __LINE__);

            break;
            case ATOM_moof:
                msg_Info(p_demux, "%d:DemuxFrag", __LINE__);

                i_status = DemuxMoof( p_demux );
                msg_Info(p_demux, "%d:DemuxFrag", __LINE__);

              break;
        default:
             msg_Err( p_demux, "fragment type %4.4s", (char*) &p_sys->context.p_fragment_atom->i_type );
             break;
        }

        if( i_status == VLC_DEMUXER_EOS )
        {
            i_status = VLC_DEMUXER_SUCCESS;
            /* Skip if we didn't reach the end of mdat box */
            msg_Info(p_demux, "%d:DemuxFrag", __LINE__);

            uint64_t i_pos = vlc_stream_Tell( p_sys->s_frag );
            msg_Info(p_demux, "%d:DemuxFrag", __LINE__);

            if( i_pos != p_sys->context.i_post_mdat_offset && i_status != VLC_DEMUXER_EOF )
            {
                msg_Info(p_demux, "%d:DemuxFrag", __LINE__);

                if( i_pos > p_sys->context.i_post_mdat_offset )
                    msg_Err( p_demux, " Overread mdat by %" PRIu64, i_pos - p_sys->context.i_post_mdat_offset );
                else
                    msg_Warn( p_demux, "mdat had still %"PRIu64" bytes unparsed as samples",
                                        p_sys->context.i_post_mdat_offset - i_pos );
                msg_Info(p_demux, "%d:DemuxFrag", __LINE__);

                if( MP4_Seek( p_sys->s_frag, p_sys->context.i_post_mdat_offset ) != VLC_SUCCESS )
                    i_status = VLC_DEMUXER_EGENERIC;
            }
            p_sys->context.i_current_box_type = 0;

        }
    }
    msg_Info(p_demux, "%d:DemuxFrag", __LINE__);

end:
    if( i_status == VLC_DEMUXER_EOF )
    {
        msg_Info(p_demux, "%d:DemuxFrag, status: %d", __LINE__, i_status);

        vlc_tick_t i_demux_end = INT64_MIN;
        for( unsigned i = 0; i < p_sys->i_tracks; i++ )
        {
            const mp4_track_t *tk = &p_sys->track[i];
            vlc_tick_t i_track_end = MP4_rescale_mtime( tk->i_time, tk->i_timescale );
            if( i_track_end > i_demux_end  )
                i_demux_end = i_track_end;
        }
        if( i_demux_end != INT64_MIN )
            es_out_SetPCR( p_demux->out, VLC_TICK_0 + i_demux_end );
    }

    msg_Info(p_demux, "%d:DemuxFrag, status: %d", __LINE__, i_status);

    return i_status;
}

/* ASF Handlers */
inline static mp4_track_t *MP4ASF_GetTrack( asf_packet_sys_t *p_packetsys,
                                            uint8_t i_stream_number )
{
    demux_sys_t *p_sys = p_packetsys->p_demux->p_sys;
    for ( unsigned int i=0; i<p_sys->i_tracks; i++ )
    {
        if ( p_sys->track[i].p_asf &&
             i_stream_number == p_sys->track[i].BOXDATA(p_asf)->i_stream_number )
        {
            return &p_sys->track[i];
        }
    }
    return NULL;
}

static asf_track_info_t * MP4ASF_GetTrackInfo( asf_packet_sys_t *p_packetsys,
                                               uint8_t i_stream_number )
{
    mp4_track_t *p_track = MP4ASF_GetTrack( p_packetsys, i_stream_number );
    if ( p_track )
        return &p_track->asfinfo;
    else
        return NULL;
}

static void MP4ASF_Send( asf_packet_sys_t *p_packetsys, uint8_t i_stream_number,
                         block_t **pp_frame )
{
    mp4_track_t *p_track = MP4ASF_GetTrack( p_packetsys, i_stream_number );
    if ( !p_track )
    {
        block_Release( *pp_frame );
    }
    else
    {
        block_t *p_gather = block_ChainGather( *pp_frame );
        p_gather->i_dts = p_track->i_dts_backup;
        p_gather->i_pts = p_track->i_pts_backup;
        es_out_Send( p_packetsys->p_demux->out, p_track->p_es, p_gather );
    }

    *pp_frame = NULL;
}

static void MP4ASF_ResetFrames( demux_sys_t *p_sys )
{
    for ( unsigned int i=0; i<p_sys->i_tracks; i++ )
    {
        mp4_track_t *p_track = &p_sys->track[i];
        if( p_track->asfinfo.p_frame )
        {
            block_ChainRelease( p_track->asfinfo.p_frame );
            p_track->asfinfo.p_frame = NULL;
        }
    }
}
