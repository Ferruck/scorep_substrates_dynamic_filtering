#define UNW_LOCAL_ONLY
#define _GNU_SOURCE /* <- needed for libunwind so stack_t is known */

#include <stddef.h> /* <- needs to come before libunwind for size_t */
#include <errno.h>
#include <fcntl.h>
#include <libunwind.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <SCOREP_SubstratePlugins.h>
#include <SCOREP_SubstrateEvents.h>
#include <SCOREP_PublicHandles.h>
#include <SCOREP_PublicTypes.h>

/**
 * Default to own built-in hash function.
 */
#ifndef HASH_FUNCTION
#define HASH_FUNCTION HASH_OWN
#endif

/**
 * Simple identity hash function.
 *
 * This has proven slightly faster than the internal uthash hash functions.
 */
#define HASH_OWN( key, keylen, hashv )                                                            \
{                                                                                                 \
    hashv = *key;                                                                                 \
}

#include "uthash.h"

/**
 * Stores region info.
 */
typedef struct region_info
{
    /** Handle for uthash usage */
    UT_hash_handle hh;
    /** Global counter for region entries */
    uint64_t call_cnt;
    /** Global calculated region duration */
    uint64_t duration;
    /** Timestamp of last enter into this region (used by main thread) */
    uint64_t last_enter;
    /** Pointer to the callq for the enter instrumentation function */
    char* enter_func;
    /** Pointer to the callq for the exit instrumentation function */
    char* exit_func;
    /** Human readable name of the region */
    char* region_name;
    /** Handle identifying the region */
    uint32_t region_handle;
    /** Recursion depth in this region */
    uint32_t depth;
    /** Mean region duration used for comparison */
    float mean_duration;
    /** Marks whether the region is deletable */
    bool deletable;
    /** Marks whether the region has been deleted */
    bool inactive;
} region_info;

/**
 * Stores calling information per thread.
 *
 * This is necessary as otherwise all on_exit_region and on_enter_region calls need to be
 * serialized.
 */
typedef struct local_region_info
{
    /** Handle for uthash usage */
    UT_hash_handle hh;
    /** Local counter for region entries */
    uint64_t call_cnt;
    /** Local calculated region duration */
    uint64_t duration;
    /** Timestamp of last enter into this region */
    uint64_t last_enter;
    /** Region id this info belongs to */
    uint32_t region_handle;
    /** Pointer to the callq for the enter instrumentation function */
    char* enter_func;
    /** Pointer to the callq for the exit instrumentation function */
    char* exit_func;
} local_region_info;

/** Global list of defined regions */
region_info* regions = NULL;

/** Number of created threads */
uint32_t num_threads = 0;

/** Default to max 512 threads that are observed */
#ifndef MAX_THREAD_CNT
#define MAX_THREAD_CNT 512
#endif

/** Thread local region info */
local_region_info* local_info_array[MAX_THREAD_CNT];

/** Thread-local index for accessing the thread-local part of the array */
__thread uint32_t local_info_array_index;

/** General mutex used as a guard for writing region info */
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

/** Special mutex used for protecting the thread counter */
pthread_mutex_t thread_ctr_mtx = PTHREAD_MUTEX_INITIALIZER;

/** Special mutex used for protecting the number of threads */
pthread_mutex_t num_threads_mtx = PTHREAD_MUTEX_INITIALIZER;

/** Thread counter */
uint64_t thread_ctr = 0;

/** Flag indicating the filtering method to be used (true = absolute, false = relative) */
bool filtering_absolute;

/** Threshold for filtering */
unsigned long long threshold = 0;

/** Mean duration across all regions */
float mean_duration = 0;

/** Flag indicating that this current thread is the main thread */
__thread bool main_thread = false;

/** Internal substrates id */
size_t id;

/** Internal substrates callbacks for information retrieval about handles */
const char* (*get_region_name)( SCOREP_RegionHandle handle );
SCOREP_ParadigmType (*get_paradigm_type)( SCOREP_RegionHandle handle );
uint32_t (*get_location_id)( const struct SCOREP_Location* location );

/** Name of the enter region instrumentation call */
char* enter_func = NULL;

/** Name of the exit region instrumentation call */
char* exit_func = NULL;

/**
 * Update the mean duration of all regions.
 *
 * Only used if the plugin uses the relative filtering method.
 */
