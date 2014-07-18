/*****************************************************************************
 * hds.c: Http Dynamic Streaming (HDS) stream filter
 *****************************************************************************
 *
 * Author: Jonathan Thambidurai <jonathan _AT_ fastly _DOT_ com>
 * Heavily inspired by SMooth module of Frédéric Yhuel <fyhuel _AT_ viotech _DOT_ net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_stream.h>
#include <vlc_strings.h>            /* b64_decode */
#include <vlc_xml.h>
#include <vlc_charset.h>            /* FromCharset */
#include <vlc_es.h>                 /* UNKNOWN_ES */

typedef struct chunk_s
{
    int64_t     duration;   /* chunk duration in afrt timescale units */
    uint64_t    timestamp;
    uint32_t    frag_num;
    uint32_t    seg_num;
    uint32_t    frun_entry; /* Used to speed things up in vod situations */

    uint32_t    data_len;

    uint32_t    mdat_pos;   /* position in the mdat */
    uint32_t    mdat_len;

    void        *next;

    uint8_t     *mdat_data;
    uint8_t     *data;
    bool        failed;
    bool        eof;
} chunk_t;

typedef struct segment_run_s
{
    uint32_t first_segment;
    uint32_t fragments_per_segment;
} segment_run_t;

typedef struct fragment_run_s
{
    uint32_t fragment_number_start;
    uint32_t fragment_duration;
    uint64_t fragment_timestamp;
    uint8_t  discont;
} fragment_run_t;

typedef struct hds_stream_s
{
    /* linked-list of chunks */
    chunk_t        *chunks_head;
    chunk_t        *chunks_livereadpos;
    chunk_t        *chunks_downloadpos;

    char*          quality_segment_modifier;

    /* we can make this configurable */
    uint64_t       download_leadtime;

    /* in timescale units */
    uint32_t       total_duration;

    uint32_t       afrt_timescale;

    /* these two values come from the abst */
    uint32_t       timescale;
    uint64_t       live_current_time;

    vlc_mutex_t    abst_lock;

    vlc_mutex_t    dl_lock;
    vlc_cond_t     dl_cond;

    /* can be left as null */
    char*          abst_url;

    /* this comes from the manifest media section  */
    char*          url;

    /* this comes from the bootstrap info */
    char*          movie_id;

#define MAX_HDS_SERVERS 10
    char*          server_entries[MAX_HDS_SERVERS];
    uint8_t        server_entry_count;

#define MAX_HDS_SEGMENT_RUNS 256
    segment_run_t  segment_runs[MAX_HDS_SEGMENT_RUNS];
    uint8_t        segment_run_count;

#define MAX_HDS_FRAGMENT_RUNS 10000
    fragment_run_t fragment_runs[MAX_HDS_FRAGMENT_RUNS];
    uint32_t       fragment_run_count;
} hds_stream_t;

/* this is effectively just a sanity check  mechanism */
#define MAX_REQUEST_SIZE (50*1024*1024)

struct stream_sys_t
{
    char         *base_url;    /* URL common part for chunks */
    vlc_thread_t live_thread;
    vlc_thread_t dl_thread;

    /* we pend on peek until some number of segments arrives; otherwise
     * the downstream system dies in case of playback */
    uint64_t     chunk_count;

    vlc_array_t  *hds_streams; /* available streams */

    uint32_t     flv_header_bytes_sent;
    uint64_t     duration_seconds;

    bool         live;
    bool         closed;
};

typedef struct _bootstrap_info {
    uint8_t* data;
    char*    id;
    char*    url;
    char*    profile;
    int      data_len;
} bootstrap_info;

typedef struct _media_info {
    char* stream_id;
    char* media_url;
    char* bootstrap_id;
} media_info;

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open( vlc_object_t * );
static void Close( vlc_object_t * );

vlc_module_begin()
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_STREAM_FILTER )
    set_description( N_("HTTP Dynamic Streaming") )
    set_shortname( "Dynamic Streaming")
    add_shortcut( "hds" )
    set_capability( "stream_filter", 30 )
    set_callbacks( Open, Close )
vlc_module_end()

static int   Read( stream_t *, void *, unsigned );
static int   Peek( stream_t *, const uint8_t **, unsigned );
static int   Control( stream_t *, int , va_list );

static inline bool isFQUrl( char* url )
{
    return ( NULL != vlc_strcasestr( url, "https://") ||
             NULL != vlc_strcasestr( url, "http://" ) );
}

static bool isHDS( stream_t *s )
{
    const char *peek;
    int i_size = stream_Peek( s->p_source, (const uint8_t**) &peek, 200 );
    if( i_size < 200 )
        return false;

    const char *str;

    if( !memcmp( peek, "\xFF\xFE", 2 ) )
    {
        str = FromCharset( "UTF-16LE", peek, 512 );
    }
    else if( !memcmp( peek, "\xFE\xFF", 2 ) )
    {
        str = FromCharset( "UTF-16BE", peek, 512 );
    }
    else
        str = peek;

    if( str == NULL )
        return false;

    bool ret = strstr( str, "<manifest" ) != NULL;
    return ret;
}

