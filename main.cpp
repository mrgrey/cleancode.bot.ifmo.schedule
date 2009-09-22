/*
 * File:   main.cpp
 * Author: mrgrey, thorn (cleancode)
 *
 * Created on 1 Сентябрь 2009 г., 0:02
 */

#include <stdlib.h>
#include <glib-2.0/glib.h>
#include <libpurple/purple.h>
#include <curl/curl.h>
#include <iconv.h>
#include <getopt.h>

#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
//#include <math.h>

// <editor-fold defaultstate="collapsed" desc="define section">
#define CUSTOM_USER_DIRECTORY  "/dev/null"
#define CUSTOM_PLUGIN_PATH     ""
#define PLUGIN_SAVE_PREF       "/purple/nullclient/plugins/saved"
#define UI_ID                  "cleancode.bot.ifmo.schedule"


#define PURPLE_GLIB_READ_COND  (G_IO_IN | G_IO_HUP | G_IO_ERR)
#define PURPLE_GLIB_WRITE_COND (G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL)
// </editor-fold>

#define COMMANDS_TABLE_INIT() commands_table = g_hash_table_new(g_str_hash, g_str_equal);
#define COMMANDS_TABLE_ENTRY(cmd,func) g_hash_table_insert(commands_table, (void*)cmd, (void*)func);

#define STATUS_AVAILABLE 0
#define STATUS_OFFLINE 1
#define STATUS_AWAY 2
#define STATUS_MAX_ID STATUS_AWAY

#define INIT_STATUS(our_code, purple_code) 	status_types[our_code] = purple_status_type_new(purple_code, NULL, NULL, true);\
								statuses[our_code] = purple_status_new(status_types[our_code], presence);
#define STATUS_ID(our_code) purple_status_get_id(statuses[our_code])

char uin[10];
char password[30];
char datasource_url[255];
char datasource_url_params[255] = "";


int requests_schedule_count = 0;

static PurplePlugin *icqPlugin;
static PurplePluginInfo *icqPluginInfo;
static PurplePresence *presence;
static PurpleStatusType* status_types[STATUS_MAX_ID + 1];
static PurpleStatus* statuses[STATUS_MAX_ID + 1];

static PurpleAccount *globalAccount;

#define LOG_CATEGORY_NONE 0

#define LOG_CATEGORY_GENERAL 0x1
#define LOG_CATEGORY_FUNC_CALL 0x2
#define LOG_CATEGORY_INCOMING 0x4

#define LOG_CATEGORY_ALL 0xFFFFFFFF

#define LOG_BUFFER_LENGTH 200

#define LOG_DIRECTION_CONSOLE 1
#define LOG_DIRECTION_FILE 2

#define MAX_PATH 255

static unsigned int LogCategories = LOG_CATEGORY_GENERAL;
static unsigned int log_directions = 0;

static FILE* log_file = NULL;
static char log_file_name[MAX_PATH];

int log2(int number){
	int result = 0;
	while(number > 1){
		number = number>>1;
		result++;
	}	
	return result;
}


int weekday(int day, int month, int year)
{
	int ix, tx, vx;
	
	switch (month) {
		case 2 :
		case 6 : vx = 0; break;
		case 8 : vx = 4; break;
		case 10 : vx = 8; break;
		case 9 :
		case 12 : vx = 12; break;
		case 3 :
		case 11 : vx = 16; break;
		case 1 :
		case 5 : vx = 20; break;
		case 4 :
		case 7 : vx = 24; break;
	}
	
	if (year > 1900) // 1900 was not a leap year
		year -= 1900;
	ix = ((year - 21) % 28) + vx + (month > 2); // take care of February

	tx = (ix + (ix / 4)) % 7 + day; // take care of leap year

	return (tx % 7);
}

const char* log_categories_names[]={
	"- general - ", //LOG_CATEGORY_GENERAL
	"- func call - ", //LOG_CATEGORY_FUNC_CALL
	"- incoming - " //LOG_CATEGORY_INCOMING 
};

bool log_init(const char* new_log_file_name){
	
	if(new_log_file_name ){
		if (log_file = fopen(new_log_file_name, "a")){
			strcpy(log_file_name, new_log_file_name);
			log_directions |= LOG_DIRECTION_FILE;
		}
	}
	else
		log_directions |= LOG_DIRECTION_CONSOLE;
	
	fflush(stdout);
	
	return true;
}