static void update_mean_duration( )
{
    region_info *current, *tmp;
    uint64_t ctr = 1;
    float new_duration = 0;

    HASH_ITER( hh, regions, current, tmp )
    {
        // Only use active regions for calculating the mean duration.
#ifdef DYNAMIC_FILTERING_DEBUG
        if( !current->inactive )
#else
        if( !current->deletable && !current->inactive )
#endif
        {
            new_duration += current->mean_duration;
            ctr++;
        }
    }

    mean_duration = new_duration / ctr;
}

/**
 * Overrides a callq at the given position with a five byte NOP.
 *
 * By calling mprotect right before and after writing the NOP this function ensures that the correct
 * part of the TXT segment is writable and it's only writable as long as needed.
 *
 * @param   ptr                             Position of the callq to override.
 */
static void override_callq( char*                                                   ptr )
{
    // Get the page size of the system we're running on.
    int page_size = sysconf( _SC_PAGE_SIZE );
    // The callq may be splitted onto two different pages and mprotect changes access permissions
    // on a per page basis, so we need to change the access permission on the pages where the first
    // and the last byte of the callq reside.
    void* first_ptr = ptr - ( (unsigned long) ptr % page_size );
    void* second_ptr = ( ptr + 4 ) - ( (unsigned long) ( ptr + 4 ) % page_size );

    // Add the write permission.
    if( mprotect( first_ptr, page_size, PROT_READ | PROT_WRITE | PROT_EXEC ) != 0
     || mprotect( second_ptr, page_size, PROT_READ | PROT_WRITE | PROT_EXEC ) != 0 )
    {
        fprintf( stderr,  "Could not add write permission to memory access rights on position "
                          "%p", ptr );
    }

    // Finally write that NOP.
    const char nop[] = { 0x0f, 0x1f, 0x44, 0x00, 0x00 };
    memcpy( ptr, nop, sizeof( char ) * 5 );

    // Remove the write permission.
    if( mprotect( first_ptr, page_size, PROT_READ | PROT_EXEC ) != 0
     || mprotect( second_ptr, page_size, PROT_READ | PROT_EXEC ) != 0 )
    {
        fprintf( stderr,  "Could not remove write permission to memory access rights on position "
                          "%p", ptr );
    }
}

/**
 * Checks which instrumentation call is used in the binary.
 *
 * Walks down the call path and searches for all known instrumentation functions (enter functions,
 * as this one should be called within a enter instrumentation call). The type found is stored for
 * later use in get_function_call_ip.
 */
static void get_instrumentation_call_type( )
{
    unw_cursor_t cursor;
    unw_context_t uc;
    unw_word_t offset;
    char sym[256];

    unw_getcontext( &uc );
    unw_init_local( &cursor, &uc );

    // Step up the call path...
    while( unw_step( &cursor ) > 0 )
    {
        // ... and check the function name against all know instrumentation call names.
        unw_get_proc_name( &cursor, sym, sizeof( sym ), &offset );

        if( strncmp( sym, "__cyg_profile_func_enter", 24 ) == 0 )
        {
            enter_func = "__cyg_profile_func_enter";
            exit_func = "__cyg_profile_func_exit";
            return;
        }
        else if( strncmp( sym, "scorep_plugin_enter_region", 26 ) == 0 )
        {
            enter_func = "scorep_plugin_enter_region";
            exit_func = "scorep_plugin_exit_region";
            return;
        }
        else if( strncmp( sym, "__VT_IntelEntry", 15 ) == 0 )
        {
            enter_func = "__VT_IntelEntry";
            exit_func = "__VT_IntelExit";
            return;
        }
    }
}

/**
 * Returns the instruction pointer for the given function.
 *
 * This method walks down the call path and tries to find a function with the given name and
 * returns a pointer for the first byte of the call to this function in the current call path.
 * Note that only the first (beginning from the innermost function) occurrence of the function call
 * will be handled.
 *
 * @param   function_name                   The function to look up.
 *
 * @return                                  Pointer to the first byte of the call to the given
 *                                          function in the current call path.
 */