static uint8_t* parse_asrt( vlc_object_t* p_this,
                        hds_stream_t* s,
                        uint8_t* data,
                        uint8_t* data_end )
{
    uint8_t* data_p = data;

    uint32_t asrt_len = 0;
    asrt_len = U32_AT( data_p );
    if( asrt_len > data_end - data ||
        data_end - data <  14 )
    {
        msg_Err( p_this, "Not enough asrt data (%"PRIu32", %lu)", asrt_len, data_end - data );
        return NULL;
    }

    data_p += sizeof(asrt_len);

    if( 0 != memcmp( "asrt", data_p, 4 ) )
    {
        msg_Err( p_this, "Cant find asrt in bootstrap" );
        return NULL;
    }
    data_p += 4;

    /* ignore flags and versions (we don't handle multiple updates) */
    data_p += 4;

    uint8_t quality_entry_count = *data_p;
    bool quality_found = false;
    data_p++;

    if( ! s->quality_segment_modifier )
    {
        quality_found = true;
    }

    while( quality_entry_count-- > 0 )
    {
        char* str_start = (char*) data_p;
        data_p = memchr( data_p, '\0', data_end - data_p );
        if( ! data_p )
        {
            msg_Err( p_this, "Couldn't find quality entry string in asrt" );
            return NULL;
        }
        data_p++;

        if( ! quality_found )
        {
            if( ! strncmp( str_start, s->quality_segment_modifier,
                           strlen(s->quality_segment_modifier) ) )
            {
                quality_found = true;
            }
        }

        if( data_p >= data_end )
        {
            msg_Err( p_this, "Premature end of asrt in quality entries" );
            return NULL;
        }
    }

    if( data_end - data_p < 4 )
    {
        msg_Err( p_this, "Premature end of asrt after quality entries" );
        return NULL;
    }

    uint32_t segment_run_entry_count = U32_AT( data_p );
    data_p += sizeof(segment_run_entry_count);

    if( data_end - data_p < 8 * segment_run_entry_count )
    {
        msg_Err( p_this, "Not enough data in asrt for segment run entries" );
        return NULL;
    }

    if( segment_run_entry_count >= MAX_HDS_SEGMENT_RUNS )
    {
        msg_Err( p_this, "Too many segment runs" );
        return NULL;
    }

    while( segment_run_entry_count-- > 0 )
    {
        if( quality_found )
        {
            s->segment_runs[s->segment_run_count].first_segment = U32_AT(data_p);
        }
        data_p+=4;

        if( quality_found )
        {
            s->segment_runs[s->segment_run_count].fragments_per_segment = U32_AT(data_p);
        }
        data_p+=4;

        s->segment_run_count++;
    }

    return data_p;
}

static uint8_t* parse_afrt( vlc_object_t* p_this,
                        hds_stream_t* s,
                        uint8_t* data,
                        uint8_t* data_end )
{
    uint8_t* data_p = data;

    uint32_t afrt_len = U32_AT( data_p );
    if( afrt_len > data_end - data ||
        data_end - data <  9 )
    {
        msg_Err( p_this, "Not enough afrt data %u, %ld", afrt_len, data_end - data );
        return NULL;
    }
    data_p += sizeof(afrt_len);

    if( 0 != memcmp( data_p, "afrt", 4 ) )
    {
        msg_Err( p_this, "Cant find afrt in bootstrap" );
        return NULL;
    }
    data_p += 4;

    /* ignore flags and versions (we don't handle multiple updates) */
    data_p += 4;

    if( data_end - data_p < 9 )
    {
        msg_Err( p_this, "afrt is too short" );
        return NULL;
    }

    s->afrt_timescale = U32_AT( data_p );
    data_p += 4;

    bool quality_found = false;
    if( ! s->quality_segment_modifier )
    {
        quality_found = true;
    }

    uint32_t quality_entry_count = *data_p;
    data_p++;
    while( quality_entry_count-- > 0 )
    {
        char* str_start = (char*)data_p;
        data_p = memchr( data_p, '\0', data_end - data_p );
        if( ! data_p )
        {
            msg_Err( p_this, "Couldn't find quality entry string in afrt" );
            return NULL;
        }
        data_p++;

        if( ! quality_found )
        {
            if( ! strncmp( str_start, s->quality_segment_modifier,
                           strlen(s->quality_segment_modifier) ) )
            {
                quality_found = true;
            }
        }
    }

    if( data_end - data_p < 5 )
    {
        msg_Err( p_this, "No more space in afrt after quality entries" );
        return NULL;
    }

    uint32_t fragment_run_entry_count = U32_AT( data_p );
    data_p += sizeof(uint32_t);

    while(fragment_run_entry_count-- > 0)
    {
        if( data_end - data_p < 16 )
        {
            msg_Err( p_this, "Not enough data in afrt" );
            return NULL;
        }

        if( s->fragment_run_count >= MAX_HDS_FRAGMENT_RUNS )
        {
            msg_Err( p_this, "Too many fragment runs, exiting" );
            return NULL;
        }

        s->fragment_runs[s->fragment_run_count].fragment_number_start = U32_AT(data_p);
        data_p += 4;

        s->fragment_runs[s->fragment_run_count].fragment_timestamp = U64_AT( data_p );
        data_p += 8;

        s->fragment_runs[s->fragment_run_count].fragment_duration = U32_AT( data_p );
        data_p += 4;

        s->fragment_runs[s->fragment_run_count].discont = 0;
        if( s->fragment_runs[s->fragment_run_count].fragment_duration == 0 )
        {
            /* discontinuity flag */
            s->fragment_runs[s->fragment_run_count].discont = *(data_p++);
        }

        s->fragment_run_count++;
    }

    return data_p;
}

static inline chunk_t* chunk_new()
{
    chunk_t* chunk = calloc(1, sizeof(chunk_t));
    return chunk;
}

static void chunk_free( chunk_t * chunk )
{
    FREENULL( chunk->data );
    free( chunk );
}