void log_out(unsigned int category, const char *message){
	if(!(category & LogCategories))
		return;
	
	char date[24];

	const time_t now = time(NULL);
	strcpy(date, purple_utf8_strftime("[%d.%m.%Y %H:%M:%S] ", localtime(&now)));
	
	const char* category_name = log_categories_names[log2(category)];
	
	if((log_directions & LOG_DIRECTION_FILE) && log_file){

		fwrite(date, 1, strlen(date),log_file);
		
		fwrite(category_name, 1, strlen(category_name), log_file);
		
		fwrite(message, 1, strlen(message), log_file);
		if(message[strlen(message)-1] != '\n')
			fwrite("\n", 1, 1, log_file);
		fflush(log_file);
	}
	
	if(log_directions & LOG_DIRECTION_CONSOLE){
		printf("%s%s%s", date, category_name, message);
		if(message[strlen(message)-1] != '\n')
			printf("\n");
		fflush(stdout);
	}
}

void log_uninit(){
	if(log_directions & LOG_DIRECTION_FILE){
		fclose(log_file);
	}
}

void wait(int seconds){
	clock_t endwait;
	endwait=clock()+seconds*CLOCKS_PER_SEC;
	while (clock()<endwait);
}

int min(int a, int b){
	return a<b?a:b;
}


static char day_of_week[][23]={
	"Воскресенье",
    "Понедельник",
    "Вторник",
    "Среда",
    "Четверг",
    "Пятница",
    "Суббота",
    "Воскресенье"
};

typedef struct _PurpleGLibIOClosure {
    PurpleInputFunction function;
    guint result;
    gpointer data;
} PurpleGLibIOClosure;

static void purple_glib_io_destroy(gpointer data) {
    g_free(data);
}

static gboolean purple_glib_io_invoke(GIOChannel *source, GIOCondition condition, gpointer data) {
    PurpleGLibIOClosure *closure = (PurpleGLibIOClosure *) data;
    PurpleInputCondition purple_cond = (PurpleInputCondition) 0;

    if (condition & PURPLE_GLIB_READ_COND)
        purple_cond = (PurpleInputCondition) (purple_cond | PURPLE_INPUT_READ);
    if (condition & PURPLE_GLIB_WRITE_COND)
        purple_cond = (PurpleInputCondition) (purple_cond | PURPLE_INPUT_WRITE);

    closure->function(closure->data, g_io_channel_unix_get_fd(source),
            purple_cond);

    return TRUE;
}

static guint glib_input_add(gint fd, PurpleInputCondition condition, PurpleInputFunction function,
        gpointer data) {
    PurpleGLibIOClosure *closure = g_new0(PurpleGLibIOClosure, 1);
    GIOChannel *channel;
    GIOCondition cond = (GIOCondition) 0;

    closure->function = function;
    closure->data = data;

    if (condition & PURPLE_INPUT_READ)
        cond = (GIOCondition) (cond | PURPLE_GLIB_READ_COND);
    if (condition & PURPLE_INPUT_WRITE)
        cond = (GIOCondition) (cond | PURPLE_GLIB_WRITE_COND);

    channel = g_io_channel_unix_new(fd);
    closure->result = g_io_add_watch_full(channel, G_PRIORITY_DEFAULT, cond,
            purple_glib_io_invoke, closure, purple_glib_io_destroy);

    g_io_channel_unref(channel);
    return closure->result;
}

static PurpleEventLoopUiOps glib_eventloops ={
    g_timeout_add,
    g_source_remove,
    glib_input_add,
    g_source_remove,
    NULL,
#if GLIB_CHECK_VERSION(2,14,0)
    g_timeout_add_seconds,
#else
    NULL,
#endif

    /* padding */
    NULL,
    NULL,
    NULL
};
/*** End of the eventloop functions. ***/

static char * get_answer(const char *command, char *answer);

/*** Conversation uiops ***/
// <editor-fold defaultstate="collapsed" desc="conversation uiops">

static void write_conv(PurpleConversation *conv, const char *who, const char *alias,
        const char *message, PurpleMessageFlags flags, time_t mtime) {
	
		
    if (!(flags & PURPLE_MESSAGE_RECV))
        return;
		
	log_out(LOG_CATEGORY_FUNC_CALL, "write_conv() called");

    const char *name;
    if (alias && *alias)
        name = alias;
    else if (who && *who)
        name = who;
    else
        name = NULL;

	const char* conversation_name = purple_conversation_get_name(conv);
		
    /*printf("(%s) %s %s: %s\n", conversation_name,
            purple_utf8_strftime("(%H:%M:%S)", localtime(&mtime)),
            name, message);
	*/	
	
	//бредокод, работает
	//LOG VERY BIG CODE PART BEGIN============================================
	char log_buffer[LOG_BUFFER_LENGTH];
	strcpy(log_buffer, conversation_name);
	strcat(log_buffer, ": \""); //3

	int max_message_to_copy = LOG_BUFFER_LENGTH - strlen(conversation_name) - 5;
	
	int length_to_copy = max_message_to_copy <= strlen(message) ? max_message_to_copy : strlen(message);
	
	memcpy(log_buffer + strlen(conversation_name) + 3, message, length_to_copy);
	
	log_buffer[strlen(conversation_name) + 3 + length_to_copy] = 0;
	
	strcat(log_buffer, "\""); //1
	log_buffer[strlen(log_buffer)] = 0;
	
	log_out(LOG_CATEGORY_INCOMING, log_buffer);
	//LOG VERY BIG CODE PART END ============================================
	
    char send_message[5120];
    get_answer(message, &send_message[0]);
    
    purple_conv_im_send(PURPLE_CONV_IM(conv), send_message);
	
	log_out(LOG_CATEGORY_FUNC_CALL, "write_conv() exited");
}