static char* get_function_call_ip( const char*                                      function_name )
{
    // If we haven't found the instrumentation type, we can't search for call IPs
    if( !function_name )
    {
        return 0;
    }

    unw_cursor_t cursor;
    unw_context_t uc;
    unw_word_t ip, offset;
    char sym[256];

    unw_getcontext( &uc );
    unw_init_local( &cursor, &uc );

    bool found = false;

    // Step up the call path...
    while( unw_step( &cursor ) > 0 )
    {
        // ...and check if the current entry has the name given in the parameter.
        unw_get_proc_name( &cursor, sym, sizeof( sym ), &offset );

        // We need to get the last (in up direction) call to the function to avoid deleting of
        // intermediate jumps within the function as libunwind does not report them any different.
        if( strcmp( sym, function_name ) == 0 )
        {
            found = true;
        }

        if( found && strcmp( sym, function_name ) != 0 )
        {
            // UNW_REG_IP is the first byte _after_ the callq so we need to step back 5 bytes.
            unw_get_reg( &cursor, UNW_REG_IP, &ip );
            return (char*) ( ip - 5 );
        }
    }

    // This shouldn't happen, if we're on this point, we tried to delete a function not present in
    // the current call path. We return zero in this case because delete_regions wont try to delete
    // this call in this case and we get no undefined behaviour.
    return (char*) 0;
}

/**
 * Remove all unwanted regions.
 *
 * This function iterates over all regions and deletes all of them that are marked as deletable. The
 * iteration over the elements isn't guarded by locks because the program's semantic ensures that
 * this function is only called when there's only one thread present.
 */
static void delete_regions( )
{
    region_info *current, *tmp;

    HASH_ITER( hh, regions, current, tmp )
    {
        // Only delete the function calls if the region is marked as deletable, the address of the
        // entry function call and the address of the exit function call are correctly set and the
        // call stack depth for the function is zero (we're not currently in a recursive call of
        // that function).
        if( !current->inactive
            && current->deletable
            && current->depth == 0
            && !( current->enter_func == 0 || current->exit_func == 0 ) )
        {
            override_callq( current->enter_func );
            override_callq( current->exit_func );
            current->inactive = true;
#ifdef DYNAMIC_FILTERING_DEBUG
            fprintf( stderr, "Deleted instrumentation calls for region %s!\n",
                                                                        current->region_name );
#endif
        }
    }
}

/**
 * Team begin event.
 *
 * As long as there's multithreaded execution, there's no safe way to filter functions. So we've to
 * notice the begin of a multithreaded part of execution to stop filtering.
 *
 * @param   scorep_location                 unused
 * @param   timestamp                       unused
 * @param   scorep_paradigm                 unused
 * @param   scorep_thread_team              unused
 */
static void on_team_begin( __attribute__((unused)) struct SCOREP_Location*          scorep_location,
                           __attribute__((unused)) uint64_t                         timestamp,
                           __attribute__((unused)) SCOREP_ParadigmType              scorep_paradigm,
                           __attribute__((unused)) SCOREP_InterimCommunicatorHandle scorep_thread_team )
{
    pthread_mutex_lock( &thread_ctr_mtx );
    thread_ctr++;
    pthread_mutex_unlock( &thread_ctr_mtx );
}

/**
 * Team end event.
 *
 * See on_team_begin.
 *
 * @param   scorep_location                 unused
 * @param   timestamp                       unused
 * @param   scorep_paradigm                 unused
 * @param   scorep_thread_team              unused
 */
static void on_team_end( __attribute__((unused)) struct SCOREP_Location*            scorep_location,
                         __attribute__((unused)) uint64_t                           timestamp,
                         __attribute__((unused)) SCOREP_ParadigmType                scorep_paradigm,
                         __attribute__((unused)) SCOREP_InterimCommunicatorHandle   scorep_thread_team )
{
    pthread_mutex_lock( &thread_ctr_mtx );
    thread_ctr--;
    pthread_mutex_unlock( &thread_ctr_mtx );
}

/**
 * Thread join event.
 *
 * This is emitted when all threads finished on_team_end. As all information is gathered in the
 * global storage, the main thread can recalculate the metrics.
 *
 * @param scorep_location                   unused
 * @param timestamp                         unused
 * @param scorep_paradigm                   unused
 */