static void parse_BootstrapData( vlc_object_t* p_this,
                                 hds_stream_t * s,
                                 uint8_t* data,
                                 uint8_t* data_end )
{
    uint8_t* data_p = data;

    uint32_t abst_len = U32_AT( data_p );
    if( abst_len > data_end - data
        || data_end - data < 29 /* min size of data */ )
    {
        msg_Warn( p_this, "Not enough bootstrap data" );
        return;
    }
    data_p += sizeof(abst_len);

    if( 0 != memcmp( data_p, "abst", 4 ) )
    {
        msg_Warn( p_this, "Cant find abst in bootstrap" );
        return;
    }
    data_p += 4;

    /* version, flags*/
    data_p += 4;

    /* we ignore the version */
    data_p += 4;

    /* some flags we don't care about here because they are
     * in the manifest
     */
    data_p += 1;

    /* timescale */
    s->timescale = U32_AT( data_p );
    data_p += sizeof(s->timescale);

    s->live_current_time = U64_AT( data_p );
    data_p += sizeof(s->live_current_time);

    /* smtpe time code offset */
    data_p += 8;

    s->movie_id = strndup( (char*)data_p, data_end - data_p );
    data_p += ( strlen( s->movie_id ) + 1 );

    if( data_end - data_p < 4 ) {
        msg_Warn( p_this, "Not enough bootstrap after Movie Identifier" );
        return;
    }

    uint8_t server_entry_count = 0;
    server_entry_count = (uint8_t) *data_p;
    data_p++;

    s->server_entry_count = 0;
    while( server_entry_count-- > 0 )
    {
        if( s->server_entry_count < MAX_HDS_SERVERS )
        {
            s->server_entries[s->server_entry_count++] = strndup( (char*)data_p,
                                                                  data_end - data_p );
            data_p += strlen( s->server_entries[s->server_entry_count-1] ) + 1;
        }
        else
        {
            msg_Warn( p_this, "Too many servers" );
            data_p = memchr( data_p, '\0', data_end - data_p );
            if( ! data_p )
            {
                msg_Err( p_this, "Couldn't find server entry" );
                return;
            }
            data_p++;
        }

        if( data_p >= data_end )
        {
            msg_Warn( p_this, "Premature end of bootstrap info while reading servers" );
            return;
        }
    }

    if( data_end - data_p < 3 ) {
        msg_Warn( p_this, "Not enough bootstrap after Servers" );
        return;
    }

    s->quality_segment_modifier = 0;

    uint8_t quality_entry_count = *data_p;
    data_p++;

    if( quality_entry_count > 1 )
    {
        msg_Err( p_this, "I don't know what to do with multiple quality levels in the bootstrap - shouldn't this be handled at the manifest level?" );
        return;
    }

    s->quality_segment_modifier = 0;
    while( quality_entry_count-- > 0 )
    {
        if( s->quality_segment_modifier != 0 )
        {
            s->quality_segment_modifier = strndup( (char*)data_p, data_end - data_p );
        }
        data_p += strnlen( (char*)data_p, data_end - data_p ) + 1;
    }

    if( data_end - data_p < 2 ) {
        msg_Warn( p_this, "Not enough bootstrap after quality entries" );
        return;
    }

    /* ignoring "DrmData" */
    data_p = memchr( data_p, '\0', data_end - data_p );
    if( ! data_p )
    {
        msg_Err( p_this, "Couldn't find DRM Data" );
        return;
    }
    data_p++;

    if( data_end - data_p < 2 ) {
        msg_Warn( p_this, "Not enough bootstrap after drm data" );
        return;
    }

    /* ignoring "metadata" */
    data_p = memchr( data_p, '\0', data_end - data_p );
    if( ! data_p )
    {
        msg_Err( p_this, "Couldn't find metadata");
        return;
    }
    data_p++;

    if( data_end - data_p < 2 ) {
        msg_Warn( p_this, "Not enough bootstrap after drm data" );
        return;
    }

    uint8_t asrt_count = *data_p;
    data_p++;

    s->segment_run_count = 0;
    while( asrt_count-- > 0 &&
           data_end > data_p &&
           (data_p = parse_asrt( p_this, s, data_p, data_end )) );

    uint8_t afrt_count = *data_p;
    data_p++;

    s->fragment_run_count = 0;
    while( afrt_count-- > 0 &&
           data_end > data_p &&
           (data_p = parse_afrt( p_this, s, data_p, data_end )) );
}

/* this only works with ANSI characters - this is ok
   for the bootstrapinfo field which this function is
   exclusively used for since it is merely a base64 encoding
*/
static bool is_whitespace( char c )
{
    return ( ' '  == c ||
             '\t' == c ||
             '\n' == c ||
             '\v' == c ||
             '\f' == c ||
             '\r' == c );
}

/* see above note for is_whitespace */
static void whitespace_substr( char** start,
                               char** end )
{
    while( is_whitespace( **start ) && *start != *end ) {
        (*start)++;
    }

    if( *start == *end )
        return;

    while( is_whitespace(*(*end - 1) ) ) {
        end--;
    }
}

/* returns length (could be zero, indicating all of remaining data) */
/* ptr is to start of data, right after 'mdat' string */
static uint32_t find_chunk_mdat( vlc_object_t* p_this,
                                 uint8_t* chunkdata, uint8_t* chunkdata_end,
                                 uint8_t** mdatptr )
{
    uint8_t* boxname = 0;
    uint8_t* boxdata = 0;
    uint64_t boxsize = 0;

    do {
        if( chunkdata_end < chunkdata ||
            chunkdata_end - chunkdata < 8 )
        {
            msg_Err( p_this, "Couldn't find mdat in box 1!" );
            *mdatptr = 0;
            return 0;
        }

        boxsize = (uint64_t)U32_AT( chunkdata );
        chunkdata += 4;

        boxname = chunkdata;
        chunkdata += 4;

        if( boxsize == 1 )
        {
            if( chunkdata_end - chunkdata >= 12 )
            {
                boxsize =  U64_AT(chunkdata);
                chunkdata += 8;
            }
            else
            {
                msg_Err( p_this, "Couldn't find mdat in box 2!");
                *mdatptr = 0;
                return 0;
            }
            boxdata = chunkdata;
            chunkdata += (boxsize - 16);
        }
        else
        {
            boxdata = chunkdata;
            chunkdata += (boxsize - 8);
        }
    } while ( 0 != memcmp( boxname, "mdat", 4 ) );

    *mdatptr = boxdata;

    return chunkdata_end - ((uint8_t*)boxdata);
}

/* returns data ptr if valid (updating the chunk itself
   tells the reader that the chunk is safe to read, which is not yet correct)*/