static PurpleConversationUiOps conv_uiops = {
    NULL, /* create_conversation  */
    NULL, /* destroy_conversation */
    NULL, /* write_chat           */
    NULL, /* write_im             */
    write_conv, /* write_conv           */
    NULL, /* chat_add_users       */
    NULL, /* chat_rename_user     */
    NULL, /* chat_remove_users    */
    NULL, /* chat_update_user     */
    NULL, /* present              */
    NULL, /* has_focus            */
    NULL, /* custom_smiley_add    */
    NULL, /* custom_smiley_write  */
    NULL, /* custom_smiley_close  */
    NULL, /* send_confirm         */
    NULL,
    NULL,
    NULL,
    NULL
};

static void ui_init(void) {
    purple_conversations_set_ui_ops(&conv_uiops);
}// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="libpurple init">
static PurpleCoreUiOps core_uiops = {
    NULL,
    NULL,
    ui_init,
    NULL,

    /* padding */
    NULL,
    NULL,
    NULL,
    NULL
};

static void init_libpurple(void) {
    /* Set a custom user directory (optional) */
    purple_util_set_user_dir(CUSTOM_USER_DIRECTORY);

    /* We do not want any debugging for now to keep the noise to a minimum. */
    purple_debug_set_enabled(FALSE);

    /* Set the core-uiops, which is used to
     * 	- initialize the ui specific preferences.
     * 	- initialize the debug ui.
     * 	- initialize the ui components for all the modules.
     * 	- uninitialize the ui components for all the modules when the core terminates.
     */
    purple_core_set_ui_ops(&core_uiops);

    /* Set the uiops for the eventloop. If your client is glib-based, you can safely
     * copy this verbatim. */
    purple_eventloop_set_ui_ops(&glib_eventloops);

    /* Set path to search for plugins. The core (libpurple) takes care of loading the
     * core-plugins, which includes the protocol-plugins. So it is not essential to add
     * any path here, but it might be desired, especially for ui-specific plugins. */
    purple_plugins_add_search_path(CUSTOM_PLUGIN_PATH);

    /* Now that all the essential stuff has been set, let's try to init the core. It's
     * necessary to provide a non-NULL name for the current ui to the core. This name
     * is used by stuff that depends on this ui, for example the ui-specific plugins. */
    if (!purple_core_init(UI_ID)) {
        /* Initializing the core failed. Terminate. */
        fprintf(stderr,
                "libpurple initialization failed. Dumping core.\n"
                "Please report this!\n");
        abort();
    }

    /* Create and load the buddylist. */
    purple_set_blist(purple_blist_new());
    purple_blist_load();

    /* Load the preferences. */
    purple_prefs_load();

    /* Load the desired plugins. The client should save the list of loaded plugins in
     * the preferences using purple_plugins_save_loaded(PLUGIN_SAVE_PREF) */
    purple_plugins_load_saved(PLUGIN_SAVE_PREF);

    /* Load the pounces. */
    purple_pounces_load();
}// </editor-fold>

static void signed_on(PurpleConnection *gc, gpointer null) {
    PurpleAccount *account = purple_connection_get_account(gc);
	
	char buffer[200];
	sprintf(buffer,"Account connected: %s %s\n", account->username, account->protocol_id);
	
	log_out(LOG_CATEGORY_GENERAL, buffer);
}

static void signed_off(PurpleConnection *gc, gpointer null) {
    PurpleAccount *account = purple_connection_get_account(gc);
	
	char buffer[200];
	sprintf(buffer,"Account disconnected: %s %s\n", account->username, account->protocol_id);
	
	log_out(LOG_CATEGORY_GENERAL, buffer);
}

static void connect_to_signals(void) {
    static int handle;
    purple_signal_connect(purple_connections_get_handle(), "signed-on", &handle,
            PURPLE_CALLBACK(signed_on), NULL);
     purple_signal_connect(purple_connections_get_handle(), "signed-off", &handle,
            PURPLE_CALLBACK(signed_off), NULL);

}