void on_join( __attribute__((unused)) struct SCOREP_Location*                       scorep_location,
              __attribute__((unused)) uint64_t                                      timestamp,
              __attribute__((unused)) SCOREP_ParadigmType                           paradigm_type )
{
    region_info *to_change, *tmp;

    // Recalculate all filter decisions.
    HASH_ITER( hh, regions, to_change, tmp )
    {
        uint32_t border = num_threads > MAX_THREAD_CNT ? MAX_THREAD_CNT : num_threads;

        // Combine the locally gathered information with the global ones.
        for( uint32_t i = 0; i < border; ++i )
        {
            local_region_info* local;
            HASH_FIND( hh, local_info_array[i], &to_change->region_handle,
                                                                    sizeof( uint32_t ), local );

            if( local != NULL )
            {
                to_change->call_cnt += local->call_cnt;
                local->call_cnt = 0;
                to_change->duration += local->duration;
                local->duration = 0;

                if( to_change->enter_func == 0 && local->enter_func != 0 )
                {
                    to_change->enter_func = local->enter_func;
                }

                if( to_change->exit_func == 0 && local->exit_func != 0 )
                {
                    to_change->exit_func = local->exit_func;
                }
            }
        }

        if( filtering_absolute )
        {
            // We're filtering absolute so just compare this region's mean duration with the
            // threshold.
            if( ( (float) to_change->duration / to_change->call_cnt ) < threshold )
            {
                to_change->deletable = true;
            }
        }
        else
        {
            // We're filtering relative so first update all regions' mean durations and then
            // compare the duration of this region with the mean of all regions.
            if( to_change->call_cnt == 0 )
            {
                to_change->mean_duration = 0;
            }
            else
            {
                to_change->mean_duration = (float) to_change->duration / to_change->call_cnt;
            }

            update_mean_duration( );

            if( to_change->mean_duration < mean_duration - threshold )
            {
                to_change->deletable = true;
            }
        }
    }
}

/**
 * Enter region.
 *
 * This updates the region info (call counter) and decides whether or not the region instrumentation
 * should be overwritten. If so, it checks if we're in a multi threaded environment and acts
 * accordingly:
 *
 *  * multithreaded: Skip deletion and wait untill all threads are joined.
 *  * singlethreaded: Mark the state as deletion ready and hold a mutex so that no new spawned
 *    thread can enter a possibly deleted region.
 *
 * @param   scorep_location                 unused
 * @param   timestamp                       The timestamp of the entry event.
 * @param   region_handle                   The region that is entered.
 * @param   metric_values                   unused
 */
static void on_enter_region( __attribute__((unused)) struct SCOREP_Location*        scorep_location,
                             uint64_t                                               timestamp,
                             SCOREP_RegionHandle                                    region_handle,
                             __attribute__((unused)) uint64_t*                      metric_values )
{
    // Skip the undeletable functions!
    if( (*get_paradigm_type)( region_handle ) != SCOREP_PARADIGM_COMPILER )
    {
        return;
    }

    // Once per runtime determine which instrumentation calls are used in this binary.
    if( !enter_func || !exit_func )
    {
        get_instrumentation_call_type( );
    }

    // The function could be overwritten. Process it further.
    if( main_thread )
    {
        region_info* region;
        HASH_FIND( hh, regions, &region_handle, sizeof( uint32_t ), region );

        // Check for missing instruction pointer.
        if( !region->enter_func )
        {
            region->enter_func = get_function_call_ip( enter_func );
        }

        // If the current region is already deleted, skip this whole thing.
        if( !region->inactive )
        {
            pthread_mutex_lock( &mtx );
            region->last_enter = timestamp;
            region->depth++;
            pthread_mutex_unlock( &mtx );
        }
    }
    else if( local_info_array_index < MAX_THREAD_CNT )
    {
        local_region_info* info;
        HASH_FIND( hh, local_info_array[local_info_array_index], &region_handle, sizeof( uint32_t ),
                                                                                            info );

        if( info != NULL )
        {
            // Store the last (this) entry for the current thread.
            info->last_enter = timestamp;

            // Check for missing instruction pointer.
            if( !info->enter_func )
            {
                info->enter_func = get_function_call_ip( enter_func );
            }
        }
    }
}

/**
 * Exit region.
 *
 * This method contains the code for calculating metrics (see on_enter as well) and the actual
 * instrumentation override code.
 *
 * @param   scorep_location                 unused
 * @param   timestamp                       Time of the exit from the region.
 * @param   region_handle                   The region that is exited.
 * @param   metric_values                   unused
 */