static uint8_t* download_chunk( stream_t *s,
                                stream_sys_t* sys,
                                hds_stream_t* stream, chunk_t* chunk )
{
    const char* quality = "";
    char* server_base = sys->base_url;
    if( stream->server_entry_count > 0 &&
        strlen(stream->server_entries[0]) > 0 )
    {
        server_base = stream->server_entries[0];
    }

    if( stream->quality_segment_modifier )
    {
        quality = stream->quality_segment_modifier;
    }

    const char* movie_id = "";
    if( stream->url && strlen(stream->url) > 0 )
    {
        if( isFQUrl( stream->url ) )
        {
            server_base = stream->url;
        }
        else
        {
            movie_id = stream->url;
        }
    }

    char* fragment_url;
    if( 0 > asprintf( &fragment_url, "%s/%s%sSeg%u-Frag%u",
              server_base,
              movie_id,
              quality,
              chunk->seg_num,
                      chunk->frag_num ) ) {
        msg_Err(s, "Failed to allocate memory for fragment url" );
        return NULL;
    }

    msg_Info(s, "Downloading fragment %s",  fragment_url );

    stream_t* download_stream = stream_UrlNew( s, fragment_url );
    if( ! download_stream )
    {
        msg_Err(s, "Failed to download fragment %s", fragment_url );
        free( fragment_url );
        chunk->failed = true;
        return NULL;
    }
    free( fragment_url );

    int64_t size = stream_Size( download_stream );
    chunk->data_len = (uint32_t) size;

    if( size > MAX_REQUEST_SIZE )
    {
        msg_Err(s, "Strangely-large chunk of %"PRIi64" Bytes", size );
        return NULL;
    }

    uint8_t* data = malloc( size );
    if( ! data )
    {
        msg_Err(s, "Couldn't allocate chunk" );
        return NULL;
    }

    int read = stream_Read( download_stream, data,
                            size );
    chunk->data_len = read;

    if( read < size )
    {
        msg_Err( s, "Requested %"PRIi64" bytes, "\
                 "but only got %d", size, read );
        data = realloc( chunk->data, read );
        chunk->failed = true;
        return NULL;
    }
    else
    {
        chunk->failed = false;
    }

    stream_Delete( download_stream );
    return data;
}

static void* download_thread( void* p )
{
    vlc_object_t* p_this = (vlc_object_t*)p;
    stream_t* s = (stream_t*) p_this;
    stream_sys_t* sys = s->p_sys;

    if ( vlc_array_count( sys->hds_streams ) == 0 )
        return NULL;

    // TODO: Change here for selectable stream
    hds_stream_t* hds_stream = sys->hds_streams->pp_elems[0];

    int canc = vlc_savecancel();

    vlc_mutex_lock( & hds_stream->dl_lock );

    while( ! sys->closed )
    {
        if( ! hds_stream->chunks_downloadpos )
        {
            chunk_t* chunk = hds_stream->chunks_head;
            while(chunk && chunk->data )
            {
                chunk = chunk->next;
            }

            if( chunk && ! chunk->data )
                hds_stream->chunks_downloadpos = chunk;
        }

        while( hds_stream->chunks_downloadpos )
        {
            chunk_t *chunk = hds_stream->chunks_downloadpos;

            uint8_t *data = download_chunk( (stream_t*)p_this,
                                            sys,
                                            hds_stream,
                                            chunk );

            if( ! chunk->failed )
            {
                chunk->mdat_len =
                    find_chunk_mdat( p_this,
                                     data,
                                     data + chunk->data_len,
                                     & chunk->mdat_data );
                if( chunk->mdat_len == 0 ) {
                    chunk->mdat_len = chunk->data_len - (chunk->mdat_data - data);
                }
                hds_stream->chunks_downloadpos = chunk->next;
                chunk->data = data;

                sys->chunk_count++;
            }
        }

        vlc_cond_wait( & hds_stream->dl_cond,
                       & hds_stream->dl_lock );
    }

    vlc_mutex_unlock( & hds_stream->dl_lock );

    vlc_restorecancel( canc );
    return NULL;
}

static chunk_t* generate_new_chunk(
    vlc_object_t* p_this,
    chunk_t* last_chunk,
    hds_stream_t* hds_stream )
{
    stream_t* s = (stream_t*) p_this;
    stream_sys_t *sys = s->p_sys;
    chunk_t *chunk = chunk_new();
    unsigned int frun_entry = 0;

    if( ! chunk ) {
        msg_Err( p_this, "Couldn't allocate new chunk!" );
        return 0;
    }

    if( last_chunk )
    {
        chunk->timestamp = last_chunk->timestamp + last_chunk->duration;
        chunk->frag_num = last_chunk->frag_num + 1;

        if( ! sys->live )
        {
            frun_entry = last_chunk->frun_entry;
        }
    }
    else
    {
        fragment_run_t* first_frun  = hds_stream->fragment_runs;
        if( sys->live )
        {
            chunk->timestamp = (hds_stream->live_current_time * ((uint64_t)hds_stream->afrt_timescale)) / ((uint64_t)hds_stream->timescale);
        }
        else
        {
            chunk->timestamp = first_frun->fragment_timestamp;
            chunk->frag_num =  first_frun->fragment_number_start;
        }
    }

    for( ; frun_entry < hds_stream->fragment_run_count;
         frun_entry++ )
    {
        /* check for discontinuity first */
        if( hds_stream->fragment_runs[frun_entry].fragment_duration == 0 )
        {
            if( frun_entry == hds_stream->fragment_run_count - 1 )
            {
                msg_Err( p_this, "Discontinuity but can't find next timestamp!");
                return NULL;
            }

            chunk->frag_num = hds_stream->fragment_runs[frun_entry+1].fragment_number_start;
            chunk->duration = hds_stream->fragment_runs[frun_entry+1].fragment_duration;
            chunk->timestamp = hds_stream->fragment_runs[frun_entry+1].fragment_timestamp;

            frun_entry++;
            break;
        }

        if( chunk->frag_num == 0 )
        {
            if( frun_entry == hds_stream->fragment_run_count - 1 ||
                ( chunk->timestamp >= hds_stream->fragment_runs[frun_entry].fragment_timestamp &&
                  chunk->timestamp < hds_stream->fragment_runs[frun_entry+1].fragment_timestamp )
                )
            {
                fragment_run_t* frun = hds_stream->fragment_runs + frun_entry;
                chunk->frag_num = frun->fragment_number_start + ( (chunk->timestamp - frun->fragment_timestamp) /
                                                                  frun->fragment_duration );
                chunk->duration = frun->fragment_duration;
            }

        }

        if( hds_stream->fragment_runs[frun_entry].fragment_number_start <=
            chunk->frag_num &&
            (frun_entry == hds_stream->fragment_run_count - 1 ||
             hds_stream->fragment_runs[frun_entry+1].fragment_number_start > chunk->frag_num ) )
        {
            chunk->duration = hds_stream->fragment_runs[frun_entry].fragment_duration;
            chunk->timestamp = hds_stream->fragment_runs[frun_entry].fragment_timestamp +
                chunk->duration * (chunk->frag_num - hds_stream->fragment_runs[frun_entry].fragment_number_start);
            break;
        }
    }

    if( frun_entry == hds_stream->fragment_run_count )
    {
        msg_Err( p_this, "Couldn'd find the fragment run!" );
        return NULL;
    }

    int srun_entry = 0;
    unsigned int segment = 0;
    uint64_t fragments_accum = chunk->frag_num;
    for( srun_entry = 0; srun_entry < hds_stream->segment_run_count;
         srun_entry++ )
    {
        segment = hds_stream->segment_runs[srun_entry].first_segment +
            (chunk->frag_num - fragments_accum ) / hds_stream->segment_runs[srun_entry].fragments_per_segment;

        if( srun_entry + 1 == hds_stream->segment_run_count ||
            hds_stream->segment_runs[srun_entry+1].first_segment > segment )
        {
            break;
        }

        fragments_accum += (
            (hds_stream->segment_runs[srun_entry+1].first_segment -
             hds_stream->segment_runs[srun_entry].first_segment) *
            hds_stream->segment_runs[srun_entry].fragments_per_segment );
    }

    chunk->seg_num = segment;
    chunk->frun_entry = frun_entry;

    if( ! sys->live )
    {
        if( (chunk->timestamp + chunk->duration) / hds_stream->afrt_timescale  >= sys->duration_seconds )
        {
            chunk->eof = true;
        }
    }

    return chunk;
}