typedef struct{
    void *buffer;
    int offset;
}write_data_buffer;

static size_t write_data(char *buffer, size_t size, size_t nmemb, write_data_buffer *data_buffer) {
    strcpy((char *)(data_buffer->buffer)+(data_buffer->offset), buffer);
    data_buffer->offset += strlen(buffer);
    return size*nmemb;
}

static char * get_schedule_json(const char* group_id, char *buffer, time_t date = 0) {
	log_out(LOG_CATEGORY_FUNC_CALL, "get_schedule_json() called");
    char* startptr = buffer;
        write_data_buffer data_buffer;
        data_buffer.buffer = buffer;
        data_buffer.offset = 0;

    CURL *curl;
    CURLcode res;
	char dateStr[11];
	struct tm *dateInfo;

    if (!(curl = curl_easy_init())) {
		log_out(LOG_CATEGORY_FUNC_CALL, "get_schedule_json() exited");
		return NULL;
	}

	if(!date){
		date = time(NULL);
	}
	dateInfo = localtime(&date);
	strftime(dateStr, 11, "%d.%m.%Y", dateInfo);
	char url[512];
	sprintf(&url[0], "%s?gr=%s&date=%s", datasource_url, group_id, dateStr);
	
	if(datasource_url_params[0]){
		strcat(url, "&");
		strcat(url, datasource_url_params);
	}
	
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data_buffer);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
	
	res = curl_easy_perform(curl);
	long response_code;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
	
	if(res){
		//something goes wrong
		buffer = NULL;
	} else {
		//everything is ok
		buffer = (char *)data_buffer.buffer;
	}
	
	curl_easy_cleanup(curl);
	log_out(LOG_CATEGORY_FUNC_CALL, "get_schedule_json() exited");
	
    return startptr;
}

static char * decode_utf_literals(const char *json_data, char *buffer) {
	log_out(LOG_CATEGORY_FUNC_CALL, "decode_utf_literals() called");
	
    char *startptr = buffer;
    char tdata;
    int utf_symbol;
    char *curptr = (char *)json_data;
	char t_converted_data[4];
    while(*curptr != '\0') {
        if(*curptr == '\\' && *(curptr+1) == 'u') {
            tdata = *(curptr+6);
            *(curptr+6) = '\\';
            utf_symbol = (guint32)strtol(curptr+2, NULL, 16);

            //Convert character from utf
            if (utf_symbol <= 0x7F) {
                *buffer = (unsigned char) utf_symbol;
				buffer++;
            } else if (utf_symbol <= 0x7FF) {
				t_converted_data[1] = 0x80 | (utf_symbol & 0x3F);
				t_converted_data[0] = 0xC0 | ((utf_symbol >> 6) & 0x1F);
				memcpy(buffer, t_converted_data, 2);
				buffer += 2;
			} else if (utf_symbol <= 0xFFFF) {
				t_converted_data[2] = 0x80 | (utf_symbol & 0x3F);
				t_converted_data[1] = 0x80 | ((utf_symbol >> 6) & 0x3F);
				t_converted_data[0] = 0xE0 | ((utf_symbol >> 12) & 0xF);
				memcpy(buffer, t_converted_data, 3);
				buffer += 3;
			} else if (utf_symbol <= 0x10FFFF ) {
				t_converted_data[3] = 0x80 | (utf_symbol & 0x3F);
				t_converted_data[2] = 0x80 | ((utf_symbol >> 6) & 0x3F);
				t_converted_data[1] = 0x80 | ((utf_symbol >> 12) & 0x3F);
				t_converted_data[0] = 0xF0 | ((utf_symbol >> 18) & 0x7);
				memcpy(buffer, t_converted_data, 4);
				buffer += 4;
            } else {
                *buffer = '?';
				buffer++;
            }

            *(curptr+6) = tdata;
            curptr += 6;
        } else {
            *(buffer++) = *(curptr++);
        }
        *buffer = 0;
    }    return startptr;
	
	log_out(LOG_CATEGORY_FUNC_CALL, "decode_utf_literals() exited");
}

typedef struct{
    char time[60];
    char place[60];
    char subject[5120];
    char person_name[100];
} pair;

typedef struct {
    int week_number;
    char day[23];
    char group_id[11];
    pair lessons[8];
} data;