static void on_exit_region( __attribute__((unused)) struct SCOREP_Location*         scorep_location,
                            uint64_t                                                timestamp,
                            SCOREP_RegionHandle                                     region_handle,
                            __attribute__((unused)) uint64_t*                       metric_values )
{
    // Skip the undeletable functions!
    if( (*get_paradigm_type)( region_handle ) != SCOREP_PARADIGM_COMPILER )
    {
        return;
    }

    // This function could be overwritten. Process it further.
    if( main_thread )
    {
        region_info* region;
        HASH_FIND( hh, regions, &region_handle, sizeof( uint32_t ), region );

        region->depth--;

        // Check for missing instruction pointer.
        if( !region->exit_func )
        {
            region->exit_func = get_function_call_ip( exit_func );
        }

        // If the region already has been deleted or marked as deletable, skip the next steps.
#ifdef DYNAMIC_FILTERING_DEBUG
        if( !region->inactive )
#else
        if( !region->deletable && !region->inactive )
#endif
        {
            region->call_cnt++;
            region->duration += ( timestamp - region->last_enter );

            if( filtering_absolute )
            {
                // We're filtering absolute so just compare this region's mean duration with the
                // threshold.
                if( ( (float) region->duration / region->call_cnt ) < threshold )
                {
                    region->deletable = true;
                }
            }
            else
            {
                // We're filtering relative so first update all regions' mean durations and then
                // compare the duration of this region with the mean of all regions.
                if( region->call_cnt == 0 )
                {
                    region->mean_duration = 0;
                }
                else
                {
                    region->mean_duration = (float) region->duration / region->call_cnt;
                }

                update_mean_duration( );

                if( region->mean_duration < mean_duration - threshold )
                {
                    region->deletable = true;
                }
            }
        }

        pthread_mutex_lock( &thread_ctr_mtx );
        if( thread_ctr == 0 )
        {
            // Single threaded execution, time for filtering.
            delete_regions( );
        }
        pthread_mutex_unlock( &thread_ctr_mtx );
    }
    else if( local_info_array_index < MAX_THREAD_CNT )
    {
        local_region_info* info;
        HASH_FIND( hh, local_info_array[local_info_array_index], &region_handle, sizeof( uint32_t ),
                                                                                            info );

        if( info != NULL )
        {
            // Region not (yet) ready for deletion so update the metrics.
            info->call_cnt++;
            info->duration += ( timestamp - info->last_enter );

            // Check for missing instruction pointer.
            if( !info->exit_func )
            {
                info->exit_func = get_function_call_ip( exit_func );
            }
        }
    }
}

/**
 * Call on Score-P's region definition event.
 *
 * Creates a new region_info struct in the global regions table for the newly defined region.
 *
 * @param   handle                          Generic handle type identifying the region.
 * @param   type                            Type specifier for the handle.
 */
static void on_define_region( SCOREP_AnyHandle                                      handle,
                              SCOREP_HandleType                                     type )
{
    // This plugin can only handle compiler instrumentation, so we can safely ignore all other
    // regions.
    if( type != SCOREP_HANDLE_TYPE_REGION
        || (*get_paradigm_type)( handle ) != SCOREP_PARADIGM_COMPILER )
    {
        return;
    }

    region_info* new;

    // Check if this region handle is already registered, as this shouldn't happen.
    HASH_FIND( hh, regions, &handle, sizeof( uint32_t ), new );
    if( new == NULL )
    {
        const char* region_name = (*get_region_name)( handle );

        new = calloc( 1, sizeof( region_info ) );
        new->region_handle = handle;
        new->region_name = calloc( 1, strlen( region_name ) * sizeof( char ) );
        memcpy( new->region_name, region_name, strlen( region_name ) * sizeof( char ) );

        HASH_ADD( hh, regions, region_handle, sizeof( uint32_t ), new );
    }
    else
    {
        exit( EXIT_FAILURE );
    }
}

/**
 * Called whenever a location is created.
 *
 * When a location (e.g. an OpenMP thread) is created, we have to copy all region definitions into
 * a location-local storage to gain a lock free data access.
 *
 * @param   location                        The location which is created (unused).
 * @param   parent_location                 The location's parent location (unused).
 */
void on_create_location( struct SCOREP_Location*                                    location,
                         __attribute__((unused)) struct SCOREP_Location*            parent_location )
{
    if( (*get_location_id)( location ) == 0 )
    {
        // Mark the main thread as the main thread.
        main_thread = true;
    }
    else
    {
        // Get array index for thread local storage
        pthread_mutex_lock( &num_threads_mtx );
        local_info_array_index = num_threads;
        num_threads++;
        pthread_mutex_unlock( &num_threads_mtx );

        if( local_info_array_index >= MAX_THREAD_CNT )
        {
            fprintf( stderr, "Maximum thread count reached. No information gathered for this thread."
                             " To increase the maximum number of observable threads you need to "
                             "recompile the plugin.\n" );
            return;
        }

        // All other threads store their info in thread local storage to avoid synchronization.
        region_info *current, *tmp;
        local_region_info* new;

        HASH_ITER( hh, regions, current, tmp )
        {
            new = calloc( 1, sizeof( local_region_info ) );
            new->region_handle = current->region_handle;
            HASH_ADD( hh, local_info_array[local_info_array_index], region_handle,
                                                                        sizeof( uint32_t ), new );
        }
    }
}