static void maintain_live_chunks(
    vlc_object_t* p_this,
    hds_stream_t* hds_stream
    )
{
    if( ! hds_stream->chunks_head )
    {
        /* just start with the earliest in the abst
         * maybe it would be better to use the currentMediaTime?
         * but then we are right on the edge of buffering, esp for
         * small fragments */
        hds_stream->chunks_head = generate_new_chunk(
            p_this, 0, hds_stream );
        hds_stream->chunks_livereadpos = hds_stream->chunks_head;
    }

    chunk_t* chunk = hds_stream->chunks_head;
    bool dl = false;
    while( chunk && ( chunk->timestamp * ((uint64_t)hds_stream->timescale) )
           / ((uint64_t)hds_stream->afrt_timescale)
           <= hds_stream->live_current_time )
    {
        if( chunk->next )
        {
            chunk = chunk->next;
        }
        else
        {
            chunk->next = generate_new_chunk( p_this, chunk, hds_stream );
            chunk = chunk->next;
            dl = true;
        }
    }

    if( dl )
        vlc_cond_signal( & hds_stream->dl_cond );

    chunk = hds_stream->chunks_head;
    while( chunk && chunk->data && chunk->mdat_pos >= chunk->mdat_len && chunk->next )
    {
        chunk_t* next_chunk = chunk->next;
        chunk_free( chunk );
        chunk = next_chunk;
    }

    if( ! hds_stream->chunks_livereadpos )
        hds_stream->chunks_livereadpos = hds_stream->chunks_head;

    hds_stream->chunks_head = chunk;
}

static void* live_thread( void* p )
{
    vlc_object_t* p_this = (vlc_object_t*)p;
    stream_t* s = (stream_t*) p_this;
    stream_sys_t* sys = s->p_sys;

    if ( vlc_array_count( sys->hds_streams ) == 0 )
        return NULL;

    // TODO: Change here for selectable stream
    hds_stream_t* hds_stream = sys->hds_streams->pp_elems[0];

    int canc = vlc_savecancel();

    char* abst_url;

    if( hds_stream->abst_url &&
        ( isFQUrl( hds_stream->abst_url ) ) )
    {
        abst_url = strdup ( hds_stream->abst_url );
    }
    else
    {
        char* server_base = sys->base_url;


        if( 0 > asprintf( &abst_url, "%s/%s",
                          server_base,
                          hds_stream->abst_url ) )
        {
            return 0;
        }
    }

    mtime_t last_dl_start_time;

    while( ! sys->closed )
    {
        last_dl_start_time = mdate();
        stream_t* download_stream = stream_UrlNew( p_this, abst_url );
        if( ! download_stream )
        {
            msg_Err( p_this, "Failed to download abst %s", abst_url );
        }
        else
        {
            int64_t size = stream_Size( download_stream );
            uint8_t* data = malloc( size );
            int read = stream_Read( download_stream, data,
                                    size );
            if( read < size )
            {
                msg_Err( p_this, "Requested %"PRIi64" bytes, "  \
                         "but only got %d", size, read );

            }
            else
            {
                vlc_mutex_lock( & hds_stream->abst_lock );
                parse_BootstrapData( p_this, hds_stream,
                                     data, data + read );
                vlc_mutex_unlock( & hds_stream->abst_lock );
                maintain_live_chunks( p_this, hds_stream );
            }

            free( data );

            stream_Delete( download_stream );
        }

        mwait( last_dl_start_time + ( ((int64_t)hds_stream->fragment_runs[hds_stream->fragment_run_count-1].fragment_duration) * 1000000LL) / ((int64_t)hds_stream->afrt_timescale) );


    }

    free( abst_url );

    vlc_restorecancel( canc );
    return NULL;
}