static int parse_json(const char *json_data, data *data) {
	log_out(LOG_CATEGORY_FUNC_CALL, "parse_json() called");

    char *startptr, *endptr, statusSymbol;
    startptr = strstr(json_data, "status");
	statusSymbol = startptr[9];
	if(statusSymbol == 'w')
	{
        log_out(LOG_CATEGORY_FUNC_CALL, "parse_json() exited");
		return -1;
	}
	
	
    startptr = strstr(json_data, "week_number");
    data->week_number = (int)strtol(startptr+13,NULL,10);
    startptr = strstr(startptr, "day");
    strcpy(data->day, day_of_week[(int)strtol(startptr+6,NULL,10)]);
	
    startptr = strstr(startptr, "group");
	startptr += 8;
	endptr = strstr(startptr, "\"");
	memcpy(data->group_id, startptr, endptr - startptr);
	
	if(statusSymbol == 'n')
	{
            log_out(LOG_CATEGORY_FUNC_CALL, "parse_json() exited");
		return 0;
	}

    for(int i = 0; i < 8; i++) {
        //time
        startptr = strstr(startptr, "time");
        if( startptr == NULL ) {
            return i+1;
        }
        endptr = strstr(startptr, "place");
        memcpy(data->lessons[i].time, startptr+7, endptr-startptr-10);
        data->lessons[i].time[endptr-startptr-10] = 0;
        //place
        startptr = endptr;
        endptr = strstr(startptr, "subject");
        memcpy(data->lessons[i].place, startptr+8, endptr-startptr-11);
        data->lessons[i].place[endptr-startptr-11] = 0;
        //subject
        startptr = endptr;
        endptr = strstr(startptr, "person_name");
        memcpy(data->lessons[i].subject, startptr+10, endptr-startptr-13);
        data->lessons[i].subject[endptr-startptr-13] = 0;
        //person_name
        startptr = endptr;
        endptr = strstr(startptr, "}");
        memcpy(data->lessons[i].person_name, startptr+14, endptr-startptr-15);
        data->lessons[i].person_name[endptr-startptr-15] = 0;
    }

        log_out(LOG_CATEGORY_FUNC_CALL, "parse_json() exited");
    return 8;
}

typedef char (*COMMAND_HANDLER)(const char* command, char* answer);

static char* date_time(const char *command, char *answer){
    char* answer_start_ptr = answer;
	time_t rawtime;
	time(&rawtime);
	struct tm *timeinfo = localtime(&rawtime);
	sprintf(answer, "Date and time: %s", asctime(timeinfo));
	return answer_start_ptr;
}

static char* version(const char *command, char *answer){
    char* answer_start_ptr = answer;
	strcpy(answer, "Версия: 0.2.9, билд от 21.09.2009");
	return answer_start_ptr;
}