/**
 * Called whenever a location is deleted.
 *
 * If a location (e.g. a OpenMP thread) is deleted, its data is not needed any longer. So it can
 * safely be deleted.
 *
 * @param   location                        The location which is deleted (unused).
 */
void on_delete_location( __attribute__((unused)) struct SCOREP_Location*            location )
{
    if( local_info_array_index < MAX_THREAD_CNT )
    {
        local_region_info *current, *tmp;

        HASH_ITER( hh, local_info_array[local_info_array_index], current, tmp )
        {
            HASH_DEL( local_info_array[local_info_array_index], current );
            free( current );
        }

        local_info_array[local_info_array_index] = NULL;
    }

    pthread_mutex_lock( &num_threads_mtx );
    num_threads--;
    pthread_mutex_unlock( &num_threads_mtx );
}

/**
 * The plugin's initialization method.
 *
 * Just sets some default values and reads some environment variables.
 */
static int init( void )
{
    // Get the threshold for filtering.
    char* env_str = getenv( "SCOREP_SUBSTRATES_DYNAMIC_FILTERING_THRESHOLD" );
    if( env_str == NULL )
    {
        fprintf( stderr, "Unable to parse SCOREP_SUBSTRATES_DYNAMIC_FILTERING_THRESHOLD.\n" );
        exit( EXIT_FAILURE );
    }

    threshold = strtoull( env_str, NULL, 10 );
    if( threshold == 0 )
    {
        fprintf( stderr, "Unable to parse SCOREP_SUBSTRATES_DYNAMIC_FILTERING_THRESHOLD or set "
                         "to 0.\n" );
        exit( EXIT_FAILURE );
    }

    // Get the wanted filtering method.
    env_str = getenv( "SCOREP_SUBSTRATES_DYNAMIC_FILTERING_METHOD" );
    if( env_str == NULL )
    {
        fprintf( stderr, "Unable to parse SCOREP_SUBSTRATES_DYNAMIC_FILTERING_METHOD.\n" );
        exit( EXIT_FAILURE );
    }

    if( strcmp( env_str, "absolute" ) == 0 )
    {
        filtering_absolute = true;
    }
    else
    {
        filtering_absolute = false;
    }

    return 0;
}

/**
 * Gets the internal Score-P id for this plugin.
 *
 * The id is needed in order to give a proper return value in the finalizing method.
 *
 * @param   s_id                            Score-Ps internal id for this plugin.
 */
static void assign( size_t                                                          s_id )
{
    id = s_id;
}

#ifdef DYNAMIC_FILTERING_DEBUG
/**
 * Debug output at the end of the program.
 *
 * Only used if the plugin has been built with -DBUILD_DEBUG=on.
 */
static void on_write_data( void )
{
    fprintf( stderr, "\n\nFinalizing.\n\n\n" );
    fprintf( stderr, "Global mean duration: %f\n\n", mean_duration );
    fprintf( stderr, "|                  Region Name                  "
                     "| Region handle "
                     "| Call count "
                     "|        Duration        "
                     "|   Mean duration  "
                     "|   Status  |\n" );
    region_info *current, *tmp;

    HASH_ITER( hh, regions, current, tmp )
    {
        fprintf( stderr, "| %-45s | %13d | %10lu | %22lu | %16f | %-9s |\n",
                    current->region_name,
                    current->region_handle,
                    current->call_cnt,
                    current->duration,
                    current->mean_duration,
                    current->deletable ? ( current->inactive ? "deleted" : "deletable" ) : " " );
    }
}
#endif

/**
 * Finalizing method.
 *
 * Used for cleanup and writing the filter file.
 */