static int parse_Manifest( stream_t *s )
{
    xml_t *vlc_xml = NULL;
    xml_reader_t *vlc_reader = NULL;
    int type = UNKNOWN_ES;
    stream_t *st = s->p_source;

    msg_Dbg( s, "Manifest parsing\n" );

    vlc_xml = xml_Create( st );
    if( !vlc_xml )
    {
        msg_Err( s, "Failed to open XML parser" );
        return VLC_EGENERIC;
    }

    vlc_reader = xml_ReaderCreate( vlc_xml, st );
    if( !vlc_reader )
    {
        msg_Err( s, "Failed to open source for parsing" );
        xml_Delete( vlc_xml );
        return VLC_EGENERIC;
    }

    char *node;

    stream_sys_t *sys = s->p_sys;

    sys->duration_seconds = 0;

#define MAX_BOOTSTRAP_INFO 10
    bootstrap_info bootstraps[MAX_BOOTSTRAP_INFO];
    uint8_t bootstrap_idx = 0;
    memset( bootstraps, 0, sizeof(bootstrap_info) * MAX_BOOTSTRAP_INFO );

#define MAX_MEDIA_ELEMENTS 10
    media_info medias[MAX_MEDIA_ELEMENTS];
    uint8_t media_idx = 0;
    memset( medias, 0, sizeof(media_info) * MAX_MEDIA_ELEMENTS );

#define MAX_XML_DEPTH 256
    char* element_stack[256];
    uint8_t current_element_idx = 0;
    char* current_element = 0;
    memset( element_stack, 0, sizeof(char*) * MAX_XML_DEPTH );

    const char* attr_name;
    const char* attr_value;

    char* media_id = 0;

#define TIMESCALE 10000000
    while( (type = xml_ReaderNextNode( vlc_reader, (const char**) &node )) > 0 )
    {
        switch( type )
        {
        case XML_READER_STARTELEM:
            if( current_element_idx == 0 && element_stack[current_element_idx] == 0 ) {
                element_stack[current_element_idx] = strdup( node );
            } else {
                element_stack[++current_element_idx] = strdup( node );
            }
            break;
        case XML_READER_ENDELEM:
            if( ! strcmp( current_element, "bootstrapInfo") ) {
                if( bootstrap_idx + 1 == MAX_BOOTSTRAP_INFO ) {
                    msg_Warn( (vlc_object_t*) s, "Too many bootstraps, ignoring" );
                } else {
                    bootstrap_idx++;
                }
            }

            free( current_element );
            element_stack[current_element_idx--] = 0;
            break;
        }

        if( ! element_stack[current_element_idx] ) {
            continue;
        }

        current_element = element_stack[current_element_idx];

        if( type == XML_READER_STARTELEM && ! strcmp( current_element, "media") )
        {
            if( media_idx == MAX_MEDIA_ELEMENTS )
            {
                msg_Err( (vlc_object_t*) s, "Too many media elements, quitting" );
                return VLC_EGENERIC;
            }

            while( ( attr_name = xml_ReaderNextAttr( vlc_reader, &attr_value )) )
            {
                if( !strcmp(attr_name, "streamId" ) )
                {
                    medias[media_idx].stream_id = strdup( attr_value );
                }
                if( !strcmp(attr_name, "url" ) )
                {
                    medias[media_idx].media_url = strdup( attr_value );
                }
                if( !strcmp(attr_name, "bootstrapInfoId" ) )
                {
                    medias[media_idx].bootstrap_id = strdup( attr_value );
                }
            }

            media_idx++;
        }

        if( type == XML_READER_STARTELEM && ! strcmp( current_element, "bootstrapInfo") )
        {
            while( ( attr_name = xml_ReaderNextAttr( vlc_reader, &attr_value )) )
            {
                if( !strcmp(attr_name, "url" ) )
                {
                    bootstraps[bootstrap_idx].url = strdup( attr_value );
                }
                if( !strcmp(attr_name, "id" ) )
                {
                    bootstraps[bootstrap_idx].id = strdup( attr_value );
                }
                if( !strcmp(attr_name, "profile" ) )
                {
                    bootstraps[bootstrap_idx].profile = strdup( attr_value );
                }
            }
        }

        if( type == XML_READER_TEXT )
        {
            if( ! strcmp( current_element, "bootstrapInfo" ) )
            {
                char* start = node;
                char* end = start + strlen(start);
                whitespace_substr( &start, &end );
                *end = '\0';

                bootstraps[bootstrap_idx].data_len =
                    vlc_b64_decode_binary( (uint8_t**)&bootstraps[bootstrap_idx].data, start );
                if( ! bootstraps[bootstrap_idx].data )
                {
                    msg_Err( (vlc_object_t*) s, "Couldn't decode bootstrap info" );
                }
            }
            if( ! strcmp( current_element, "duration" ) )
            {
                double shutup_gcc = atof( node );
                sys->duration_seconds = (uint64_t) shutup_gcc;
            }
            if( ! strcmp( current_element, "id" ) )
            {
                if( current_element != 0 &&
                    ! strcmp( element_stack[current_element_idx-1], "manifest" ) )
                {
                    media_id = strdup( node );
                }
            }
        }
    }

    xml_ReaderDelete( vlc_reader );
    xml_Delete( vlc_xml );

    for( int i = 0; i <= media_idx; i++ )
    {
        for( int j = 0; j < bootstrap_idx; j++ )
        {
            if( ( ! medias[i].bootstrap_id && ! bootstraps[j].id ) ||
                (medias[i].bootstrap_id && bootstraps[j].id &&
                 ! strcmp( medias[i].bootstrap_id, bootstraps[j].id ) ) )
            {
                hds_stream_t* new_stream = malloc(sizeof(hds_stream_t));
                memset( new_stream, 0, sizeof(hds_stream_t));

                vlc_mutex_init( & new_stream->abst_lock );
                vlc_mutex_init( & new_stream->dl_lock );
                vlc_cond_init( & new_stream->dl_cond );

                if( sys->duration_seconds )
                {
                    sys->live = false;
                }
                else
                {
                    sys->live = true;
                }

                if( medias[i].media_url )
                {
                    new_stream->url = strdup( medias[i].media_url );
                }

                if( ! sys->live )
                {
                    parse_BootstrapData( (vlc_object_t*)s,
                                         new_stream,
                                         bootstraps[j].data,
                                         bootstraps[j].data + bootstraps[j].data_len );

                    new_stream->download_leadtime = 15;

                    new_stream->chunks_head = generate_new_chunk(
                        (vlc_object_t*) s, 0, new_stream );
                    chunk_t* chunk = new_stream->chunks_head;
                    uint64_t total_duration = chunk->duration;
                    while( chunk && total_duration/new_stream->afrt_timescale < new_stream->download_leadtime )
                    {
                        chunk->next = generate_new_chunk(
                            (vlc_object_t*) s, chunk, new_stream );
                        chunk = chunk->next;
                        if( chunk )
                            total_duration += chunk->duration;
                    }
                }
                else
                {
                    new_stream->abst_url = strdup( bootstraps[j].url );
                }

                vlc_array_append( sys->hds_streams, new_stream );

                msg_Info( (vlc_object_t*)s, "New track with quality_segment(%s), timescale(%u), movie_id(%s), segment_run_count(%d), fragment_run_count(%u)",
                          new_stream->quality_segment_modifier?"":new_stream->quality_segment_modifier, new_stream->timescale,
                          new_stream->movie_id, new_stream->segment_run_count, new_stream->fragment_run_count );

            }
        }
    }

    for( int i = 0; i < MAX_MEDIA_ELEMENTS; i++ )
    {
        FREENULL( medias[media_idx].stream_id );
        FREENULL( medias[media_idx].media_url );
        FREENULL( medias[media_idx].bootstrap_id );
    }

    for( int i = 0; i < MAX_BOOTSTRAP_INFO; i++ )
    {
        FREENULL( bootstraps[i].data );
        FREENULL( bootstraps[i].id );
        FREENULL( bootstraps[i].url );
        FREENULL( bootstraps[i].profile );
    }

    FREENULL( media_id );

    return VLC_SUCCESS;
}