static char* schedule(const char *command, char *answer){
		//DEBUG LOG OUTPUT
		log_out(LOG_CATEGORY_FUNC_CALL, "schedule() called");

    char* answer_start_ptr = answer;

		/*
			New version supports ONLY base syntax:
			schedule <group_id, required> [date spec, optional]
		*/
		
		requests_schedule_count++;

		char *curcharptr = (char *)command;
		
		const int max_group_id_length = 10;
		char groupId[max_group_id_length + 1] = "";
		
        time_t date;
        time(&date);
        struct tm *dateinfo = localtime(&date);
		
        gboolean is_tomorrow = FALSE;
		
		curcharptr = strstr((const char *)curcharptr, " "); //need be checked for 0
		if(curcharptr){
			curcharptr++;
		
			int cmd_group_id_length = strlen(curcharptr) < max_group_id_length ? strlen(curcharptr) : max_group_id_length;
			
			char *endcharptr = strstr((const char *)curcharptr, " ");
			
			if(endcharptr && (curcharptr + cmd_group_id_length > endcharptr))
				cmd_group_id_length = endcharptr - curcharptr;
				
			memcpy(groupId, curcharptr, cmd_group_id_length);
			groupId[cmd_group_id_length] = 0;
			
			if(endcharptr){
			
				//curcharptr = endcharptr + 1;
				
				while(*curcharptr == ' '){
					curcharptr++;
				}
				
				if(*curcharptr){
					if((strstr((const char *)curcharptr, "tomorrow") || strstr((const char *)curcharptr, "на завтра"))){
						date += 24 * 3600;
					}else if((strstr((const char *)curcharptr, "for ") || strstr((const char *)curcharptr, "на "))){
						curcharptr = strstr((const char *)curcharptr, " ") + 1;
						
						int today_day_id = weekday(dateinfo->tm_mday, dateinfo->tm_mon + 1, dateinfo->tm_year + 1900);
						int day_id = -1;
						
						if((strcmp((const char *)curcharptr, "monday") == 0 || strcmp((const char *)curcharptr, "понедельник") == 0)){
							day_id = 0;
						}else if((strcmp((const char *)curcharptr, "tuesday") == 0 || strcmp((const char *)curcharptr, "вторник") == 0)){
							day_id = 1;
						}else if((strcmp((const char *)curcharptr, "wednesday") == 0 || strcmp((const char *)curcharptr, "среду") == 0)){
							day_id = 2;
						}else if((strcmp((const char *)curcharptr, "thursday") == 0 || strcmp((const char *)curcharptr, "четверг") == 0)){
							day_id = 3;
						}else if((strcmp((const char *)curcharptr, "friday") == 0 || strcmp((const char *)curcharptr, "пятницу") == 0)){
							day_id = 4;
						}else if((strcmp((const char *)curcharptr, "saturday") == 0 || strcmp((const char *)curcharptr, "субботу") == 0)){
							day_id = 5;
						}else if((strcmp((const char *)curcharptr, "sunday") == 0 || strcmp((const char *)curcharptr, "воскресенье") == 0)){
							day_id = 6;
						};
						
						if(day_id != -1)
						{
							if(day_id<=today_day_id)
								day_id += 7;
							
							date += (day_id - today_day_id) * 24 * 3600;
							
						} else {
							strcpy(answer, "Неизвестный день недели.");
							log_out(LOG_CATEGORY_FUNC_CALL, "schedule() exited");
							return answer;
						}
					} else {
						strcpy(answer, "Некорректный спецификатор даты.");
						log_out(LOG_CATEGORY_FUNC_CALL, "schedule() exited");
						return answer;
					}					
				}
			}
		}
		
		if(!groupId[0]){
			strcpy(answer, "Пожалуйста, укажите номер группы.");
		}else{
		

	        char buffer[5120], out[5120];
	        if(!get_schedule_json(groupId, &buffer[0], date)){
				strcpy(answer, "Ошибка получения данных. Пожалуйста, повторите позже.");
			} else {
			
				decode_utf_literals(&buffer[0], &out[0]);
				data data;
				int lessons = parse_json(out, &data);

				if (lessons == -1) {
					strcpy(answer, "Недопустимый номер группы");
				} else if (lessons == 0) {
					sprintf(answer, "%s, %d неделя<br>Группа: %s<br><br>", data.day, data.week_number, data.group_id);
					strcat(answer, "<br>Нет занятий");
				} else {
					sprintf(answer, "%s, %d неделя<br>Группа: %s<br><br>", data.day, data.week_number, data.group_id);
					sprintf(answer, "%s_______________________________<br>", answer);
					for (int i = 0; i < lessons - 1; i++) {
						if (i) {
							sprintf(answer, "%s_______________________________<br>", answer);
						}
						sprintf(answer, "%s%s %s - %s<br>%s<br>", answer, data.lessons[i].time, data.lessons[i].place, data.lessons[i].person_name, data.lessons[i].subject);
						//sprintf(answer, "%s_______________________________<br>", answer);
					}
				}
			}
		}
		
		//DEBUG LOG OUTPUT
		log_out(LOG_CATEGORY_FUNC_CALL, "schedule() exited");
		
		return answer_start_ptr;
}


static char* help(const char *command, char *answer){
    char* answer_start_ptr = answer;
	strcpy(answer,  "Список доступных комманд:<br>"
					"-----------------------------------------------------------<br>"
					"ENG:<br>"
					"schedule 1234 - выводит расписание для группы 1234<br>"
					"s 1234 - аналогично<br>"
					"schedule 1234 tomorrow - выводит расписание на завтра для группы 1234<br>"
					"schedule 1234 for wednesday - выводит расписание на ближайшую среду для группы 1234<br>"
					"link 1234 - выводит ссылку на печатную форму расписания для 1234<br>"
					"version - выводит информацию о версии<br>"
					"help - выводит это сообщение<br>"
					"------------------------------------------------------------<br>"
					"РУС:<br>"
					"расписание 1234 - выводит расписание для группы 1234<br>"
					"р 1234 - аналогично<br>"
					"расписание 1234 на завтра - выводит расписание на завтра для группы 1234<br>"
					"расписание 1234 на среду - выводит расписание на ближайшую среду для группы 1234<br>"
					"ссылка 1234 - выводит ссылку на печатную форму расписания для группы 1234<br>"
					"версия - выводит информацию о версии<br>"
					"помощь - выводит это сообщение<br>"
					"------------------------------------------------------------<br>"
					"Вы можете связаться с разработчиками по e-mail:<br>"
					" denis.bykov@cleancode.ru<br>"
					" chuyko.yury@cleancode.ru"
					);
	return answer_start_ptr;
}

static char* stat(const char *command, char *answer){
    char* answer_start_ptr = answer;
	sprintf(answer, "С запуска %d запросов расписания", requests_schedule_count);
	return answer_start_ptr;
}