static size_t finalize( void )
{
    char filename[128], backup[128];
    sprintf( filename, "df-filter.list.%d", getpid( ) );
    sprintf( backup, "%s.old", filename );

    int fd = open( filename, O_CREAT | O_WRONLY | O_EXCL, S_IRUSR | S_IWUSR );
    if( fd < 0 && errno == EEXIST )
    {
        // File could not be created because it already exists. Let's move it as a backup.
        rename( filename, backup );
        fd = open( filename, O_CREAT | O_WRONLY | O_EXCL, S_IRUSR | S_IWUSR );
    }

    if( fd > 0 )
    {
        // File could be created. Write a Score-P filter file to it.
        FILE* fp = fdopen( fd, "w" );

        fprintf( fp, "SCOREP_REGION_NAMES_BEGIN\n" );

        region_info *current, *tmp;
        bool first = true;

        HASH_ITER( hh, regions, current, tmp )
        {
            if( current->inactive )
            {
                if( first )
                {
                    fprintf( fp, "EXCLUDE %s\n", current->region_name );
                    first = false;
                }
                else
                {
                    fprintf( fp, "        %s\n", current->region_name );
                }
            }

            HASH_DEL( regions, current );
            free( current );
        }

        regions = NULL;

        fprintf( fp, "SCOREP_REGION_NAMES_END\n" );
        fclose( fp );
    }
    else
    {
        // File still could not be created. Dump an error message.
        fprintf( stderr, "Couldn't create filter list. Please check if you have write "
                         "permissions for the current working directory.\n" );

        region_info *current, *tmp;

        HASH_ITER( hh, regions, current, tmp )
        {
            HASH_DEL( regions, current );
            free( current );
        }

        regions = NULL;
    }

    return id;
}

/**
 * Defines callbacks for events.
 *
 * Defines callbacks for all events that are handled by this plugin.
 *
 * @param   mode                            unused
 * @param   functions                       Struct containing all available events.
 */
static uint32_t event_functions( __attribute__((unused)) SCOREP_Substrates_Mode     mode,
                                 SCOREP_Substrates_Callback**                       functions )
{
    SCOREP_Substrates_Callback* ret = calloc( SCOREP_SUBSTRATES_NUM_EVENTS,
                                                    sizeof( SCOREP_Substrates_Callback ) );

    ret[SCOREP_EVENT_ENTER_REGION] = (SCOREP_Substrates_Callback) on_enter_region;
    ret[SCOREP_EVENT_EXIT_REGION]  = (SCOREP_Substrates_Callback) on_exit_region;
    ret[SCOREP_EVENT_THREAD_FORK_JOIN_TEAM_BEGIN]  = (SCOREP_Substrates_Callback) on_team_begin;
    ret[SCOREP_EVENT_THREAD_FORK_JOIN_TEAM_END]    = (SCOREP_Substrates_Callback) on_team_end;
    ret[SCOREP_EVENT_THREAD_FORK_JOIN_JOIN]        = (SCOREP_Substrates_Callback) on_join;

    *functions = ret;
    return SCOREP_SUBSTRATES_NUM_EVENTS;
}

/**
 * Gets the callbacks for information retrieval about handles.
 *
 * Just stores the callbacks used by this plugin for later use.
 *
 * @param   callbacks                       The callbacks to be stored.
 * @param   size                            The size of the struct containing the callbacks.
 *                                          (unused)
 */
void set_callbacks( SCOREP_SubstrateCallbacks                                       callbacks,
                    __attribute__((unused)) size_t                                  size )
{
    get_region_name         = callbacks.SCOREP_RegionHandle_GetName;
    get_paradigm_type       = callbacks.SCOREP_RegionHandle_GetParadigmType;
    get_location_id         = callbacks.SCOREP_Location_GetId;
}

/**
 * Registers the plugin in Score-Ps interface.
 *
 * Sets management callbacks as well as the standard plugin version.
 */
SCOREP_SUBSTRATE_PLUGIN_ENTRY( dynamic_filtering_plugin )
{
    SCOREP_Substrate_Plugin_Info info = { 0 };

    info.early_init             = init;
    info.assign_id              = assign;
    info.finalize               = finalize;
    info.define_handle          = on_define_region;
    info.create_location        = on_create_location;
    info.delete_location        = on_delete_location;
#ifdef DYNAMIC_FILTERING_DEBUG
    info.write_data             = on_write_data;
#endif
    info.get_event_functions    = event_functions;
    info.set_callbacks          = set_callbacks;

    info.plugin_version         = SCOREP_SUBSTRATE_PLUGIN_VERSION;

    return info;
}