static void hds_free( hds_stream_t *p_stream )
{
    FREENULL( p_stream->quality_segment_modifier );

    FREENULL( p_stream->abst_url );

    FREENULL( p_stream->url );
    FREENULL( p_stream->movie_id );
    for( int i = 0; i < p_stream->server_entry_count; i++ )
    {
        FREENULL( p_stream->server_entries[i] );
    }

    chunk_t* chunk = p_stream->chunks_head;
    while( chunk )
    {
        chunk_t* next = chunk->next;
        chunk_free( chunk );
        chunk = next;
    }

    free( p_stream );
}

static void SysCleanup( stream_sys_t *p_sys )
{
    if ( p_sys->hds_streams )
    {
        for ( int i=0; i< p_sys->hds_streams->i_count ; i++ )
            hds_free( p_sys->hds_streams->pp_elems[i] );
        vlc_array_destroy( p_sys->hds_streams );
    }
    free( p_sys->base_url );
}

static int Open( vlc_object_t *p_this )
{
    stream_t *s = (stream_t*)p_this;
    stream_sys_t *p_sys;

    if( !isHDS( s ) )
        return VLC_EGENERIC;

    msg_Info( p_this, "HTTP Dynamic Streaming (%s)", s->psz_path );

    s->p_sys = p_sys = calloc( 1, sizeof(*p_sys ) );
    if( unlikely( p_sys == NULL ) )
        return VLC_ENOMEM;

    char *uri = NULL;
    if( unlikely( asprintf( &uri, "%s://%s", s->psz_access, s->psz_path ) < 0 ) )
    {
        free( p_sys );
        return VLC_ENOMEM;
    }

    /* remove the last part of the url */
    char *pos = strrchr( uri, '/');
    *pos = '\0';
    p_sys->base_url = uri;

    p_sys->flv_header_bytes_sent = 0;

    p_sys->hds_streams = vlc_array_new();

    if( parse_Manifest( s ) != VLC_SUCCESS )
    {
        goto error;
    }

    s->pf_read = Read;
    s->pf_peek = Peek;
    s->pf_control = Control;

    if( vlc_clone( &p_sys->dl_thread, download_thread, s, VLC_THREAD_PRIORITY_INPUT ) )
    {
        goto error;
    }

    if( p_sys->live ) {
        msg_Info( p_this, "Live stream detected" );

        if( vlc_clone( &p_sys->live_thread, live_thread, s, VLC_THREAD_PRIORITY_INPUT ) )
        {
            goto error;
        }
    }

    return VLC_SUCCESS;

error:
    SysCleanup( p_sys );
    free( p_sys );
    return VLC_EGENERIC;
}

static void Close( vlc_object_t *p_this )
{
    stream_t *s = (stream_t*)p_this;
    stream_sys_t *p_sys = s->p_sys;

    // TODO: Change here for selectable stream
    hds_stream_t *stream = vlc_array_count(p_sys->hds_streams) ?
        s->p_sys->hds_streams->pp_elems[0] : NULL;

    p_sys->closed = true;
    if (stream)
        vlc_cond_signal( & stream->dl_cond );

    vlc_join( p_sys->dl_thread, NULL );

    if (stream)
    {
        vlc_mutex_destroy( &stream->dl_lock );
        vlc_cond_destroy( &stream->dl_cond );
        vlc_mutex_destroy( &stream->abst_lock );
    }

    if( p_sys->live )
    {
        vlc_join( p_sys->live_thread, NULL );
    }

    SysCleanup( p_sys );
    free( p_sys );
}

static unsigned char flv_header[] = {
        'F',
        'L',
        'V',
        0x1, //version
        0x5, //indicates audio and video
        0x0, // length
        0x0, // length
        0x0, // length
        0x9, // length of header
        0x0,
        0x0,
        0x0,
        0x0, // initial "trailer"
};