static char* show_log_tail(const char *command, char *answer){
    char* answer_start_ptr = answer;
	if(!log_file_name[0]){
		strcpy(answer, "Файловое логирование отключено");
	}else{
		char inbuf[BUFSIZ];
		char cmd[MAX_PATH + 12];
		sprintf(cmd, "tail -n 20 %s", log_file_name);

		char *answer_position = answer;
		
		FILE *cmd_out_stream;
		answer[0]=0;
		if ((cmd_out_stream = popen(cmd, "r")) != NULL)
			while (fgets(inbuf, BUFSIZ, cmd_out_stream) != NULL){
				for(int e = 0; e<strlen(inbuf); e++){
					if(inbuf[e] == '\n'){
						memcpy(answer_position, "<br>", 4);
						answer_position += 4;
					}else{
						*(answer_position++) = inbuf[e];
					}
				}
			}
		*answer_position = 0;

		pclose(cmd_out_stream);
	}
	return answer_start_ptr;
}

static char* show_schedule_link(const char *command, char *answer){
    char* answer_start_ptr = answer;
    int groupNumber = 0;
    char* wsptr = strstr(command, " ");
    if(wsptr != 0) {
        groupNumber = atoi(wsptr+1);
        sprintf(answer, "Ссылка на печатную форму расписания: http://www.ifmo.ru/file/schedule.php?gr=%d", groupNumber);
    }
    if(groupNumber == 0) {
        sprintf(answer, "Неверно задан номер группы!");
    }
    return answer_start_ptr;
}

GHashTable *commands_table = NULL;

static void init_commands_table()
{
	if(commands_table)
		return;
	
	COMMANDS_TABLE_INIT();
	
	COMMANDS_TABLE_ENTRY("date", date_time);
	COMMANDS_TABLE_ENTRY("дата", date_time);
	COMMANDS_TABLE_ENTRY("time", date_time);
	COMMANDS_TABLE_ENTRY("время", date_time);
	
	COMMANDS_TABLE_ENTRY("version", version);
	COMMANDS_TABLE_ENTRY("версия", version);
	
	COMMANDS_TABLE_ENTRY("schedule", schedule);
	COMMANDS_TABLE_ENTRY("расписание", schedule);
	COMMANDS_TABLE_ENTRY("s", schedule);
	COMMANDS_TABLE_ENTRY("р", schedule);
	
	COMMANDS_TABLE_ENTRY("help", help);
	COMMANDS_TABLE_ENTRY("помощь", help);
	COMMANDS_TABLE_ENTRY("!help", help);
	COMMANDS_TABLE_ENTRY("?", help);
	
	COMMANDS_TABLE_ENTRY("stat", stat);
	COMMANDS_TABLE_ENTRY("стат", stat);
	
	COMMANDS_TABLE_ENTRY("лог", show_log_tail);
	COMMANDS_TABLE_ENTRY("log", show_log_tail);

	COMMANDS_TABLE_ENTRY("ссылка", show_schedule_link);
	COMMANDS_TABLE_ENTRY("link", show_schedule_link);
}

static char * short_group_name_parse(const char *command, char *prepared_command) {
    //command is already trimmed
    char *start_prepared_command_ptr = prepared_command;
    char* last_part = strstr(command, " ");
    int group_num = atoi(command);
    if((strlen(command) == 4 || strlen(command) == 3) && group_num != 0) { //IFMO or PGUPS detected
        if(last_part) {
            sprintf(prepared_command, "schedule %d%s", group_num, last_part);
        } else {
            sprintf(prepared_command, "schedule %d", group_num);
        }
        return start_prepared_command_ptr;
    } else if(*command == 'i' && *(command+1) == 'd') { //SZIP detected
        group_num = atoi(command+2);
        if(group_num != 0) {
            if(last_part) {
                sprintf(prepared_command, "schedule id%d%s", group_num, last_part);
            } else {
                sprintf(prepared_command, "schedule id%d", group_num);
            }
            return start_prepared_command_ptr;
        }
    }
    strcpy(prepared_command, command);
    return start_prepared_command_ptr;
}

static char * get_answer(const char *command, char *answer) {
	//DEBUG LOG OUTPUT
	log_out(LOG_CATEGORY_FUNC_CALL, "get_answer() called");

        char* answer_start_ptr = answer;

	while(*command == ' ')
		command++;

        char buffer[5120];
        short_group_name_parse(command, &buffer[0]);
		
	char* cmd_end = strstr(&buffer[0], " ");
	
	//security threat, possible buffer overflow
	char cmd[30];
	int cmdLength;
	
	cmdLength = (cmd_end) ? (cmd_end - &buffer[0]) : strlen(&buffer[0]);
	cmdLength = min(cmdLength, 29);
	
	
	
	memcpy(cmd, &buffer[0], cmdLength);
	
	cmd[cmdLength] = 0;
	const char* normalized_command = purple_normalize_nocase(globalAccount, cmd);
	
	gpointer func = g_hash_table_lookup(commands_table, normalized_command);
	if(!func){
		strcpy(answer,  "Неизвестная команда. Возможно, Вы опечатались.<br>"
						"Попробуйте ввести help или помощь, чтобы просмотреть краткую информацию по доступным командам."
		);
	} else {
		((COMMAND_HANDLER)func)(&buffer[0], answer);
	}
	
	log_out(LOG_CATEGORY_FUNC_CALL, "get_answer() exited");
	
    return answer_start_ptr;
}

void show_usage(){
	printf("\nUsage: main --icq.login <uin> --icq.pass <pass> --datasource.url <url>\n");
}

void quit_handler(int sig){
	printf("Ctrl+C received. See u!\n");
	exit(0);
}

void init_account(){
	globalAccount = purple_account_new(&uin[0], icqPluginInfo->id);
	
    purple_account_set_password(globalAccount, &password[0]);
    purple_account_set_enabled(globalAccount, UI_ID, TRUE);

    purple_account_set_bool(globalAccount, "authorization", FALSE);
	
	presence = purple_presence_new_for_account(globalAccount);
	
	INIT_STATUS(STATUS_AVAILABLE, PURPLE_STATUS_AVAILABLE);
	INIT_STATUS(STATUS_OFFLINE, PURPLE_STATUS_OFFLINE);
	INIT_STATUS(STATUS_AWAY, PURPLE_STATUS_AWAY);	
	
	//purple_presence_set_status_active(presence, purple_status_get_id(awayStatus), TRUE);
	
	purple_account_set_status(globalAccount, STATUS_ID(STATUS_AVAILABLE), TRUE, 0);	
}

/*void destroy_account(){
	//TODO
	//purple_account_destroy(globalAccount);
}*/

int main(int argc, char *argv[]) {
    //char uin[10] = "573869459";
    //char uin[10] = "595266840";
    

    const int optIcgLogin = 0x1;
    const int optIcgPass = 0x2;
    const int optLogFile = 0x4;
    const int optLogConsole = 0x8;
    const int optLogLevel = 0xF;
	const int optDatasourceUrl = 0x10;
	const int optDatasourceUrlParams = 0x20;
	
	
	int required_options = optIcgLogin | optIcgPass | optDatasourceUrl;

    static struct option long_options[] = {
        {"icq.login", required_argument, 0, optIcgLogin},
        {"icq.pass", required_argument, 0, optIcgPass},
        {"log.file", required_argument, 0, optLogFile},
        {"log.console", no_argument, 0, optLogConsole},
        {"log.level", required_argument, 0, optLogLevel},
		{"datasource.url", required_argument, 0, optDatasourceUrl},
		{"datasource.url.params", required_argument, 0, optDatasourceUrlParams}
    };

    int option_index = 0;

    int c = -1;
	
    while ((c = getopt_long(argc, argv, "", long_options, &option_index)) != -1) {
        switch (c) {
            case optIcgLogin:
                strcpy(&uin[0], optarg);
				required_options &= ~optIcgLogin;
                break;
            case optIcgPass:
                strcpy(&password[0], optarg);
				required_options &= ~optIcgPass;
                break;
            case optLogFile:
                log_init(optarg);
                break;
            case optLogConsole:
                log_init(NULL);
                break;
            case optLogLevel:
                if(strstr(optarg, "a")) {
                    LogCategories |= LOG_CATEGORY_ALL;
                } else if(strstr(optarg, "n")) {
                    LogCategories &= LOG_CATEGORY_NONE;
                } else {
                    if(strstr(optarg, "f")) {
                        LogCategories |= LOG_CATEGORY_FUNC_CALL;
                    }
                    if(strstr(optarg, "i")) {
                        LogCategories |= LOG_CATEGORY_INCOMING;
                    }
                    if(strstr(optarg, "g")) {
                        LogCategories |= LOG_CATEGORY_GENERAL;
                    }
                }
                break;
			case optDatasourceUrl:
				strcpy(datasource_url, optarg);
				required_options &= ~optDatasourceUrl;
				break;
			case optDatasourceUrlParams:
				strcpy(datasource_url_params, optarg);
				break;
        }
    }
	
	if(required_options){
		show_usage();
		exit(0);
	}

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    signal(SIGCHLD, SIG_IGN);
	signal(SIGINT, quit_handler);

    init_libpurple();

	log_out(LOG_CATEGORY_GENERAL, "libpurple initialized");

    icqPlugin = purple_plugins_find_with_name("ICQ");
    icqPluginInfo = icqPlugin->info;

	init_account();
	
    connect_to_signals();
	init_commands_table();
    g_main_loop_run(loop);

	log_uninit();
    return 0;
}