static int send_flv_header( stream_sys_t* p_sys, void* buffer, unsigned i_read,
                            bool peek )
{
    uint32_t to_be_read = i_read;
    if( to_be_read > 13 - p_sys->flv_header_bytes_sent ) {
        to_be_read = 13 - p_sys->flv_header_bytes_sent;
    }

    memcpy( buffer, flv_header + p_sys->flv_header_bytes_sent, to_be_read );

    if( ! peek )
    {
        p_sys->flv_header_bytes_sent += to_be_read;
    }
    return to_be_read;
}

static unsigned read_chunk_data(
    vlc_object_t* p_this,
    uint8_t* buffer, unsigned read_len,
    hds_stream_t* stream,
    bool* eof
    )
{
    stream_t* s = (stream_t*) p_this;
    stream_sys_t* sys = s->p_sys;
    chunk_t* chunk = stream->chunks_head;
    uint8_t* buffer_start = buffer;
    bool dl = false;

    if( chunk && chunk->eof && chunk->mdat_pos >= chunk->mdat_len ) {
        *eof = true;
        return 0;
    }

    while( chunk && chunk->data && read_len > 0 && ! (chunk->eof && chunk->mdat_pos >= chunk->mdat_len ) )
    {
        /* in the live case, it is necessary to store the next
         * pointer here, since as soon as we increment the mdat_pos, that
         * chunk may be deleted */
        chunk_t* next = chunk->next;

        if( chunk->mdat_pos < chunk->mdat_len )
        {
            unsigned cp_len = chunk->mdat_len - chunk->mdat_pos;
            if( cp_len > read_len )
                cp_len = read_len;
            memcpy( buffer, chunk->mdat_data + chunk->mdat_pos,
                    cp_len );

            read_len -= cp_len;
            buffer += cp_len;
            chunk->mdat_pos += cp_len;
        }

        if( ! sys->live && (chunk->mdat_pos >= chunk->mdat_len || chunk->failed) )
        {
            if( chunk->eof )
            {
                *eof = true;
            }

            /* make sure there is at least one chunk in the queue */
            if( ! chunk->next && ! chunk->eof )
            {
                chunk->next = generate_new_chunk( p_this, chunk,  stream );
                dl = true;
            }

            if( ! chunk->eof )
            {
                chunk_free( chunk );
                chunk = next;
                stream->chunks_head = chunk;
            }
        }
        else if( sys->live && (chunk->mdat_pos >= chunk->mdat_len || chunk->failed) )
        {
            chunk = next;
        }
    }

    if( sys->live )
    {
        stream->chunks_livereadpos = chunk;
    }

    /* new chunk generation is handled by a different thread in live case */
    if( ! sys->live )
    {
        chunk = stream->chunks_head;
        if( chunk )
        {
            uint64_t total_duration = chunk->duration;
            while( chunk && total_duration/stream->afrt_timescale < stream->download_leadtime && ! chunk->eof )
            {
                if( ! chunk->next && ! chunk->eof )
                {
                    chunk->next = generate_new_chunk( p_this, chunk, stream );
                    dl = true;
                }

                if( ! chunk->eof )
                {
                    chunk = chunk->next;
                    if( chunk )
                    {
                        total_duration += chunk->duration;
                    }
                }
            }
        }

        if( dl )
            vlc_cond_signal( & stream->dl_cond );
    }

    return ( ((uint8_t*)buffer) - ((uint8_t*)buffer_start));
}

static int Read( stream_t *s, void *buffer, unsigned i_read )
{
    stream_sys_t *p_sys = s->p_sys;

    if ( vlc_array_count( p_sys->hds_streams ) == 0 )
        return 0;

    // TODO: change here for selectable stream
    hds_stream_t *stream = s->p_sys->hds_streams->pp_elems[0];
    int length = 0;

    uint8_t *buffer_uint8 = (uint8_t*) buffer;

    unsigned hdr_bytes = send_flv_header( p_sys, buffer, i_read, false );
    length += hdr_bytes;
    i_read -= hdr_bytes;
    buffer_uint8 += hdr_bytes;

    bool eof = false;
    while( i_read > 0 && ! eof )
    {
        int tmp_length = read_chunk_data( (vlc_object_t*)s, buffer_uint8, i_read, stream, &eof );
        buffer_uint8 += tmp_length;
        i_read -= tmp_length;
        length += tmp_length;
    }

    return length;
}

static int Peek( stream_t *s, const uint8_t **pp_peek, unsigned i_peek )
{
    stream_sys_t *p_sys = s->p_sys;

    if ( vlc_array_count( p_sys->hds_streams ) == 0 )
        return 0;

    // TODO: change here for selectable stream
    hds_stream_t *stream = p_sys->hds_streams->pp_elems[0];

    if( p_sys->flv_header_bytes_sent < 13 )
    {
        *pp_peek = flv_header + p_sys->flv_header_bytes_sent;
        return 13 - p_sys->flv_header_bytes_sent;
    }

    if( stream->chunks_head && ! stream->chunks_head->failed && stream->chunks_head->data )
    {
        // TODO: change here for selectable stream
        chunk_t* chunk = stream->chunks_head;
        *pp_peek = chunk->mdat_data + chunk->mdat_pos;
        if( chunk->mdat_len - chunk->mdat_pos < i_peek )
        {
            return chunk->mdat_len - chunk->mdat_pos;
        }
        else
        {
            return i_peek;
        }
    } else
    {
        return 0;
    }
}

static int Control( stream_t *s, int i_query, va_list args )
{
    switch( i_query )
    {
        case STREAM_CAN_SEEK:
            *(va_arg( args, bool * )) = false;
            break;
        case STREAM_CAN_FASTSEEK:
        case STREAM_CAN_PAUSE: /* TODO */
            *(va_arg( args, bool * )) = false;
            break;
        case STREAM_CAN_CONTROL_PACE:
            *(va_arg( args, bool * )) = true;
            break;
        case STREAM_GET_PTS_DELAY:
            *va_arg (args, int64_t *) = INT64_C(1000) *
                var_InheritInteger(s, "network-caching");
             break;
        default:
            return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}